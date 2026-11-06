// Settings that survive a restart, a serial console, WiFi pushes, the SD card
// and the clock (clock.c). Still no sensors and no logging.
//
// At boot and then every push interval (a setting, `config interval`), the logger
// sends a status line to the listener (logger-server/udp_listener.py) and records whether an
// ack came back. The logger only pushes; it never listens. Push problems are
// always printed on the serial console; successful pushes are hidden (quiet
// on, the boot default) until `quiet off`. Commands come in on
// the serial console (console_cmds.c); a settings change or `push` wakes this
// loop so it takes effect at once.
//
// Status line: hello seq=<count> up=<seconds since boot> rssi=<dBm>
//              ap=<access point MAC> ip=<own address> mac=<own MAC>
//              reconnects=<times WiFi was regained> push_s=<interval>
//              vin_mv=<supply voltage, mV; 0 if unavailable>
// The supply voltage is read just before each push and checked against the
// low-supply threshold (supply.c); from stage 20 it's read at every sample.
// A missing reply is counted and logged; nothing is resent.
//
// SD card (sd.c): mounted at boot, never formatted, and checked at every
// sample interval. A missing or failing card is a hard fault (fault.c):
// normal pushes stop, and the fault is printed on the console and pushed to
// the listener at once and then every push interval, as
//   fault sd=<missing|mount_failed|not_responding> seq=<n> up=<s> mac=<own MAC>
// Only a reboot clears it.
//
// Clock (clock.c): a DS3231 fault is a hard fault too (fault rtc=<missing|
// invalid|not_responding>). After a remediated restart (invalid DS3231, empty
// card) the clock is unset: the status line carries clock=unset and is sent
// every UNSET_RETRY_S as a time request until an ack sets the time. Each ack's
// time, plus half the round trip, is passed to the clock.

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "ambient.h"
#include "clock.h"
#include "console_cmds.h"
#include "fault.h"
#include "sd.h"
#include "settings.h"
#include "supply.h"
#include "wifi.h"

#define CONNECT_TIMEOUT_MS 30000
#define REPLY_TIMEOUT_MS 2000
#define UNSET_RETRY_S 10        // time requests while the clock is unset

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

// One push: the status line, or the fault line while in a hard fault.
static void push_once(const settings_t *cfg, const char *mac, uint32_t seq)
{
    uint32_t vin_mv = 0;
    supply_check(cfg->low_mv, &vin_mv);     // leaves 0 if no reading

    fault_src_t fsrc;
    const char *freason;
    bool in_fault = fault_get(&fsrc, &freason);
    if (in_fault) {
        ESP_LOGE(TAG, "HARD FAULT %s: %s. Fix it and reboot", fault_src_name(fsrc), freason);
    }

    if (cfg->listener_host[0] == '\0') {
        ESP_LOGW(TAG, "seq %" PRIu32 ": listener not set; use config listener <ip> <port>", seq);
        record(PUSH_NOT_CONNECTED, seq, NULL);
        return;
    }
    if (!wifi_is_connected()) {
        if (cfg->wifi_ssid[0] == '\0') {
            ESP_LOGW(TAG, "seq %" PRIu32 ": WiFi not set; use config wifi <ssid> <password>", seq);
        } else {
            ESP_LOGW(TAG, "seq %" PRIu32 ": not connected", seq);
        }
        record(PUSH_NOT_CONNECTED, seq, NULL);
        return;
    }

    char ip[16], bssid[18], msg[192], reply[128];
    size_t reply_len = 0;
    int rssi = 0;
    wifi_ip_str(ip, sizeof(ip));
    wifi_rssi(&rssi);
    wifi_bssid_str(bssid, sizeof(bssid));
    uint64_t up = (uint64_t)(esp_timer_get_time() / 1000000);
    int len;
    if (in_fault) {
        len = snprintf(msg, sizeof(msg), "fault %s=%s seq=%" PRIu32 " up=%" PRIu64 " mac=%s",
                       fault_src_name(fsrc), freason, seq, up, mac);
    } else {
        len = snprintf(msg, sizeof(msg),
                       "hello seq=%" PRIu32 " up=%" PRIu64 " rssi=%d ap=%s ip=%s mac=%s reconnects=%" PRIu32
                       " push_s=%" PRIu32 " vin_mv=%" PRIu32 "%s",
                       seq, up, rssi, bssid, ip, mac, wifi_reconnect_count(), cfg->push_s, vin_mv,
                       clock_is_set() ? "" : " clock=unset");
    }

    int64_t sent_us = esp_timer_get_time();
    int rc = wifi_udp_exchange(cfg->listener_host, cfg->listener_port, msg, len,
                               reply, sizeof(reply), &reply_len, REPLY_TIMEOUT_MS);
    int64_t rtt_us = esp_timer_get_time() - sent_us;
    if (rc == 0) {
        if (!s_quiet) {
            ESP_LOGI(TAG, "seq %" PRIu32 ": %s", seq, reply);
        }
        record(PUSH_OK, seq, reply);
        clock_on_ack(reply, rtt_us, cfg->clock_auto);
    } else if (rc == -2) {
        ESP_LOGW(TAG, "seq %" PRIu32 ": no reply", seq);
        record(PUSH_NO_REPLY, seq, NULL);
    } else {
        ESP_LOGE(TAG, "seq %" PRIu32 ": send failed", seq);
        record(PUSH_SEND_FAILED, seq, NULL);
    }
}

