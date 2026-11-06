#include "settings.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "settings";

#define NVS_NAMESPACE "ambient"

// NVS keys (15 characters at most).
#define K_PUSH      "push_s"
#define K_SAMPLE    "sample_s"
#define K_WIN_START "win_start"
#define K_WIN_END   "win_end"
#define K_CLOCK     "clock_auto"
#define K_HOST      "lst_host"
#define K_PORT      "lst_port"
#define K_SSID      "wifi_ssid"
#define K_PASS      "wifi_pass"
#define K_LOWV      "low_mv"

static settings_t s_cur;
static SemaphoreHandle_t s_lock;
static bool s_nvs_ok;

static void set_defaults(settings_t *s)
{
    memset(s, 0, sizeof(*s));             // WiFi and listener: empty, not set
    s->sample_s = 120;
    s->push_s = 3600;                     // hourly, 30 samples
    s->window_start = 0;                  // 00:00-24:00 UTC: always, until a window is
    s->window_end = SETTINGS_DAY_MIN;     // set; not enforced before stage 22
    s->clock_auto = true;                 // dev10's clock is internet-synced, so trust the ack
    s->low_mv = 4600;                     // provisional; set from normal readings (plan)
}

// --- validation: NULL when valid, else the reason ---

static const char *check_sample(uint32_t v)
{
    return (v < SETTINGS_SAMPLE_MIN_S || v > SETTINGS_SAMPLE_MAX_S)
               ? "sample interval must be 15 s to 1 h" : NULL;
}

// The push interval on its own; check_interval() checks it against the sample.
static const char *check_push(uint32_t v)
{
    return (v < SETTINGS_SAMPLE_MIN_S || v > SETTINGS_PUSH_MAX_S)
               ? "push interval must be 15 s to 24 h" : NULL;
}

static const char *check_interval(uint32_t sample, uint32_t push)
{
    const char *bad = check_sample(sample);
    if (!bad) bad = check_push(push);
    if (bad) return bad;
    if (push < sample) {
        return "push interval must not be shorter than the sample interval";
    }
    return push % sample ? "push interval must be a whole multiple of the sample interval" : NULL;
}

static const char *check_window(uint16_t start, uint16_t end)
{
    if (start >= SETTINGS_DAY_MIN || end == 0 || end > SETTINGS_DAY_MIN) {
        return "window times must be 00:00-23:59 (start) and 00:01-24:00 (end)";
    }
    return start == end ? "window start and end must differ" : NULL;
}

// Exactly four dot-separated decimal numbers 0-255. lwIP's inet_pton alone
// isn't enough: it accepts shorthand like "1.2.3" (meaning 1.2.0.3), which
// let a mistyped listener address through in testing.
static const char *check_host(const char *host)
{
    const char *bad = "listener must be a dotted-quad IPv4 address (a.b.c.d, each 0-255)";
    if (strlen(host) >= sizeof(s_cur.listener_host)) {
        return bad;
    }
    int parts = 0;
    const char *p = host;
    while (1) {
        int digits = 0, value = 0;
        while (*p >= '0' && *p <= '9') {
            value = value * 10 + (*p - '0');
            digits++;
            p++;
        }
        if (digits == 0 || digits > 3 || value > 255) {
            return bad;
        }
        parts++;
        if (*p == '\0') {
            break;
        }
        if (*p != '.' || parts == 4) {
            return bad;
        }
        p++;
    }
    return parts == 4 ? NULL : bad;
}

static const char *check_port(uint32_t port)
{
    return (port == 0 || port > 65535) ? "port must be 1-65535" : NULL;
}

static const char *check_low_mv(uint32_t v)
{
    return (v < SETTINGS_LOWV_MIN_MV || v > SETTINGS_LOWV_MAX_MV)
               ? "low-supply threshold must be 3.0-5.5 V" : NULL;
}

static const char *check_wifi(const char *ssid, const char *pass)
{
    size_t sl = strlen(ssid), pl = strlen(pass);
    if (sl == 0 || sl > 32) {
        return "SSID must be 1-32 characters";
    }
    return (pl != 0 && (pl < 8 || pl > 63)) ? "password must be empty or 8-63 characters" : NULL;
}

