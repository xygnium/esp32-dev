#ifndef AMBIENT_SETTINGS_H
#define AMBIENT_SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

// Logger settings, kept in NVS (non-volatile storage: the ESP32's small
// key-value settings area in flash) so they survive a restart and an SD card
// swap. Every setting has a default; a missing or out-of-range saved value
// falls back to its default instead of stopping the logger.
//
// WiFi and the listener have no default: they are empty ("not set") until
// set on the console (`config wifi`, `config listener`), and are then saved
// like every other setting. Nothing in the build supplies them.

// Sample interval (`config interval`). Below 15 s the thermistor's
// excitation heats it (FIRMWARE.md).
#define SETTINGS_SAMPLE_MIN_S   15u
#define SETTINGS_SAMPLE_MAX_S   3600u    // 1 h
#define SETTINGS_DAY_MIN        1440u    // minutes in a day; 24:00
#define SETTINGS_LOWV_MIN_MV    3000u
#define SETTINGS_LOWV_MAX_MV    5500u

typedef struct {
    char wifi_ssid[33];       // 1-32 characters; empty = not set
    char wifi_pass[64];       // empty (open network) or 8-63 characters
    char listener_host[16];   // dotted-quad IPv4; empty = not set
    uint16_t listener_port;   // 0 when not set
    uint32_t sample_s;        // seconds between samples (not used until sensors are read)
    uint16_t window_start;    // push window, minutes after 00:00 UTC (0-1439)
    uint16_t window_end;      // minutes after 00:00 UTC (1-1440); end < start wraps past midnight
    bool clock_auto;          // true: the ack's time corrects the clock; false: only reported
    uint32_t low_mv;          // supply-low threshold for VIN, mV
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
const char *settings_set_interval(uint32_t sample_s);
const char *settings_set_window(uint16_t start_min, uint16_t end_min);
const char *settings_set_clock_auto(bool on);
const char *settings_set_listener(const char *host, uint16_t port);
const char *settings_set_wifi(const char *ssid, const char *pass);
const char *settings_set_low_mv(uint32_t mv);


#endif
