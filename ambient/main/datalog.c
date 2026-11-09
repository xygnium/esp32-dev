#include "datalog.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "ambient.h"
#include "clock.h"
#include "fault.h"
#include "sd.h"
#include "sensor_util.h"

static const char *TAG = "sd";

#define EVENTS_PATH     SD_MOUNT_POINT "/events.csv"
#define READINGS_HEADER "timestamp,sensor,data_type,value,status\n"
#define EVENTS_HEADER   "timestamp,source,event,detail\n"

#define SENT_PATH       SD_MOUNT_POINT "/sent.txt"
#define SAVE_US         (10 * 60 * 1000000LL)   // how often the send position is saved
#define RAM_QUEUE_LEN   32      // events not on the card, waiting to be sent

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;   // guards s_info
static datalog_info_t s_info;

// Sending: events not written to the card go from a RAM queue. Everything on
// the card is sent from the card: events.csv, then the readings files, oldest
// first. The position (the readings file, and the byte after the last acked
// line in it and in events.csv) is saved in sent.txt every SAVE_US, so after
// a reboot up to SAVE_US of lines are sent again. With no sent.txt (a new
// card) everything on the card is sent.
static QueueHandle_t s_ram;
static char s_cur[24];            // readings file being sent; "" none yet
static long s_cur_off, s_ev_off;  // where the next line to send starts
static long s_next_off;           // after the line datalog_next returned
static enum { FROM_NONE, FROM_RAM, FROM_EVENTS, FROM_READINGS } s_from;
static char s_newest[24];         // readings file last written
static bool s_loaded, s_dirty;
static int64_t s_saved_us;

static void load_pos(void)
{
    sd_lock();
    FILE *f = fopen(SENT_PATH, "r");
    if (f) {
        if (fscanf(f, "%23s %ld %ld", s_cur, &s_cur_off, &s_ev_off) != 3 || s_cur[0] == '-') {
            s_cur[0] = '\0';
            s_cur_off = 0;
        }
        fclose(f);
    }
    sd_unlock();
    s_loaded = true;
}

// Fixed width, so each save overwrites the same bytes.
static void save_pos(void)
{
    char line[48];
    int n = snprintf(line, sizeof line, "%-23s %10ld %10ld\n", s_cur[0] ? s_cur : "-",
                     s_cur_off, s_ev_off);
    sd_lock();
    int fd = open(SENT_PATH, O_WRONLY | O_CREAT, 0644);
    bool ok = fd >= 0 && write(fd, line, n) == n && fsync(fd) == 0;
    if (fd >= 0) {
        close(fd);
    }
    sd_unlock();
    if (!ok) {
        ESP_LOGW(TAG, "can't save the send position: %s", strerror(errno));
    }
    s_saved_us = esp_timer_get_time();
    s_dirty = false;
}

// The line starting at off in path, skipping header lines, into msg as
// "<kind> <line>". Returns the offset after it, or -1 if there's no whole
// line there.
static long read_line(const char *path, long off, const char *kind, char *msg)
{
    char buf[DATALOG_MSG_LEN];
    while (1) {
        sd_lock();
        int fd = open(path, O_RDONLY);
        ssize_t n = fd >= 0 && lseek(fd, off, SEEK_SET) == off ? read(fd, buf, sizeof buf - 1) : -1;
        if (fd >= 0) {
            close(fd);
        }
        sd_unlock();
        if (n <= 0) {
            return -1;
        }
        buf[n] = '\0';
        char *end = strchr(buf, '\n');
        if (!end) {
            return -1;
        }
        *end = '\0';
        off += end - buf + 1;
        if (strncmp(buf, "timestamp,", 10) != 0) {
            snprintf(msg, DATALOG_MSG_LEN, "%s %.150s", kind, buf);
            return off;
        }
    }
}

// The first readings file (YYYY-MM-DD.csv) named after `after`, into out.
static bool next_file(const char *after, char out[24])
{
    bool found = false;
    sd_lock();
    DIR *d = opendir(SD_MOUNT_POINT);
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL) {
        const char *n = e->d_name;
        if (strlen(n) == 14 && n[4] == '-' && n[7] == '-' && strcmp(n + 10, ".csv") == 0 &&
            strcmp(n, after) > 0 && (!found || strcmp(n, out) < 0)) {
            strlcpy(out, n, 24);
            found = true;
        }
    }
    if (d) {
        closedir(d);
    }
    sd_unlock();
    return found;
}