// --- NVS load/save ---

static void load_u32(nvs_handle_t h, const char *key, uint32_t *field,
                     const char *(*check)(uint32_t))
{
    uint32_t v;
    if (nvs_get_u32(h, key, &v) != ESP_OK) {
        return;                           // not saved: keep the default
    }
    if (check(v) != NULL) {
        ESP_LOGW(TAG, "saved %s=%lu out of range; using default", key, (unsigned long)v);
        return;
    }
    *field = v;
}

static bool load_str(nvs_handle_t h, const char *key, char *out, size_t size)
{
    size_t len = size;
    return nvs_get_str(h, key, out, &len) == ESP_OK;
}

static void load_saved(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;                           // nothing saved yet
    }

    load_u32(h, K_SAMPLE, &s_cur.sample_s, check_sample);
    load_u32(h, K_PUSH, &s_cur.push_s, check_push);
    load_u32(h, K_LOWV, &s_cur.low_mv, check_low_mv);

    uint16_t ws, we;
    if (nvs_get_u16(h, K_WIN_START, &ws) == ESP_OK && nvs_get_u16(h, K_WIN_END, &we) == ESP_OK) {
        if (check_window(ws, we) == NULL) {
            s_cur.window_start = ws;
            s_cur.window_end = we;
        } else {
            ESP_LOGW(TAG, "saved window invalid; using default");
        }
    }

    uint8_t clk;
    if (nvs_get_u8(h, K_CLOCK, &clk) == ESP_OK) {
        s_cur.clock_auto = clk != 0;
    }

    char host[sizeof(s_cur.listener_host)];
    uint16_t port;
    // An empty saved host means "not set": saved along with the other settings
    //   before a listener was given, so no warning.
    if (load_str(h, K_HOST, host, sizeof(host)) && host[0] != '\0'
        && nvs_get_u16(h, K_PORT, &port) == ESP_OK) {
        if (check_host(host) == NULL && check_port(port) == NULL) {
            strlcpy(s_cur.listener_host, host, sizeof(s_cur.listener_host));
            s_cur.listener_port = port;
        } else {
            ESP_LOGW(TAG, "saved listener invalid; listener not set");
        }
    }

    char ssid[sizeof(s_cur.wifi_ssid)], pass[sizeof(s_cur.wifi_pass)];
    if (load_str(h, K_SSID, ssid, sizeof(ssid)) && ssid[0] != '\0'
        && load_str(h, K_PASS, pass, sizeof(pass))) {
        if (check_wifi(ssid, pass) == NULL) {
            strlcpy(s_cur.wifi_ssid, ssid, sizeof(s_cur.wifi_ssid));
            strlcpy(s_cur.wifi_pass, pass, sizeof(s_cur.wifi_pass));
        } else {
            ESP_LOGW(TAG, "saved WiFi settings invalid; WiFi not set");
        }
    }

    nvs_close(h);
}

static const char *save_locked(void);

int settings_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    set_defaults(&s_cur);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // The NVS area is full or from an older format: erase it and start over.
        ESP_LOGW(TAG, "NVS needs erasing (%s); saved settings lost", esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err == ESP_OK) {
            err = nvs_flash_init();
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS unavailable (%s); using defaults, changes won't be saved",
                 esp_err_to_name(err));
        return -1;
    }
    s_nvs_ok = true;
    load_saved();

    // Saved by older firmware, which set the two separately: make the push
    // interval the next whole multiple of the sample interval (within 24 h),
    // and save the fix so it's reported once.
    if (check_interval(s_cur.sample_s, s_cur.push_s) != NULL) {
        uint32_t s = s_cur.sample_s;
        uint32_t p = (s_cur.push_s + s - 1) / s * s;
        if (p > SETTINGS_PUSH_MAX_S) {
            p = SETTINGS_PUSH_MAX_S / s * s;
        }
        ESP_LOGW(TAG, "saved push interval %lu s isn't a multiple of the %lu s sample interval; "
                 "now %lu s", (unsigned long)s_cur.push_s, (unsigned long)s, (unsigned long)p);
        s_cur.push_s = p;
        const char *r = save_locked();
        if (r) {
            ESP_LOGW(TAG, "%s", r);
        }
    }
    return 0;
}

