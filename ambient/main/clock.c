#include "clock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "nvs.h"

#include "datalog.h"
#include "ds3231.h"
#include "fault.h"
#include "sd.h"

static const char *TAG = "clock";

#define SYNC_EVERY_US (24LL * 3600 * 1000000)
#define EDGE_WAIT_US  1500000          // the seconds register must change within this
#define NVS_NAMESPACE "ambient"        // the settings' namespace
#define K_SYNCS       "clock_syncs"

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static clock_info_t s_info = { .last_sync_up_s = -1 };
static int64_t s_last_sync_us = -1;    // esp_timer time of the last logserv sync

static int64_t sys_now_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

static int64_t uptime_s(void)
{
    return esp_timer_get_time() / 1000000;
}

static void not_responding(void)
{
    fault_raise(FAULT_RTC, "not_responding");
}

// Wait for the DS3231's seconds register to change. *edge_us is the esp_timer
// time of the read that saw the change (within about 1 ms of the edge).
static int wait_edge(int64_t *edge_us)
{
    int s0, s;
    if (ds3231_read_seconds(&s0) != 0) {
        return -1;
    }
    int64_t start = esp_timer_get_time();
    while (esp_timer_get_time() - start < EDGE_WAIT_US) {
        int64_t before = esp_timer_get_time();
        if (ds3231_read_seconds(&s) != 0) {
            return -1;
        }
        if (s != s0) {
            *edge_us = before;
            return 0;
        }
        esp_rom_delay_us(500);
    }
    ESP_LOGE(TAG, "DS3231 seconds didn't change in %d ms: not running", EDGE_WAIT_US / 1000);
    return -1;
}

// Set the ESP32 clock to the RTC at its second edge.
static int sync_from_ds3231(void)
{
    int64_t edge;
    struct tm t;
    bool osf;
    if (wait_edge(&edge) != 0 || ds3231_read(&t, &osf) != 0) {
        return -1;
    }
    // The ESP32's time zone is UTC.
    int64_t target = (int64_t)mktime(&t) * 1000000 + (esp_timer_get_time() - edge);
    struct timeval tv = { .tv_sec = target / 1000000, .tv_usec = target % 1000000 };
    settimeofday(&tv, NULL);
    return 0;
}

// The RTC's time (us) at esp_timer time ref_us (under 1 s ago), from its next tick.
static int rtc_time_at(int64_t ref_us, int64_t *rtc_us)
{
    int64_t edge;
    struct tm t;
    bool osf;
    if (wait_edge(&edge) != 0 || ds3231_read(&t, &osf) != 0) {
        return -1;
    }
    *rtc_us = (int64_t)mktime(&t) * 1000000 - (edge - ref_us);
    return 0;
}

// Write the RTC at the next whole second of true time; true_us was the true
// time at esp_timer time ref_us.
static int set_ds3231(int64_t true_us, int64_t ref_us)
{
    int64_t mono0 = esp_timer_get_time();
    int64_t true_now = true_us + (mono0 - ref_us);
    int64_t wait = 1000000 - true_now % 1000000;
    if (wait < 2000) {
        wait += 1000000;                  // leave time to get ready
    }
    time_t sec = (time_t)((true_now + wait) / 1000000);
    struct tm t;
    gmtime_r(&sec, &t);
    int64_t target = mono0 + wait;
    if (wait > 30000) {
        vTaskDelay(pdMS_TO_TICKS((wait - 20000) / 1000));
    }
    while (esp_timer_get_time() < target) {
    }
    return ds3231_write(&t);
}

static void load_syncs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    clock_sync_t buf[CLOCK_SYNCS];
    size_t len = sizeof buf;
    if (nvs_get_blob(h, K_SYNCS, buf, &len) == ESP_OK && len % sizeof(clock_sync_t) == 0) {
        memcpy(s_info.syncs, buf, len);
        s_info.n_syncs = (int)(len / sizeof(clock_sync_t));
    }
    nvs_close(h);
}

static void save_syncs(const clock_sync_t *syncs, int n)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, K_SYNCS, syncs, n * sizeof(clock_sync_t));
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sync record not saved: %s", esp_err_to_name(err));
    }
}

