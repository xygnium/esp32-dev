#include "clock.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

#include "datalog.h"
#include "ds3231.h"
#include "fault.h"
#include "sd.h"
#include "settings.h"

static const char *TAG = "clock";

#define RESYNC_US    (3600LL * 1000000)
#define STEP_LIMIT_US 1000000          // errors larger than this are stepped, not slewed
#define EDGE_WAIT_US 1500000           // the seconds register must change within this

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static clock_info_t s_info = { .last_sync_up_s = -1, .correction_up_s = -1 };
static int64_t s_next_resync_us = INT64_MAX;

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

// Sync the ESP32 clock to the DS3231 at its second edge. step: set the clock
// outright (boot); otherwise slew, unless the error is over STEP_LIMIT_US.
static int sync_from_ds3231(bool step)
{
    int64_t edge;
    struct tm t;
    bool osf;
    if (wait_edge(&edge) != 0 || ds3231_read(&t, &osf) != 0) {
        return -1;
    }
    int64_t ds_at_edge = (int64_t)mktime(&t) * 1000000;     // the ESP32's time zone is UTC
    int64_t mono = esp_timer_get_time();
    int64_t sys_at_edge = sys_now_us() - (mono - edge);
    int64_t offset = ds_at_edge - sys_at_edge;

    if (step || llabs(offset) > STEP_LIMIT_US) {
        int64_t target = ds_at_edge + (esp_timer_get_time() - edge);
        struct timeval tv = { .tv_sec = target / 1000000, .tv_usec = target % 1000000 };
        settimeofday(&tv, NULL);
        if (!step) {
            ESP_LOGW(TAG, "ESP32 clock was %lld ms off the DS3231: stepped", (long long)(offset / 1000));
            datalog_event("clock", "stepped", "%+lld ms", (long long)(offset / 1000));
        }
    } else {
        struct timeval d = { .tv_sec = offset / 1000000, .tv_usec = offset % 1000000 };
        adjtime(&d, NULL);
        ESP_LOGI(TAG, "resynced from the DS3231: ESP32 was %+lld ms off, slewing", (long long)(offset / 1000));
    }
    portENTER_CRITICAL(&s_mux);
    s_info.last_sync_up_s = uptime_s();
    s_info.last_sync_ms = step ? 0 : (int32_t)(offset / 1000);
    portEXIT_CRITICAL(&s_mux);
    s_next_resync_us = esp_timer_get_time() + RESYNC_US;
    return 0;
}

// Write the DS3231 with the true time (ESP32 time + offset_us) at the next
// whole second of true time.
static int set_ds3231(int64_t offset_us)
{
    int64_t mono0 = esp_timer_get_time();
    int64_t true_now = sys_now_us() + offset_us;
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

int clock_init(void)
{
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
    if (sync_from_ds3231(true) != 0) {
        not_responding();
        return -1;
    }
    portENTER_CRITICAL(&s_mux);
    s_info.set = true;
    portEXIT_CRITICAL(&s_mux);
    char now[40];
    clock_now_str(now, sizeof now);
    ESP_LOGI(TAG, "set from the DS3231: %s", now);
    datalog_event("clock", "set", "from DS3231");
    return 0;
}

bool clock_is_set(void)
{
    portENTER_CRITICAL(&s_mux);
    bool set = s_info.set;
    portEXIT_CRITICAL(&s_mux);
    return set;
}

int64_t clock_next_due_us(void)
{
    return clock_is_set() && !fault_get(NULL, NULL) ? s_next_resync_us : INT64_MAX;
}

void clock_poll(void)
{
    if (!clock_is_set() || fault_get(NULL, NULL) || esp_timer_get_time() < s_next_resync_us) {
        return;
    }
    if (sync_from_ds3231(false) != 0) {
        not_responding();
    }
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

void clock_on_ack(const char *reply, int64_t rtt_us, bool auto_correct)
{
    if (fault_get(NULL, NULL)) {
        return;                           // acks never clear a fault
    }
    int64_t ack_us;
    if (!parse_ack(reply, &ack_us)) {
        ESP_LOGW(TAG, "ack time not understood: %s", reply);
        return;
    }
    int64_t offset = ack_us + rtt_us / 2 - sys_now_us();
    portENTER_CRITICAL(&s_mux);
    s_info.ack_seen = true;
    s_info.ack_offset_ms = (int32_t)(offset / 1000);
    s_info.ack_rtt_ms = (int32_t)(rtt_us / 1000);
    portEXIT_CRITICAL(&s_mux);

    bool set = clock_is_set();
    if (set && (!auto_correct || llabs(offset) <= CLOCK_CORRECT_MS * 1000LL)) {
        return;
    }
    if (set) {
        struct timeval rem = { 0 };
        adjtime(NULL, &rem);
        if (rem.tv_sec != 0 || rem.tv_usec != 0) {
            return;                       // still slewing from the last sync
        }
    }

    if (set_ds3231(offset) != 0 || (!set && ds3231_clear_osf() != 0)
        || sync_from_ds3231(!set) != 0) {
        not_responding();
        return;
    }
    portENTER_CRITICAL(&s_mux);
    s_info.set = true;
    s_info.correction_up_s = uptime_s();
    s_info.correction_ms = set ? (int32_t)(offset / 1000) : 0;
    portEXIT_CRITICAL(&s_mux);
    char now[40];
    clock_now_str(now, sizeof now);
    if (set) {
        ESP_LOGI(TAG, "DS3231 corrected by %+lld ms from the ack; now %s", (long long)(offset / 1000), now);
        datalog_event("clock", "corrected", "%+lld ms", (long long)(offset / 1000));
    } else {
        ESP_LOGI(TAG, "DS3231 set from the ack and its stop flag cleared; now %s", now);
        datalog_event("clock", "set_from_ack", "stop flag cleared");
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
