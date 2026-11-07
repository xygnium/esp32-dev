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
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"

#include "clock.h"
#include "fault.h"
#include "sd.h"
#include "sensor_util.h"

static const char *TAG = "sd";

#define EVENTS_PATH     SD_MOUNT_POINT "/events.csv"
#define READINGS_HEADER "timestamp,sensor,data_type,value,status\n"
#define EVENTS_HEADER   "timestamp,source,event,detail\n"
#define HELD_MAX        32
#define LOW_PERCENT     10
#define FULL_BYTES      (64 * 1024)   // a failed write with less free than this is "full"
#define SPACE_CHECK_US  (3600LL * 1000000)

typedef struct {
    int64_t up_us;                    // esp_timer time when it happened
    char source[12];
    char event[14];
    char detail[56];
} held_event_t;

// The held list and the counters are shared by every task that logs events;
// guarded by s_mux. Card access is under sd_lock().
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static held_event_t s_held[HELD_MAX];
static uint32_t s_n_held;
static datalog_info_t s_info;
static bool s_ready;                  // datalog_init has run
static bool s_minute_files;           // test builds: a file per minute
static int64_t s_next_space_us = INT64_MAX;

// 2026-11-07T13:35:48.134Z
static void fmt_utc(int64_t us, char *buf, size_t size)
{
    time_t sec = (time_t)(us / 1000000);
    struct tm t;
    gmtime_r(&sec, &t);
    snprintf(buf, size, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", t.tm_year + 1900, t.tm_mon + 1,
             t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, (int)(us % 1000000 / 1000));
}

