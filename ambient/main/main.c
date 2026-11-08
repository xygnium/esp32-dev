// Settings that survive a restart, a serial console, WiFi, the SD card, the
// clock (clock.c), sensor sampling (sensors.c) and logging to the card
// (datalog.c).
//
// Every sample interval the sensors are read and each reading is written to
// the card. Every reading and event is also sent to the listener
// (logger-server/udp_listener.py), one message at a time:
//   data <timestamp,sensor,data_type,value,status>
//   event <timestamp,source,event,detail>
// The logger waits for the ack ("ack <UTC time> <CRC-16 of the message, 4
// hex digits>") and sends the same message
// again every RETRY_S until it is acked, then the next. Events go ahead of
// readings. Sampling carries on meanwhile; unsent messages wait in RAM.
// Each ack's time, plus half the round trip, is passed to the clock.
//
// SD card (sd.c) and clock (clock.c) faults are hard faults (fault.c):
// sampling stops, and the fault goes to the listener as an event
// (sd,fault,<reason> or rtc,fault,<reason>). Only a reboot clears it.
// After a remediated restart (invalid DS3231, empty card) the clock is
// unset: a clock,unset event is sent, and its ack sets the time.
//
// A failed sensor is a soft failure: the others carry on and it is tried
// again at the next sample. Commands come in on the serial console
// (console_cmds.c).

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "ambient.h"
#include "clock.h"
#include "console_cmds.h"
#include "datalog.h"
#include "fault.h"
#include "sd.h"
#include "sensors.h"
#include "settings.h"
#include "supply.h"
#include "thermistor.h"
#include "wifi.h"

#define CONNECT_TIMEOUT_MS 30000
#define REPLY_TIMEOUT_MS 2000
#define RETRY_S 10              // resend an unacked message this often

static const char *TAG = "ambient";

static TaskHandle_t s_loop_task;
static volatile bool s_push_now;
static volatile bool s_quiet = true;   // quiet on at boot

// Shared with the console task; guarded by s_stats_mux.
static portMUX_TYPE s_stats_mux = portMUX_INITIALIZER_UNLOCKED;
static push_stats_t s_stats;
static int64_t s_next_due_us;

void ambient_get_push_stats(push_stats_t *out)
{
    portENTER_CRITICAL(&s_stats_mux);
    *out = s_stats;
    int64_t due = s_next_due_us;
    portEXIT_CRITICAL(&s_stats_mux);
    int64_t left = due - esp_timer_get_time();
    out->next_in_s = left > 0 ? (uint32_t)(left / 1000000) : 0;
}

void ambient_wake(void)
{
    if (s_loop_task) {
        xTaskNotifyGive(s_loop_task);
    }
}

void ambient_set_quiet(bool on)
{
    s_quiet = on;
}

bool ambient_quiet(void)
{
    return s_quiet;
}

void ambient_request_push(void)
{
    s_push_now = true;
    ambient_wake();
}

static void record(push_result_t r, uint32_t seq, const char *reply)
{
    portENTER_CRITICAL(&s_stats_mux);
    s_stats.seq = seq;
    s_stats.last = r;
    s_stats.last_uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    s_stats.last_reply[0] = '\0';
    switch (r) {
    case PUSH_OK:
        s_stats.n_ok++;
        strlcpy(s_stats.last_reply, reply, sizeof(s_stats.last_reply));
        break;
    case PUSH_NO_REPLY:      s_stats.n_no_reply++; break;
    case PUSH_SEND_FAILED:   s_stats.n_send_failed++; break;
    case PUSH_NOT_CONNECTED: s_stats.n_not_connected++; break;
    case PUSH_NONE:          break;
    }
    portEXIT_CRITICAL(&s_stats_mux);
}

// CRC-16/XMODEM (Python's binascii.crc_hqx(data, 0)), from the ROM, as
// esp_rom_crc.h gives it.
static uint16_t crc16(const char *p, size_t len)
{
    return ~esp_rom_crc16_be((uint16_t)~0x0000, (const uint8_t *)p, len);
}

// Send one message and wait for its ack. True if acked.
static bool send_one(const settings_t *cfg, const char *msg, uint32_t seq)
{
    if (cfg->listener_host[0] == '\0' || !wifi_is_connected()) {
        record(PUSH_NOT_CONNECTED, seq, NULL);
        return false;
    }
    char reply[128];
    size_t reply_len = 0;
    int64_t sent_us = esp_timer_get_time();
    int rc = wifi_udp_exchange(cfg->listener_host, cfg->listener_port, msg, strlen(msg),
                               reply, sizeof(reply), &reply_len, REPLY_TIMEOUT_MS);
    int64_t rtt_us = esp_timer_get_time() - sent_us;
    if (rc != 0) {
        record(rc == -2 ? PUSH_NO_REPLY : PUSH_SEND_FAILED, seq, NULL);
        return false;
    }
    // The ack must end with the CRC-16 of the message as sent.
    char crc[6];
    snprintf(crc, sizeof crc, " %04x", crc16(msg, strlen(msg)));
    if (strncmp(reply, "ack ", 4) != 0 || reply_len < 9 || strcmp(reply + reply_len - 5, crc) != 0) {
        ESP_LOGW(TAG, "expected ack failed: %s", reply);
        record(PUSH_NO_REPLY, seq, NULL);
        return false;
    }
    if (!s_quiet) {
        ESP_LOGI(TAG, "sent: %s", msg);
    }
    record(PUSH_OK, seq, reply);
    clock_on_ack(reply, rtt_us, cfg->clock_auto);
    return true;
}

