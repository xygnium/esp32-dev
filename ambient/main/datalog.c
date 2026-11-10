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

#define READINGS_HEADER "timestamp,sensor,data_type,value,status\n"

#define SENT_PATH       SD_MOUNT_POINT "/sent.txt"
#define SAVE_US         (10 * 60 * 1000000LL)   // how often the send position is saved
#define RAM_QUEUE_LEN   32      // events waiting to be sent

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;   // guards s_info
static datalog_info_t s_info;

// Sending: events go from a RAM queue, ahead of readings; they're not
// written to the card. Readings are sent from the card, oldest day file
// first. The position (the readings file and the byte after its last acked
// line) is saved in sent.txt every SAVE_US, so after a reboot up to SAVE_US of
// lines are sent again. With no sent.txt (a new card) everything on the card
// is sent. A day file is deleted once it's all sent and a newer one exists.
static QueueHandle_t s_ram;
static char s_cur[24];            // readings file being sent; "" none yet
static long s_cur_off;            // where the next line to send starts
static long s_next_off;           // after the line datalog_next returned
static int s_rfd = -1;            // s_cur, open for reading
static enum { FROM_NONE, FROM_RAM, FROM_READINGS } s_from;
static char s_today[40];          // today's readings file; "" before the first sample
static char s_today_path[64];
static int64_t s_today_day = -1;  // its UTC day number
static bool s_loaded, s_dirty;
static int64_t s_saved_us;

#define LINE_END   -1             // read_line: no whole line left in the file
#define LINE_ERROR -2             // read_line: the card couldn't be read

void datalog_init(void)
{
    s_ram = xQueueCreate(RAM_QUEUE_LEN, DATALOG_MSG_LEN);
}

// The status line shows the file being sent.
static void show_sent_file(void)
{
    portENTER_CRITICAL(&s_mux);
    strlcpy(s_info.sent_file, s_cur, sizeof s_info.sent_file);
    s_info.sent_bytes = (uint32_t)s_cur_off;
    portEXIT_CRITICAL(&s_mux);
}

static void load_pos(void)
{
    sd_lock();
    FILE *f = fopen(SENT_PATH, "r");
    if (f) {
        if (fscanf(f, "%23s %ld", s_cur, &s_cur_off) != 2 || s_cur[0] == '-') {
            s_cur[0] = '\0';
            s_cur_off = 0;
        }
        fclose(f);
    }
    sd_unlock();
    s_loaded = true;
    show_sent_file();
}

// Fixed width, so each save overwrites the same bytes. A failed save is
// tried again at the next ack.
static void save_pos(void)
{
    char line[48];
    int n = snprintf(line, sizeof line, "%-23s %10ld\n", s_cur[0] ? s_cur : "-", s_cur_off);
    sd_lock();
    int fd = open(SENT_PATH, O_WRONLY | O_CREAT, 0644);
    bool ok = fd >= 0 && write(fd, line, n) == n && fsync(fd) == 0;
    if (fd >= 0) {
        close(fd);
    }
    sd_unlock();
    if (!ok) {
        ESP_LOGW(TAG, "can't save the send position: %s", strerror(errno));
        return;
    }
    s_saved_us = esp_timer_get_time();
    s_dirty = false;
}

// FatFs doesn't keep two handles on one file in step: a reader open across a
// write to the same file doesn't see the new size. So the reader is closed
// before each sample is written, and at a file switch.
static void close_reader(void)
{
    if (s_rfd >= 0) {
        sd_lock();
        close(s_rfd);
        sd_unlock();
        s_rfd = -1;
    }
}