bool datalog_next(char *msg)
{
    s_from = FROM_NONE;
    if (s_ram && xQueuePeek(s_ram, msg, 0) == pdTRUE) {
        s_from = FROM_RAM;
        return true;
    }
    if (!sd_usable()) {
        return false;
    }
    if (!s_loaded) {
        load_pos();
    }
    if ((s_next_off = read_line(EVENTS_PATH, s_ev_off, "event", msg)) >= 0) {
        s_from = FROM_EVENTS;
        return true;
    }
    while (1) {
        if (s_cur[0]) {
            char path[64];
            snprintf(path, sizeof path, SD_MOUNT_POINT "/%s", s_cur);
            if ((s_next_off = read_line(path, s_cur_off, "data", msg)) >= 0) {
                s_from = FROM_READINGS;
                return true;
            }
            if (strcmp(s_cur, s_newest) == 0) {
                return false;             // all sent
            }
        }
        char next[24];
        if (!next_file(s_cur, next)) {
            return false;
        }
        strlcpy(s_cur, next, sizeof s_cur);
        s_cur_off = 0;
        s_dirty = true;
    }
}

void datalog_sent(void)
{
    char msg[DATALOG_MSG_LEN];
    if (s_from == FROM_RAM) {
        xQueueReceive(s_ram, msg, 0);
    } else if (s_from == FROM_EVENTS) {
        s_ev_off = s_next_off;
        s_dirty = true;
    } else if (s_from == FROM_READINGS) {
        s_cur_off = s_next_off;
        s_dirty = true;
    }
    s_from = FROM_NONE;
    portENTER_CRITICAL(&s_mux);
    strlcpy(s_info.sent_file, s_cur, sizeof s_info.sent_file);
    s_info.sent_bytes = (uint32_t)s_cur_off;
    portEXIT_CRITICAL(&s_mux);
    if (s_dirty && esp_timer_get_time() - s_saved_us >= SAVE_US) {
        save_pos();
    }
}

// 2026-11-07T13:35:48.134Z
static void fmt_utc(int64_t us, char *buf, size_t size)
{
    time_t sec = (time_t)(us / 1000000);
    struct tm t;
    gmtime_r(&sec, &t);
    snprintf(buf, size, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", t.tm_year + 1900, t.tm_mon + 1,
             t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, (int)(us % 1000000 / 1000));
}

// Append buf (and the header first, if the file is new or empty), fsync,
// close. Raises sd=write_failed on failure. Returns the file's size after, or
// -1.
static long append(const char *path, const char *header, const char *buf, size_t len)
{
    sd_lock();
    struct stat st;
    bool need_header = stat(path, &st) != 0 || st.st_size == 0;
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    bool ok = fd >= 0;
    if (ok && need_header) {
        ok = write(fd, header, strlen(header)) == (ssize_t)strlen(header);
    }
    ok = ok && write(fd, buf, len) == (ssize_t)len && fsync(fd) == 0;
    long size = ok && fstat(fd, &st) == 0 ? (long)st.st_size : -1;
    if (fd >= 0 && close(fd) != 0) {
        ok = false;
    }
    sd_unlock();
    if (!ok) {
        ESP_LOGE(TAG, "write to %s failed: %s", path, strerror(errno));
        fault_raise(FAULT_SD, "write_failed");
        return -1;
    }
    return size;
}

static bool can_write(void)
{
    return clock_is_set() && sd_usable();
}

void datalog_event(const char *source, const char *event, const char *fmt, ...)
{
    char detail[64], ts[32] = "", row[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof detail, fmt, ap);
    va_end(ap);
    if (clock_is_set()) {
        fmt_utc(sensor_utc_us(), ts, sizeof ts);
    }
    int n = snprintf(row, sizeof row, "%s,%s,%s,%s\n", ts, source, event, detail);
    size_t len = n < (int)sizeof row ? n : sizeof row - 1;
    if (can_write() && append(EVENTS_PATH, EVENTS_HEADER, row, len) >= 0) {
        portENTER_CRITICAL(&s_mux);
        s_info.events++;
        portEXIT_CRITICAL(&s_mux);
    } else {
        // Not on the card: sent from RAM.
        if (!s_ram) {
            s_ram = xQueueCreate(RAM_QUEUE_LEN, DATALOG_MSG_LEN);
        }
        char msg[DATALOG_MSG_LEN];
        snprintf(msg, sizeof msg, "event %.*s", (int)(len - 1), row);   // without the line end
        if (xQueueSend(s_ram, msg, 0) != pdTRUE) {
            ESP_LOGW(TAG, "queue full; not sent: %s", msg);
        }
    }
    ambient_wake();
}