int clock_init(void)
{
    load_syncs();
    if (ds3231_init(DS3231_ADDR) != 0) {
        fault_raise(FAULT_RTC, "missing");
        return -1;
    }
    struct tm t;
    bool osf;
    if (ds3231_read(&t, &osf) != 0) {
        not_responding();
        return -1;
    }
    if (osf) {
        if (sd_is_empty()) {
            ESP_LOGW(TAG, "DS3231 time invalid (oscillator stopped) and the card is empty: "
                          "remediated restart, waiting for an ack to set the time");
            return 0;
        }
        fault_raise(FAULT_RTC, "invalid");
        return -1;
    }
    if (sync_from_ds3231() != 0) {
        not_responding();
        return -1;
    }
    portENTER_CRITICAL(&s_mux);
    s_info.set = true;
    portEXIT_CRITICAL(&s_mux);
    char now[40];
    clock_now_str(now, sizeof now);
    ESP_LOGI(TAG, "set from the RTC: %s", now);
    return 0;
}

bool clock_is_set(void)
{
    portENTER_CRITICAL(&s_mux);
    bool set = s_info.set;
    portEXIT_CRITICAL(&s_mux);
    return set;
}

// "ack 2026-11-06T07:08:19.482Z" (milliseconds optional) to microseconds.
static bool parse_ack(const char *reply, int64_t *us)
{
    int y, mo, d, h, mi, s, n = 0, ms = 0;
    if (sscanf(reply, "ack %4d-%2d-%2dT%2d:%2d:%2d%n", &y, &mo, &d, &h, &mi, &s, &n) != 6) {
        return false;
    }
    const char *p = reply + n;
    if (*p == '.') {
        if (sscanf(p, ".%3d", &ms) != 1) {
            return false;
        }
    }
    struct tm t = { .tm_year = y - 1900, .tm_mon = mo - 1, .tm_mday = d,
                    .tm_hour = h, .tm_min = mi, .tm_sec = s };
    *us = (int64_t)mktime(&t) * 1000000 + (int64_t)ms * 1000;
    return true;
}

void clock_on_ack(const char *reply, int64_t rtt_us, int64_t arrived_us)
{
    if (fault_get(NULL, NULL)) {
        return;                           // acks never clear a fault
    }
    if (s_last_sync_us >= 0 && arrived_us - s_last_sync_us < SYNC_EVERY_US) {
        return;                           // not due
    }
    int64_t ack_us;
    if (!parse_ack(reply, &ack_us)) {
        ESP_LOGW(TAG, "logserv time not understood: %s", reply);
        return;
    }
    int64_t true_us = ack_us + rtt_us / 2;    // true time when the ack arrived
    bool set = clock_is_set();
    int64_t rtc_us = 0;
    if ((set && rtc_time_at(arrived_us, &rtc_us) != 0)
        || set_ds3231(true_us, arrived_us) != 0
        || (!set && ds3231_clear_osf() != 0)
        || sync_from_ds3231() != 0) {
        not_responding();
        return;
    }
    s_last_sync_us = esp_timer_get_time();
    int32_t diff_ms = (int32_t)((true_us - rtc_us) / 1000);
    clock_sync_t syncs[CLOCK_SYNCS];
    int n;
    portENTER_CRITICAL(&s_mux);
    s_info.set = true;
    s_info.last_sync_up_s = uptime_s();
    if (set) {
        if (s_info.n_syncs == CLOCK_SYNCS) {
            memmove(s_info.syncs, s_info.syncs + 1, (CLOCK_SYNCS - 1) * sizeof(clock_sync_t));
            s_info.n_syncs--;
        }
        s_info.syncs[s_info.n_syncs++] = (clock_sync_t){ .utc_s = sys_now_us() / 1000000, .diff_ms = diff_ms };
    }
    n = s_info.n_syncs;
    memcpy(syncs, s_info.syncs, sizeof syncs);
    portEXIT_CRITICAL(&s_mux);
    char now[40];
    clock_now_str(now, sizeof now);
    if (set) {
        save_syncs(syncs, n);
        ESP_LOGI(TAG, "RTC synced to logserv (logserv minus RTC was %+ld ms); now %s", (long)diff_ms, now);
        datalog_event("clock", "synced", "%+ld ms", (long)diff_ms);
    } else {
        ESP_LOGI(TAG, "RTC set from logserv and its stop flag cleared; now %s", now);
        datalog_event("clock", "synced", "%s", "");
    }
}

void clock_get_info(clock_info_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_info;
    portEXIT_CRITICAL(&s_mux);
}

void clock_now_str(char *buf, size_t size)
{
    int64_t us = sys_now_us();
    time_t sec = (time_t)(us / 1000000);
    struct tm t;
    gmtime_r(&sec, &t);
    snprintf(buf, size, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", t.tm_year + 1900, t.tm_mon + 1,
             t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, (int)(us % 1000000 / 1000));
}