// The line starting at off in s_cur, skipping header lines, into msg as
// "data <line>". Returns the offset after it, LINE_END if there's no whole
// line left (or the file is gone), or LINE_ERROR if the card couldn't be read.
static long read_line(long off, char *msg)
{
    char buf[DATALOG_MSG_LEN];
    long rc = LINE_ERROR;
    sd_lock();
    if (s_rfd < 0) {
        char path[64];
        snprintf(path, sizeof path, SD_MOUNT_POINT "/%s", s_cur);
        s_rfd = open(path, O_RDONLY);
        if (s_rfd < 0 && errno == ENOENT) {
            rc = LINE_END;
        }
    }
    while (s_rfd >= 0) {
        if (lseek(s_rfd, off, SEEK_SET) != off) {
            break;
        }
        ssize_t n = read(s_rfd, buf, sizeof buf - 1);
        if (n < 0) {
            break;
        }
        buf[n] = '\0';
        char *end = strchr(buf, '\n');
        if (!end) {
            rc = LINE_END;
            break;
        }
        *end = '\0';
        off += end - buf + 1;
        if (strncmp(buf, "timestamp,", 10) != 0) {
            snprintf(msg, DATALOG_MSG_LEN, "data %.150s", buf);
            rc = off;
            break;
        }
    }
    sd_unlock();
    return rc;
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
    if (xQueuePeek(s_ram, msg, 0) == pdTRUE) {
        s_from = FROM_RAM;
        return true;
    }
    if (!sd_usable()) {
        close_reader();
        return false;
    }
    if (!s_loaded) {
        load_pos();
    }
    while (1) {
        if (s_cur[0]) {
            long rc = read_line(s_cur_off, msg);
            if (rc >= 0) {
                s_next_off = rc;
                s_from = FROM_READINGS;
                return true;
            }
            if (rc == LINE_ERROR) {
                close_reader();           // reopened and tried again at the next wake
                return false;
            }
            // All of s_cur is sent. If it's today's file, or today's isn't
            // known yet (no sample since boot), more may come: wait.
            if (!s_today[0] || strcmp(s_cur, s_today) >= 0) {
                return false;
            }
        }
        char next[24];
        if (!next_file(s_cur, next)) {
            return false;
        }
        char done[64] = "";               // the finished file
        if (s_cur[0]) {
            snprintf(done, sizeof done, SD_MOUNT_POINT "/%s", s_cur);
        }
        close_reader();
        strlcpy(s_cur, next, sizeof s_cur);
        s_cur_off = 0;
        show_sent_file();
        if (done[0]) {
            // Save the move first, so a reboot never points at a deleted file.
            save_pos();
            sd_lock();
            int rc = unlink(done);
            sd_unlock();
            if (rc == 0) {
                ESP_LOGI(TAG, "deleted %s (all sent)", done);
            } else if (errno != ENOENT) {
                ESP_LOGW(TAG, "can't delete %s: %s", done, strerror(errno));
            }
        } else {
            s_dirty = true;
        }
    }
}

void datalog_sent(void)
{
    char msg[DATALOG_MSG_LEN];
    if (s_from == FROM_RAM) {
        xQueueReceive(s_ram, msg, 0);
    } else if (s_from == FROM_READINGS) {
        s_cur_off = s_next_off;
        s_dirty = true;
        portENTER_CRITICAL(&s_mux);
        s_info.sent_bytes = (uint32_t)s_cur_off;
        portEXIT_CRITICAL(&s_mux);
    }
    s_from = FROM_NONE;
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
    char detail[64], ts[32] = "";
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof detail, fmt, ap);
    va_end(ap);
    if (clock_is_set()) {
        fmt_utc(sensor_utc_us(), ts, sizeof ts);
    }
    // Sent from RAM, not written to the card.
    char msg[DATALOG_MSG_LEN];
    snprintf(msg, sizeof msg, "event %s,%s,%s,%.64s", ts, source, event, detail);
    if (xQueueSend(s_ram, msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "queue full; not sent: %s", msg);
        return;
    }
    portENTER_CRITICAL(&s_mux);
    s_info.events++;
    portEXIT_CRITICAL(&s_mux);
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

    // One file per UTC day: YYYY-MM-DD.csv. The name is rebuilt only when
    // the UTC day changes.
    int64_t day = r[0].utc_us / 1000000 / 86400;
    if (day != s_today_day) {
        time_t sec = (time_t)(day * 86400);
        struct tm t;
        gmtime_r(&sec, &t);
        snprintf(s_today, sizeof s_today, "%04d-%02d-%02d.csv", t.tm_year + 1900, t.tm_mon + 1,
                 t.tm_mday);
        snprintf(s_today_path, sizeof s_today_path, SD_MOUNT_POINT "/%s", s_today);
        s_today_day = day;
        portENTER_CRITICAL(&s_mux);
        strlcpy(s_info.file, s_today, sizeof s_info.file);
        portEXIT_CRITICAL(&s_mux);
    }
    close_reader();                   // so the next read sees this write
    long size = append(s_today_path, READINGS_HEADER, buf, len);
    if (size < 0) {
        return;
    }
    ambient_wake();
    portENTER_CRITICAL(&s_mux);
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
        printf("give a file name in the card's root, e.g. 2026-11-09.csv\n");
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