size_t datalog_rows(const sensor_result_t r[SENSOR_COUNT], char *buf, size_t size)
{
    size_t len = 0;
    buf[0] = '\0';
    for (int id = 0; id < SENSOR_COUNT; id++) {
        char ts[32];
        fmt_utc(r[id].utc_us, ts, sizeof ts);
        for (int i = 0; i < sensor_value_count(id); i++) {
            int n;
            if (r[id].err) {
                n = snprintf(buf + len, size - len, "%s,%s,%s,,%s\n", ts, sensor_name(id),
                             sensor_data_type(id, i), r[id].err);
            } else {
                n = snprintf(buf + len, size - len, "%s,%s,%s,%.*f,ok\n", ts, sensor_name(id),
                             sensor_data_type(id, i), sensor_decimals(id, i), r[id].value[i]);
            }
            if (n > 0 && (size_t)n < size - len) {
                len += n;
            }
        }
    }
    return len;
}

void datalog_sample(const sensor_result_t r[SENSOR_COUNT])
{
    if (!can_write()) {
        return;
    }
    char buf[512];
    size_t len = datalog_rows(r, buf, sizeof buf);

    // One file per UTC day: YYYY-MM-DD.csv
    char name[40], path[64];
    time_t sec = (time_t)(r[0].utc_us / 1000000);
    struct tm t;
    gmtime_r(&sec, &t);
    snprintf(name, sizeof name, "%04d-%02d-%02d.csv", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
    snprintf(path, sizeof path, SD_MOUNT_POINT "/%s", name);
    long size = append(path, READINGS_HEADER, buf, len);
    if (size < 0) {
        return;
    }
    strlcpy(s_newest, name, sizeof s_newest);
    ambient_wake();
    portENTER_CRITICAL(&s_mux);
    strlcpy(s_info.file, name, sizeof s_info.file);
    s_info.file_bytes = (uint32_t)size;
    s_info.rows += SENSOR_COUNT * SENSOR_MAX_VALUES;
    portEXIT_CRITICAL(&s_mux);
}

void datalog_get_info(datalog_info_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_info;
    portEXIT_CRITICAL(&s_mux);
}

// A bare file name in the root: no path separators, not . or ..
static bool plain_name(const char *name)
{
    return name[0] != '\0' && name[0] != '.' && strchr(name, '/') == NULL && strlen(name) < 64;
}

int datalog_tail(const char *name, int lines)
{
    if (!plain_name(name)) {
        printf("give a file name in the card's root, e.g. events.csv\n");
        return -1;
    }
    if (!sd_usable()) {
        printf("card not usable (not mounted, or an SD fault)\n");
        return -1;
    }
    char path[80];
    snprintf(path, sizeof path, SD_MOUNT_POINT "/%s", name);
    char buf[256];
    sd_lock();
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        sd_unlock();
        printf("%s: %s\n", name, strerror(errno));
        return -1;
    }
    // Walk back from the end, counting line ends, to where the last `lines`
    // lines start. A line end as the file's last byte doesn't count.
    off_t end = lseek(fd, 0, SEEK_END);
    off_t pos = end, start = 0;
    int found = 0;
    bool done = false;
    while (pos > 0 && !done) {
        size_t n = pos > (off_t)sizeof buf ? sizeof buf : (size_t)pos;
        pos -= n;
        if (lseek(fd, pos, SEEK_SET) < 0 || read(fd, buf, n) != (ssize_t)n) {
            break;
        }
        for (size_t i = n; i-- > 0;) {
            if (buf[i] == '\n' && pos + (off_t)i != end - 1 && ++found == lines) {
                start = pos + i + 1;
                done = true;
                break;
            }
        }
    }
    lseek(fd, start, SEEK_SET);
    ssize_t r;
    while ((r = read(fd, buf, sizeof buf)) > 0) {
        fwrite(buf, 1, r, stdout);
    }
    close(fd);
    sd_unlock();
    if (end > 0) {
        fflush(stdout);
    }
    return 0;
}

int datalog_list(void)
{
    if (!sd_usable()) {
        printf("card not usable (not mounted, or an SD fault)\n");
        return -1;
    }
    sd_lock();
    DIR *d = opendir(SD_MOUNT_POINT);
    if (!d) {
        sd_unlock();
        printf("can't read the card's root\n");
        return -1;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        char path[300];
        struct stat st;
        snprintf(path, sizeof path, SD_MOUNT_POINT "/%s", e->d_name);
        if (stat(path, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            printf("%10s  %s/\n", "", e->d_name);
        } else {
            printf("%10lu  %s\n", (unsigned long)st.st_size, e->d_name);
        }
    }
    closedir(d);
    sd_unlock();
    return 0;
}
