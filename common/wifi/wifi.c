#include "wifi.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"

static const char *TAG = "wifi";

#define CONNECTED_BIT BIT0

static EventGroupHandle_t s_events;
static esp_netif_t *s_netif;
static volatile uint32_t s_got_ip_count;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        xEventGroupClearBits(s_events, CONNECTED_BIT);
        ESP_LOGW(TAG, "disconnected (reason %d), retrying", d->reason);
        // Retry once a second so a router that is down doesn't get hammered.
        // This handler runs on the shared event task, so the wait also holds
        // up other WiFi/IP events for that second; revisit before adding sleep.
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        s_got_ip_count++;
        ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_events, CONNECTED_BIT);
    }
}

// Run one setup step; on failure log which step and why, and give up on
// WiFi (return -2) instead of aborting. A logger must keep sampling to the
// SD card when WiFi can't start, so WiFi errors never restart the board.
#define SETUP_STEP(call)                                                   \
    do {                                                                   \
        esp_err_t e_ = (call);                                             \
        if (e_ != ESP_OK) {                                                \
            ESP_LOGE(TAG, "setup failed: %s -> %s", #call, esp_err_to_name(e_)); \
            return -2;                                                     \
        }                                                                  \
    } while (0)

int wifi_connect(const char *ssid, const char *pass, uint32_t timeout_ms)
{
    // WiFi keeps its radio calibration data in NVS (non-volatile storage,
    // a small key-value settings area in flash), so NVS must be ready first.
    // Still a hard stop: without NVS no saved settings work either. This init
    // moves to the settings store at stage 12c, which should fall back to
    // default settings instead of stopping.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    s_events = xEventGroupCreate();
    if (s_events == NULL) {
        ESP_LOGE(TAG, "setup failed: no memory for event group");
        return -2;
    }
    SETUP_STEP(esp_netif_init());
    SETUP_STEP(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();
    if (s_netif == NULL) {
        ESP_LOGE(TAG, "setup failed: esp_netif_create_default_wifi_sta");
        return -2;
    }

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    SETUP_STEP(esp_wifi_init(&init));
    SETUP_STEP(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    SETUP_STEP(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));

    wifi_config_t cfg = { 0 };
    strncpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strncpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password));
    // With a router and an extender sharing one SSID, the default fast scan
    // joins whichever answers first. Scan every channel and take the strongest.
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    SETUP_STEP(esp_wifi_set_mode(WIFI_MODE_STA));
    SETUP_STEP(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    SETUP_STEP(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(s_events, CONNECTED_BIT, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(timeout_ms));
    return (bits & CONNECTED_BIT) ? 0 : -1;
}

bool wifi_is_connected(void)
{
    return s_events && (xEventGroupGetBits(s_events) & CONNECTED_BIT);
}

uint32_t wifi_reconnect_count(void)
{
    return s_got_ip_count > 0 ? s_got_ip_count - 1 : 0;
}

int wifi_rssi(int *rssi)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return -1;
    }
    *rssi = ap.rssi;
    return 0;
}

void wifi_ip_str(char *buf, size_t size)
{
    esp_netif_ip_info_t ip = { 0 };
    if (s_netif) {
        esp_netif_get_ip_info(s_netif, &ip);
    }
    snprintf(buf, size, IPSTR, IP2STR(&ip.ip));
}

// A failed lookup leaves the zeroed record, so this prints all zeros.
void wifi_bssid_str(char *buf, size_t size)
{
    wifi_ap_record_t ap = { 0 };
    esp_wifi_sta_get_ap_info(&ap);
    const uint8_t *b = ap.bssid;
    snprintf(buf, size, "%02x:%02x:%02x:%02x:%02x:%02x", b[0], b[1], b[2], b[3], b[4], b[5]);
}

// A failed read leaves the zeroed buffer, so this prints all zeros.
void wifi_mac_str(char *buf, size_t size)
{
    uint8_t m[6] = { 0 };
    esp_wifi_get_mac(WIFI_IF_STA, m);
    snprintf(buf, size, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

int wifi_udp_exchange(const char *host, uint16_t port,
                      const void *msg, size_t msg_len,
                      char *reply, size_t reply_size, size_t *reply_len,
                      uint32_t timeout_ms)
{
    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port = htons(port),
    };
    if (inet_pton(AF_INET, host, &dest.sin_addr) != 1) {
        ESP_LOGE(TAG, "bad host address %s", host);
        return -1;
    }

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket: errno %d", errno);
        return -1;
    }
    struct timeval tv = { .tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int rc = -1;
    if (sendto(sock, msg, msg_len, 0, (struct sockaddr *)&dest, sizeof(dest)) < 0) {
        ESP_LOGE(TAG, "sendto: errno %d", errno);
    } else {
        int n = recvfrom(sock, reply, reply_size - 1, 0, NULL, NULL);
        if (n >= 0) {
            reply[n] = '\0';
            *reply_len = n;
            rc = 0;
        } else {
            rc = -2;
        }
    }
    close(sock);
    return rc;
}