// The readings file for a stamp: YYYY-MM-DD.csv, or YYYY-MM-DDTHHMM.csv in
// minute mode (no colons: FAT doesn't allow them).
static void readings_name(int64_t us, char *buf, size_t size)
{
    time_t sec = (time_t)(us / 1000000);
    struct tm t;
    gmtime_r(&sec, &t);
    if (s_minute_files) {
        snprintf(buf, size, "%04d-%02d-%02dT%02d%02d.csv", t.tm_year + 1900, t.tm_mon + 1,
                 t.tm_mday, t.tm_hour, t.tm_min);
    } else {
        snprintf(buf, size, "%04d-%02d-%02d.csv", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
    }
}

// Caller holds sd_lock. Sets *free_bytes; returns -1 if the card can't say.
static int card_space(uint64_t *total, uint64_t *free_bytes)
{
    return esp_vfs_fat_info(SD_MOUNT_POINT, total, free_bytes) == ESP_OK ? 0 : -1;
}

// Caller holds sd_lock. Updates the low state; returns true when it has just
// gone low (the caller logs the event after unlocking), with the MB free.
static bool check_space(uint32_t *free_mb)
{
    uint64_t total, free_bytes;
    if (card_space(&total, &free_bytes) != 0 || total == 0) {
        return false;
    }
    bool low = free_bytes * 100 < total * LOW_PERCENT;
    portENTER_CRITICAL(&s_mux);
    bool was_low = s_info.low;
    s_info.low = low;
    portEXIT_CRITICAL(&s_mux);
    *free_mb = (uint32_t)(free_bytes >> 20);
    s_next_space_us = esp_timer_get_time() + SPACE_CHECK_US;
    return low && !was_low;
}

// Caller holds sd_lock. Append buf (and the header first, if the file is
// new), fsync, close. On failure the file is cut back to its old size (a new
// file is removed), and *fault gets the reason for the hard fault.
static int append(const char *path, const char *header, const char *buf, size_t len,
                  bool *created, uint32_t *size_after, const char **fault)
{
    errno = 0;
    struct stat st;
    bool exists = stat(path, &st) == 0;
    off_t old = exists ? st.st_size : 0;
    // An empty file gets the header too: a reset while a file was being
    // created can leave its directory entry with nothing in it.
    bool need_header = old == 0;
    *created = !exists;

    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    bool ok = fd >= 0;
    const char *chunks[2] = { need_header ? header : NULL, buf };
    size_t lens[2] = { need_header ? strlen(header) : 0, len };
    for (int i = 0; ok && i < 2; i++) {
        if (chunks[i] && lens[i] > 0) {
            ssize_t w = write(fd, chunks[i], lens[i]);
            ok = w == (ssize_t)lens[i];
        }
    }
    if (ok) {
        ok = fsync(fd) == 0;
    }
    if (ok) {
        struct stat after;
        *size_after = fstat(fd, &after) == 0 ? (uint32_t)after.st_size : 0;
        ok = close(fd) == 0;
        fd = -1;
    }
    if (ok) {
        return 0;
    }

    int err = errno;
    if (fd >= 0) {
        (void)ftruncate(fd, old);
        close(fd);
    }
    if (!exists) {
        unlink(path);
    }
    uint64_t total, free_bytes;
    bool full = err == ENOSPC
                || (card_space(&total, &free_bytes) == 0 && free_bytes < FULL_BYTES);
    *fault = full ? "full" : "write_failed";
    ESP_LOGE(TAG, "write to %s failed: %s", path, full ? "card full" : strerror(err));
    return -1;
}

// Format one event row into buf; returns its length.
static int event_row(char *buf, size_t size, int64_t utc_us, const char *source,
                     const char *event, const char *detail)
{
    char ts[32];
    fmt_utc(utc_us, ts, sizeof ts);
    int n = snprintf(buf, size, "%s,%s,%s,%s\n", ts, source, event, detail);
    return n < (int)size ? n : (int)size - 1;
}

// Caller holds sd_lock; the clock is set. Write the held events and then
// `extra` (one formatted row, or NULL) in one append. Returns 0, or -1 with
// *fault set.
static char s_ev_buf[(HELD_MAX + 1) * 112];   // used only under sd_lock

static int write_events(const char *extra, const char **fault)
{
    int64_t utc_now = sensor_utc_us();
    int64_t up_now = esp_timer_get_time();
    size_t len = 0;
    uint32_t n;
    portENTER_CRITICAL(&s_mux);
    n = s_n_held;
    portEXIT_CRITICAL(&s_mux);
    for (uint32_t i = 0; i < n; i++) {
        held_event_t e;
        portENTER_CRITICAL(&s_mux);
        e = s_held[i];
        portEXIT_CRITICAL(&s_mux);
        len += event_row(s_ev_buf + len, sizeof s_ev_buf - len, utc_now - (up_now - e.up_us),
                         e.source, e.event, e.detail);
    }
    if (extra) {
        size_t el = strlen(extra);
        if (el < sizeof s_ev_buf - len) {
            memcpy(s_ev_buf + len, extra, el);
            len += el;
        }
    }
    if (len == 0) {
        return 0;
    }
    bool created;
    uint32_t size_after;
    if (append(EVENTS_PATH, EVENTS_HEADER, s_ev_buf, len, &created, &size_after, fault) != 0) {
        return -1;
    }
    portENTER_CRITICAL(&s_mux);
    // Events held while this was written stay held for the next write.
    memmove(&s_held[0], &s_held[n], (s_n_held - n) * sizeof s_held[0]);
    s_n_held -= n;
    s_info.events += n + (extra ? 1 : 0);
    portEXIT_CRITICAL(&s_mux);
    return 0;
}

static void hold(const char *source, const char *event, const char *detail)
{
    bool dropped = false;
    portENTER_CRITICAL(&s_mux);
    if (s_n_held < HELD_MAX) {
        held_event_t *e = &s_held[s_n_held++];
        e->up_us = esp_timer_get_time();
        strlcpy(e->source, source, sizeof e->source);
        strlcpy(e->event, event, sizeof e->event);
        strlcpy(e->detail, detail, sizeof e->detail);
    } else {
        dropped = s_info.dropped++ == 0;
    }
    portEXIT_CRITICAL(&s_mux);
    if (dropped) {
        ESP_LOGW(TAG, "%d events held waiting for the clock; later ones are lost", HELD_MAX);
    }
}

static bool can_write(void)
{
    return s_ready && clock_is_set() && sd_usable();
}

// Write the held events, if any and if possible. Raises a fault on failure.
static void flush_held(void)
{
    portENTER_CRITICAL(&s_mux);
    uint32_t n = s_n_held;
    portEXIT_CRITICAL(&s_mux);
    if (n == 0 || !can_write()) {
        return;
    }
    const char *fault = NULL;
    sd_lock();
    int rc = write_events(NULL, &fault);
    sd_unlock();
    if (rc != 0) {
        fault_raise(FAULT_SD, fault);
    }
}

void datalog_event(const char *source, const char *event, const char *fmt, ...)
{
    char detail[56];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, sizeof detail, fmt, ap);
    va_end(ap);
    // A comma would split the column; the details here never need one.
    for (char *p = detail; *p; p++) {
        if (*p == ',' || *p == '\n') {
            *p = ';';
        }
    }

    if (!s_ready || !clock_is_set()) {
        hold(source, event, detail);
        return;
    }
    if (!sd_usable()) {
        return;                           // an SD fault: console and listener only
    }
    char row[160];
    event_row(row, sizeof row, sensor_utc_us(), source, event, detail);
    const char *fault = NULL;
    sd_lock();
    int rc = write_events(row, &fault);
    sd_unlock();
    if (rc != 0) {
        fault_raise(FAULT_SD, fault);
    }
}

