// Stage 12: WiFi only, no sensors. Every SEND_INTERVAL_MS the logger sends a
// status line to the listener (udp_listener.py) and logs whether an ack came
// back. The logger only pushes; it never listens.
//
// Status line: hello seq=<count> up=<seconds since boot> rssi=<dBm>
//              ap=<access point MAC> ip=<own address> mac=<own MAC>
//              reconnects=<times WiFi was regained>
// A missing reply is logged as "no reply" and the loop carries on; nothing is
// resent. Listener address and WiFi credentials come from wifi_secrets.h.

#include <inttypes.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "wifi.h"
#include "wifi_secrets.h"

#define SEND_INTERVAL_MS 10000
#define CONNECT_TIMEOUT_MS 30000
#define REPLY_TIMEOUT_MS 2000

static const char *TAG = "ambient";

void app_main(void)
{
    char mac[18];
    int wrc = wifi_connect(WIFI_SSID, WIFI_PASS, CONNECT_TIMEOUT_MS);
    if (wrc == -1) {
        ESP_LOGW(TAG, "not connected after %d s; still trying", CONNECT_TIMEOUT_MS / 1000);
    } else if (wrc == -2) {
        // Carry on: later stages keep logging to the SD card without WiFi.
        ESP_LOGE(TAG, "WiFi setup failed; continuing without WiFi");
    }
    wifi_mac_str(mac, sizeof(mac));
    ESP_LOGI(TAG, "mac %s, pushing to %s:%d", mac, LISTENER_HOST, LISTENER_PORT);

    uint32_t seq = 0;
    while (1) {
        if (!wifi_is_connected()) {
            ESP_LOGW(TAG, "not connected");
        } else {
            char ip[16];
            char bssid[18];
            char msg[160];
            char reply[128];
            size_t reply_len = 0;
            int rssi = 0;

            wifi_ip_str(ip, sizeof(ip));
            wifi_rssi(&rssi);
            wifi_bssid_str(bssid, sizeof(bssid));
            int len = snprintf(msg, sizeof(msg),
                               "hello seq=%" PRIu32 " up=%" PRIu64 " rssi=%d ap=%s ip=%s mac=%s reconnects=%" PRIu32,
                               seq, esp_timer_get_time() / 1000000, rssi, bssid, ip, mac,
                               wifi_reconnect_count());

            int rc = wifi_udp_exchange(LISTENER_HOST, LISTENER_PORT, msg, len,
                                       reply, sizeof(reply), &reply_len, REPLY_TIMEOUT_MS);
            if (rc == 0) {
                ESP_LOGI(TAG, "seq %" PRIu32 ": %s", seq, reply);
            } else if (rc == -2) {
                ESP_LOGW(TAG, "seq %" PRIu32 ": no reply", seq);
            } else {
                ESP_LOGE(TAG, "seq %" PRIu32 ": send failed", seq);
            }
            seq++;
        }
        vTaskDelay(pdMS_TO_TICKS(SEND_INTERVAL_MS));
    }
}