void app_main(void)
{
    s_loop_task = xTaskGetCurrentTaskHandle();

    // ESP-IDF's own messages are built at warnings-and-errors only (see
    // sdkconfig.defaults); the app's informational lines are turned back on.
    esp_log_level_set("ambient", ESP_LOG_INFO);
    esp_log_level_set("supply", ESP_LOG_INFO);
    esp_log_level_set("sd", ESP_LOG_INFO);
    esp_log_level_set("clock", ESP_LOG_INFO);
#ifdef AMBIENT_TEST_FAULTS
    ESP_LOGW(TAG, "TEST BUILD: fault injection enabled (test fault ...); not for production");
#endif

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

    int wrc = wifi_connect(cfg.wifi_ssid, cfg.wifi_pass, CONNECT_TIMEOUT_MS);
    if (wrc == -1) {
        ESP_LOGW(TAG, "not connected after %d s; still trying", CONNECT_TIMEOUT_MS / 1000);
    } else if (wrc == -3) {
        ESP_LOGW(TAG, "WiFi not set; use config wifi <ssid> <password>");
    } else if (wrc == -2) {
        // Carry on: the SD card is the record; pushes resume if WiFi returns.
        ESP_LOGE(TAG, "WiFi setup failed; continuing without WiFi");
    } else {
        char ip[16];
        wifi_ip_str(ip, sizeof(ip));
        ESP_LOGI(TAG, "WiFi connected, address %s", ip);
    }
    char mac[18];
    wifi_mac_str(mac, sizeof(mac));

    // Push once at boot, then every push interval; check the SD card every
    // sample interval. Both intervals are re-read on every wake, so a
    // `config interval` change also re-times what is being waited for. A new
    // fault is pushed at once, then every push interval.
    uint32_t seq = 0;
    bool first = true;
    bool fault_pushed = false;
    int64_t last_push_us = 0;
    int64_t last_check_us = esp_timer_get_time();
    while (1) {
        settings_get(&cfg);
        int64_t now = esp_timer_get_time();
        bool in_fault = fault_get(NULL, NULL);
        uint32_t every_s = (in_fault || clock_is_set()) ? cfg.push_s : UNSET_RETRY_S;
        int64_t due = first ? now : last_push_us + (int64_t)every_s * 1000000;
        int64_t check_due = last_check_us + (int64_t)cfg.sample_s * 1000000;
        int64_t clock_due = clock_next_due_us();
        portENTER_CRITICAL(&s_stats_mux);
        s_next_due_us = due;
        portEXIT_CRITICAL(&s_stats_mux);

        if (now >= check_due) {
            sd_check();
            last_check_us = now;
            continue;
        }
        if (now >= clock_due) {
            clock_poll();
            continue;
        }
        if (s_push_now || now >= due || (in_fault && !fault_pushed)) {
            s_push_now = false;
            first = false;
            fault_pushed = in_fault;
            push_once(&cfg, mac, seq++);
            last_push_us = esp_timer_get_time();
            continue;
        }
        // Sleep until the next push or check, or until the console wakes us
        // (setting changed, `push`, or an injected fault). +1 tick so we never
        // wake just short of due.
        int64_t next = due < check_due ? due : check_due;
        if (clock_due < next) {
            next = clock_due;
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS((next - now) / 1000) + 1);
    }
}
