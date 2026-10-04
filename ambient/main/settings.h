#ifndef AMBIENT_SETTINGS_H
#define AMBIENT_SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

// Logger settings, kept in NVS (non-volatile storage: the ESP32's small
// key-value settings area in flash) so they survive a restart and an SD card
// swap. Every setting has a default; a missing or out-of-range saved value
// falls back to its default instead of stopping the logger.
//
// Defaults for WiFi and the listener come from wifi_secrets.h; once a value
// is changed by command, the saved value wins.

#define SETTINGS_PUSH_MIN_S     10u
#define SETTINGS_PUSH_MAX_S     86400u   // 24 h
#define SETTINGS_SAMPLE_MIN_S   1u
#define SETTINGS_SAMPLE_MAX_S   3600u
#define SETTINGS_DAY_MIN        1440u    // minutes in a day; 24:00

typedef struct {
    char wifi_ssid[33];       // 1-32 characters
    char wifi_pass[64];       // empty (open network) or 8-63 characters
    char listener_host[16];   // dotted-quad IPv4
    uint16_t listener_port;
    uint32_t push_s;          // seconds between pushes
    uint32_t sample_s;        // seconds between samples (used once sensors exist)
    uint16_t window_start;    // push window, minutes after 00:00 UTC (0-1439)
    uint16_t window_end;      // minutes after 00:00 UTC (1-1440); end < start wraps past midnight
    bool clock_auto;          // true: the ack's time corrects the clock; false: only reported
} settings_t;

// Initialise NVS and load the saved settings over the defaults. Call once,
// before wifi_connect() (WiFi also keeps data in NVS). Returns 0 when NVS is
// usable; -1 when it isn't, in which case the defaults are in force and
// changes apply only until the next restart.
int settings_init(void);

// True when changes are being saved to flash.
bool settings_saved_to_flash(void);

// Copy of the current settings (safe to call from any task).
void settings_get(settings_t *out);

// Setters validate, apply immediately, and save. Each returns NULL on
// success, or a message saying why the value was refused (nothing changed)
// or that it was applied but couldn't be saved.
const char *settings_set_push(uint32_t seconds);
const char *settings_set_sample(uint32_t seconds);
const char *settings_set_window(uint16_t start_min, uint16_t end_min);
const char *settings_set_clock_auto(bool on);
const char *settings_set_listener(const char *host, uint16_t port);
const char *settings_set_wifi(const char *ssid, const char *pass);

#endif