void app_main(void)
{
    // Excitation off before anything that could take time.
    thermistor_pin_init();
    s_loop_task = xTaskGetCurrentTaskHandle();

    // ESP-IDF's own messages are built at warnings-and-errors only (see
    // sdkconfig.defaults); the app's informational lines are turned back on.
    esp_log_level_set("ambient", ESP_LOG_INFO);
    esp_log_level_set("supply", ESP_LOG_INFO);
    esp_log_level_set("sd", ESP_LOG_INFO);
    esp_log_level_set("clock", ESP_LOG_INFO);
    esp_log_level_set("sensors", ESP_LOG_INFO);

    // Settings first: NVS must be ready before WiFi starts, and a failure
    // here just means running on defaults.
    settings_init();
    supply_init();
    settings_t cfg;
    settings_get(&cfg);

    // Console before WiFi, so wrong WiFi settings can be fixed from it.
    console_start();

    // Card and clock faults are raised here; WiFi still starts so they can be
    // reported. The clock needs the card's empty check.
    sd_init();
    clock_init();
    sensors_init();
    datalog_event("logger", "boot", "%s", esp_app_get_description()->version);

    int wrc = wifi_connect(cfg.wifi_ssid, cfg.wifi_pass, CONNECT_TIMEOUT_MS);
    if (wrc == -1) {
        ESP_LOGW(TAG, "not connected after %d s; still trying", CONNECT_TIMEOUT_MS / 1000);
    } else if (wrc == -3) {
        ESP_LOGW(TAG, "WiFi not set; use config wifi <ssid> <password>");
    } else if (wrc == -2) {
        // Carry on: the SD card is the record; sending resumes if WiFi returns.
        ESP_LOGE(TAG, "WiFi setup failed; continuing without WiFi");
    } else {
        char ip[16];
        wifi_ip_str(ip, sizeof(ip));
        ESP_LOGI(TAG, "WiFi connected, address %s", ip);
    }
    if (!clock_is_set() && !fault_get(NULL, NULL)) {
        datalog_event("clock", "unset", "waiting for ack");   // its ack sets the time
    }

    // Sample every sample interval; in between, send the waiting messages one
    // at a time, retrying an unacked one every RETRY_S.
    uint32_t seq = 0;
    bool failing = false;
    int64_t retry_due = 0;
    int64_t last_check_us = esp_timer_get_time();
    char msg[DATALOG_MSG_LEN];
    while (1) {
        settings_get(&cfg);
        int64_t now = esp_timer_get_time();
        int64_t check_due = last_check_us + (int64_t)cfg.sample_s * 1000000;
        int64_t clock_due = clock_next_due_us();

        if (now >= check_due) {
            if (!fault_get(NULL, NULL) && clock_is_set()) {
                uint32_t vin_mv;
                supply_check(cfg.low_mv, &vin_mv);
                sensor_result_t r[SENSOR_COUNT];
                sensors_sample(r);
                datalog_sample(r);        // the write is the card's check
            } else {
                sd_check();
            }
            last_check_us = now;
            continue;
        }
        if (now >= clock_due) {
            clock_poll();
            continue;
        }
        bool waiting = datalog_next(msg);
        if (waiting && (s_push_now || now >= retry_due)) {
            s_push_now = false;
            if (send_one(&cfg, msg, seq++)) {
                datalog_sent();
                if (failing) {
                    ESP_LOGI(TAG, "listener answering again");
                    failing = false;
                }
            } else {
                if (!failing) {
                    ESP_LOGW(TAG, "not acked; retrying every %d s: %s", RETRY_S, msg);
                    failing = true;
                }
                retry_due = esp_timer_get_time() + (int64_t)RETRY_S * 1000000;
            }
            continue;
        }
        portENTER_CRITICAL(&s_stats_mux);
        s_next_due_us = waiting ? retry_due : 0;
        portEXIT_CRITICAL(&s_stats_mux);

        // Sleep until the next sample, clock poll or retry, or until woken
        // (a new message, a setting changed, `push`). +1 tick so we never
        // wake just short of due.
        int64_t next = check_due < clock_due ? check_due : clock_due;
        if (waiting && retry_due < next) {
            next = retry_due;
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS((next - now) / 1000) + 1);
    }
}