bool settings_saved_to_flash(void)
{
    return s_nvs_ok;
}

void settings_get(settings_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_cur;
    xSemaphoreGive(s_lock);
}

// Save the whole settings record. Called with s_lock held, after s_cur has
// been updated. Returns NULL on success.
static const char *save_locked(void)
{
    if (!s_nvs_ok) {
        return "applied, but not saved: NVS unavailable";
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return "applied, but not saved: NVS open failed";
    }
    esp_err_t err = ESP_OK;
    if (err == ESP_OK) err = nvs_set_u32(h, K_PUSH, s_cur.push_s);
    if (err == ESP_OK) err = nvs_set_u32(h, K_SAMPLE, s_cur.sample_s);
    if (err == ESP_OK) err = nvs_set_u32(h, K_LOWV, s_cur.low_mv);
    if (err == ESP_OK) err = nvs_set_u16(h, K_WIN_START, s_cur.window_start);
    if (err == ESP_OK) err = nvs_set_u16(h, K_WIN_END, s_cur.window_end);
    if (err == ESP_OK) err = nvs_set_u8(h, K_CLOCK, s_cur.clock_auto ? 1 : 0);
    if (err == ESP_OK) err = nvs_set_str(h, K_HOST, s_cur.listener_host);
    if (err == ESP_OK) err = nvs_set_u16(h, K_PORT, s_cur.listener_port);
    if (err == ESP_OK) err = nvs_set_str(h, K_SSID, s_cur.wifi_ssid);
    if (err == ESP_OK) err = nvs_set_str(h, K_PASS, s_cur.wifi_pass);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(err));
        return "applied, but not saved: NVS write failed";
    }
    return NULL;
}

const char *settings_set_interval(uint32_t sample_s, uint32_t push_s)
{
    const char *bad = check_interval(sample_s, push_s);
    if (bad) return bad;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cur.sample_s = sample_s;
    s_cur.push_s = push_s;
    const char *r = save_locked();
    xSemaphoreGive(s_lock);
    return r;
}

const char *settings_set_window(uint16_t start_min, uint16_t end_min)
{
    const char *bad = check_window(start_min, end_min);
    if (bad) return bad;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cur.window_start = start_min;
    s_cur.window_end = end_min;
    const char *r = save_locked();
    xSemaphoreGive(s_lock);
    return r;
}

const char *settings_set_clock_auto(bool on)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cur.clock_auto = on;
    const char *r = save_locked();
    xSemaphoreGive(s_lock);
    return r;
}

const char *settings_set_listener(const char *host, uint16_t port)
{
    const char *bad = check_host(host);
    if (!bad) bad = check_port(port);
    if (bad) return bad;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_cur.listener_host, host, sizeof(s_cur.listener_host));
    s_cur.listener_port = port;
    const char *r = save_locked();
    xSemaphoreGive(s_lock);
    return r;
}

const char *settings_set_wifi(const char *ssid, const char *pass)
{
    const char *bad = check_wifi(ssid, pass);
    if (bad) return bad;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_cur.wifi_ssid, ssid, sizeof(s_cur.wifi_ssid));
    strlcpy(s_cur.wifi_pass, pass, sizeof(s_cur.wifi_pass));
    const char *r = save_locked();
    xSemaphoreGive(s_lock);
    return r;
}

const char *settings_set_low_mv(uint32_t mv)
{
    const char *bad = check_low_mv(mv);
    if (bad) return bad;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_cur.low_mv = mv;
    const char *r = save_locked();
    xSemaphoreGive(s_lock);
    return r;
}

#ifdef AMBIENT_TEST_FAULTS
#define K_TEST_RTC "t_rtc_absent"

bool settings_test_rtc_absent(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (s_nvs_ok && nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, K_TEST_RTC, &v);
        nvs_close(h);
    }
    return v != 0;
}

const char *settings_set_test_rtc_absent(bool on)
{
    nvs_handle_t h;
    if (!s_nvs_ok || nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return "not saved: NVS unavailable";
    }
    esp_err_t err = nvs_set_u8(h, K_TEST_RTC, on ? 1 : 0);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK ? NULL : "not saved: NVS write failed";
}
#endif