void datalog_init(void)
{
    portENTER_CRITICAL(&s_mux);
    s_ready = true;
    portEXIT_CRITICAL(&s_mux);
    if (!sd_usable()) {
        return;
    }
    uint32_t free_mb = 0;
    sd_lock();
    bool newly_low = check_space(&free_mb);
    sd_unlock();
    if (newly_low) {
        ESP_LOGW(TAG, "card low: %lu MB free (under %d %%)", (unsigned long)free_mb, LOW_PERCENT);
        datalog_event("sd", "low", "%lu MB free", (unsigned long)free_mb);
    }
    flush_held();
}

void datalog_poll(void)
{
    flush_held();
    if (!sd_usable() || esp_timer_get_time() < s_next_space_us) {
        return;
    }
    uint32_t free_mb = 0;
    sd_lock();
    bool newly_low = check_space(&free_mb);
    sd_unlock();
    if (newly_low) {
        ESP_LOGW(TAG, "card low: %lu MB free (under %d %%)", (unsigned long)free_mb, LOW_PERCENT);
        datalog_event("sd", "low", "%lu MB free", (unsigned long)free_mb);
    }
}

void datalog_sample(const sensor_result_t r[SENSOR_COUNT])
{
    if (!can_write()) {
        return;
    }
    flush_held();
    if (!sd_usable()) {
        return;                           // flushing the events just failed
    }

    char buf[512];
    size_t len = 0;
    for (int id = 0; id < SENSOR_COUNT; id++) {
        char ts[32];
        fmt_utc(r[id].utc_us, ts, sizeof ts);
        for (int i = 0; i < sensor_value_count(id); i++) {
            int n;
            if (r[id].err) {
                n = snprintf(buf + len, sizeof buf - len, "%s,%s,%s,,%s\n", ts, sensor_name(id),
                             sensor_data_type(id, i), r[id].err);
            } else {
                n = snprintf(buf + len, sizeof buf - len, "%s,%s,%s,%.*f,ok\n", ts, sensor_name(id),
                             sensor_data_type(id, i), sensor_decimals(id, i), r[id].value[i]);
            }
            if (n > 0 && (size_t)n < sizeof buf - len) {
                len += n;
            }
        }
    }

    char name[24], path[48];
    readings_name(r[0].utc_us, name, sizeof name);
    snprintf(path, sizeof path, SD_MOUNT_POINT "/%s", name);
    const char *fault = NULL;
    bool created = false, newly_low = false;
    uint32_t size_after = 0, free_mb = 0;
    sd_lock();
    int rc = append(path, READINGS_HEADER, buf, len, &created, &size_after, &fault);
    if (rc == 0 && created) {
        newly_low = check_space(&free_mb);
    }
    sd_unlock();
    if (rc != 0) {
        fault_raise(FAULT_SD, fault);
        return;
    }
    portENTER_CRITICAL(&s_mux);
    strlcpy(s_info.file, name, sizeof s_info.file);
    s_info.file_bytes = size_after;
    s_info.rows += SENSOR_COUNT * SENSOR_MAX_VALUES;
    portEXIT_CRITICAL(&s_mux);
    if (newly_low) {
        ESP_LOGW(TAG, "card low: %lu MB free (under %d %%)", (unsigned long)free_mb, LOW_PERCENT);
        datalog_event("sd", "low", "%lu MB free", (unsigned long)free_mb);
    }
}

void datalog_get_info(datalog_info_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_info;
    out->held = s_n_held;
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

#ifdef AMBIENT_TEST_FAULTS
void datalog_test_minute_files(bool on)
{
    s_minute_files = on;
}

bool datalog_test_minute_files_on(void)
{
    return s_minute_files;
}
#endif
