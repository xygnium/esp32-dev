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

#include "ambient.h"
#include "clock.h"
#include "fault.h"
#include "sd.h"
#include "sensor_util.h"

static const char *TAG = "sd";

#define EVENTS_PATH     SD_MOUNT_POINT "/events.csv"
#define READINGS_HEADER "timestamp,sensor,data_type,value,status\n"
#define EVENTS_HEADER   "timestamp,source,event,detail\n"

#define DATA_QUEUE_LEN  200     // readings waiting to be sent (about 32 KB)
#define EVENT_QUEUE_LEN 32      // events waiting; these go first

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;   // guards s_info
static datalog_info_t s_info;
static QueueHandle_t s_events, s_data, s_peeked;

// Queue a message for the listener. If its queue is full it's dropped (it's
// still on the card).
static void outbox_put(const char *kind, const char *row, size_t len)
{
    if (!s_events) {
        s_events = xQueueCreate(EVENT_QUEUE_LEN, DATALOG_MSG_LEN);
        s_data = xQueueCreate(DATA_QUEUE_LEN, DATALOG_MSG_LEN);
    }
    char msg[DATALOG_MSG_LEN];
    snprintf(msg, sizeof msg, "%s %.*s", kind, (int)len, row);
    if (xQueueSend(kind[0] == 'e' ? s_events : s_data, msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "queue full; not sent: %s", msg);
        return;
    }
    ambient_wake();
}

bool datalog_next(char *msg)
{
    s_peeked = NULL;
    if (s_events && xQueuePeek(s_events, msg, 0) == pdTRUE) {
        s_peeked = s_events;
    } else if (s_data && xQueuePeek(s_data, msg, 0) == pdTRUE) {
        s_peeked = s_data;
    }
    return s_peeked != NULL;
}

void datalog_sent(void)
{
    char msg[DATALOG_MSG_LEN];
    if (s_peeked) {
        xQueueReceive(s_peeked, msg, 0);
        s_peeked = NULL;
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
    }
    outbox_put("event", row, len - 1);    // without the line end
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
    for (char *row = buf, *end; (end = strchr(row, '\n')) != NULL; row = end + 1) {
        outbox_put("data", row, end - row);
    }
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
