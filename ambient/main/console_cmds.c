#include "console_cmds.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "ambient.h"
#include "settings.h"
#include "supply.h"
#include "wifi.h"

static const char *TAG = "console";

// --- parsing helpers ---

// Parse a whole non-negative integer; false on anything else.
static bool parse_u32(const char *s, uint32_t *out)
{
    char *end;
    if (*s == '\0' || *s == '-') {
        return false;
    }
    unsigned long v = strtoul(s, &end, 10);
    if (*end != '\0' || v > UINT32_MAX) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

// Parse a duration: a number with an optional unit s, m or h. A bare number
// is taken in default_unit_s seconds (60 for minutes, 1 for seconds).
static bool parse_duration(const char *s, uint32_t default_unit_s, uint32_t *out_s)
{
    char num[16];
    size_t len = strlen(s);
    if (len == 0 || len >= sizeof(num)) {
        return false;
    }
    strlcpy(num, s, sizeof(num));
    uint32_t unit = default_unit_s;
    char last = num[len - 1];
    if (last == 's' || last == 'm' || last == 'h') {
        unit = last == 's' ? 1 : last == 'm' ? 60 : 3600;
        num[len - 1] = '\0';
    }
    uint32_t v;
    if (!parse_u32(num, &v) || v > UINT32_MAX / unit) {
        return false;
    }
    *out_s = v * unit;
    return true;
}

// Parse HH:MM into minutes after midnight; 24:00 is allowed (end of day).
static bool parse_hhmm(const char *s, uint16_t *out_min)
{
    unsigned h, m;
    char extra;
    if (sscanf(s, "%u:%u%c", &h, &m, &extra) != 2 || m > 59 || h > 24 || (h == 24 && m != 0)) {
        return false;
    }
    *out_min = (uint16_t)(h * 60 + m);
    return true;
}

// Parse volts like "4.6" or "4.60" or "5" into millivolts.
static bool parse_volts(const char *s, uint32_t *out_mv)
{
    unsigned whole = 0, frac = 0, frac_digits = 0;
    const char *p = s;
    if (*p < '0' || *p > '9') {
        return false;
    }
    while (*p >= '0' && *p <= '9') {
        whole = whole * 10 + (*p++ - '0');
        if (whole > 99) return false;
    }
    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9') {
            if (frac_digits == 3) return false;
            frac = frac * 10 + (*p++ - '0');
            frac_digits++;
        }
    }
    if (*p != '\0') {
        return false;
    }
    while (frac_digits < 3) {
        frac *= 10;
        frac_digits++;
    }
    *out_mv = whole * 1000 + frac;
    return true;
}

static void print_duration(uint32_t s)
{
    if (s % 3600 == 0) {
        printf("%" PRIu32 " h", s / 3600);
    } else if (s % 60 == 0) {
        printf("%" PRIu32 " min", s / 60);
    } else {
        printf("%" PRIu32 " s", s);
    }
}

// Print the setter's result: NULL means done.
static int report(const char *err)
{
    if (err == NULL) {
        printf("ok\n");
        return 0;
    }
    printf("%s\n", err);
    // "applied, but not saved" is a partial success; anything else changed nothing.
    return strncmp(err, "applied", 7) == 0 ? 0 : 1;
}

// --- status ---

static const char *result_name(push_result_t r)
{
    switch (r) {
    case PUSH_NONE:          return "none yet";
    case PUSH_OK:            return "ok";
    case PUSH_NO_REPLY:      return "no reply";
    case PUSH_SEND_FAILED:   return "send failed";
    case PUSH_NOT_CONNECTED: return "not connected";
    }
    return "?";
}

static int cmd_status(int argc, char **argv)
{
    settings_t cfg;
    push_stats_t ps;
    settings_get(&cfg);
    ambient_get_push_stats(&ps);

    printf("uptime     %" PRIu64 " s\n", (uint64_t)(esp_timer_get_time() / 1000000));

    if (wifi_is_connected()) {
        char ip[16], ap[18];
        int rssi = 0;
        wifi_ip_str(ip, sizeof(ip));
        wifi_bssid_str(ap, sizeof(ap));
        wifi_rssi(&rssi);
        printf("wifi       connected to %s, rssi %d dBm, ap %s, ip %s, reconnects %" PRIu32 "\n",
               cfg.wifi_ssid, rssi, ap, ip, wifi_reconnect_count());
    } else {
        printf("wifi       not connected (network %s), reconnects %" PRIu32 "\n",
               cfg.wifi_ssid, wifi_reconnect_count());
    }

    printf("listener   %s:%u\n", cfg.listener_host, cfg.listener_port);
    printf("push       every ");
    print_duration(cfg.push_s);
    printf(", next in %" PRIu32 " s\n", ps.next_in_s);
    if (ps.last == PUSH_NONE) {
        printf("last push  none yet\n");
    } else {
        printf("last push  seq %" PRIu32 ": %s at uptime %" PRIu32 " s", ps.seq,
               result_name(ps.last), ps.last_uptime_s);
        if (ps.last == PUSH_OK) {
            printf(" (%s)", ps.last_reply);
        }
        printf("\n");
    }
    printf("pushes     ok %" PRIu32 ", no reply %" PRIu32 ", send failed %" PRIu32
           ", not connected %" PRIu32 "\n",
           ps.n_ok, ps.n_no_reply, ps.n_send_failed, ps.n_not_connected);
    uint32_t vin_now;
    supply_check(cfg.low_mv, &vin_now);       // fresh reading for this status
    supply_state_t sup;
    supply_get(&sup);
    if (sup.valid) {
        printf("supply     %lu.%02lu V%s (low below %lu.%02lu V; went low %lu times since boot; %s)\n",
               (unsigned long)(sup.vin_mv / 1000), (unsigned long)(sup.vin_mv % 1000 / 10),
               sup.low ? " LOW" : "", (unsigned long)(cfg.low_mv / 1000), (unsigned long)(cfg.low_mv % 1000 / 10),
               (unsigned long)sup.n_low, supply_cal_name());
    } else {
        printf("supply     no reading yet\n");
    }
    printf("settings   %s\n", settings_saved_to_flash() ? "saved in flash"
                                                       : "NOT saved: NVS unavailable, defaults in use");
    return 0;
}

// --- info ---

static int cmd_info(int argc, char **argv)
{
    const esp_app_desc_t *app = esp_app_get_description();
    char mac[18];
    wifi_mac_str(mac, sizeof(mac));
    printf("firmware   %s %s, built %s %s\n", app->project_name, app->version, app->date, app->time);
    printf("esp-idf    %s\n", app->idf_ver);
    printf("mac        %s\n", mac);
    return 0;
}

// --- config ---

static void config_print(void)
{
    settings_t cfg;
    settings_get(&cfg);
    printf("push       ");
    print_duration(cfg.push_s);
    printf("\nsample     ");
    print_duration(cfg.sample_s);
    printf("   (used once sensors exist)\n");
    printf("window     %02u:%02u-%02u:%02u UTC   (enforced from stage 22)\n",
           cfg.window_start / 60, cfg.window_start % 60, cfg.window_end / 60, cfg.window_end % 60);
    printf("clock      %s   (used from stage 22)\n", cfg.clock_auto ? "auto" : "report");
    printf("listener   %s:%u\n", cfg.listener_host, cfg.listener_port);
    printf("lowv       %lu.%02lu V   (supply-low threshold)\n", (unsigned long)(cfg.low_mv / 1000),
           (unsigned long)(cfg.low_mv % 1000 / 10));
    printf("wifi       %s, password %s\n", cfg.wifi_ssid, cfg.wifi_pass[0] ? "set (hidden)" : "none");
}

static int config_usage(void)
{
    printf("usage:\n"
           "  config get\n"
           "  config push <n>[s|m|h]        push interval, bare number = minutes (10 s-24 h)\n"
           "  config sample <n>[s|m]        sample interval, bare number = seconds (1 s-1 h)\n"
           "  config window <HH:MM> <HH:MM> push window in UTC; end before start wraps midnight\n"
           "  config clock auto|report      let the ack's time correct the clock, or only report\n"
           "  config listener <ip> <port>   where to push\n"
           "  config lowv <volts>           supply-low threshold, e.g. 4.6 (3.0-5.5 V)\n"
           "  config wifi <ssid> <password> WiFi network; quote values with spaces, \"\" for none\n");
    return 1;
}

static int cmd_config(int argc, char **argv)
{
    if (argc < 2) {
        return config_usage();
    }
    const char *what = argv[1];

    if (strcmp(what, "get") == 0 && argc == 2) {
        config_print();
        return 0;
    }
    if (strcmp(what, "push") == 0 && argc == 3) {
        uint32_t s;
        if (!parse_duration(argv[2], 60, &s)) {
            printf("not a duration: %s\n", argv[2]);
            return 1;
        }
        int rc = report(settings_set_push(s));
        ambient_wake();               // re-plan the next push with the new interval
        return rc;
    }
    if (strcmp(what, "sample") == 0 && argc == 3) {
        uint32_t s;
        if (!parse_duration(argv[2], 1, &s)) {
            printf("not a duration: %s\n", argv[2]);
            return 1;
        }
        return report(settings_set_sample(s));
    }
    if (strcmp(what, "window") == 0 && argc == 4) {
        uint16_t a, b;
        if (!parse_hhmm(argv[2], &a) || !parse_hhmm(argv[3], &b)) {
            printf("times must be HH:MM\n");
            return 1;
        }
        return report(settings_set_window(a, b));
    }
    if (strcmp(what, "clock") == 0 && argc == 3) {
        if (strcmp(argv[2], "auto") == 0) {
            return report(settings_set_clock_auto(true));
        }
        if (strcmp(argv[2], "report") == 0) {
            return report(settings_set_clock_auto(false));
        }
        printf("clock must be auto or report\n");
        return 1;
    }
    if (strcmp(what, "listener") == 0 && argc == 4) {
        uint32_t port;
        if (!parse_u32(argv[3], &port) || port > 65535) {
            printf("port must be 1-65535\n");
            return 1;
        }
        int rc = report(settings_set_listener(argv[2], (uint16_t)port));
        ambient_wake();
        return rc;
    }
    if (strcmp(what, "lowv") == 0 && argc == 3) {
        uint32_t mv;
        if (!parse_volts(argv[2], &mv)) {
            printf("not a voltage: %s\n", argv[2]);
            return 1;
        }
        return report(settings_set_low_mv(mv));
    }
    if (strcmp(what, "wifi") == 0 && argc == 4) {
        int rc = report(settings_set_wifi(argv[2], argv[3]));
        if (rc == 0) {
            // A typo here cuts WiFi until it's corrected on this console.
            if (wifi_set_credentials(argv[2], argv[3]) == 0) {
                printf("reconnecting to %s\n", argv[2]);
            } else {
                printf("saved; WiFi isn't running, takes effect after reboot\n");
            }
        }
        return rc;
    }
    return config_usage();
}

// --- quiet ---

static int cmd_quiet(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "on") == 0) {
        ambient_set_quiet(true);
    } else if (argc == 2 && strcmp(argv[1], "off") == 0) {
        ambient_set_quiet(false);
    } else if (argc != 1) {
        printf("usage: quiet [on|off]\n");
        return 1;
    }
    printf("quiet %s: successful pushes %s\n", ambient_quiet() ? "on" : "off",
           ambient_quiet() ? "hidden (problems still shown)" : "shown");
    return 0;
}

// --- push, reboot ---

static int cmd_push(int argc, char **argv)
{
    ambient_request_push();
    printf("push requested\n");
    return 0;
}

static int cmd_reboot(int argc, char **argv)
{
    printf("restarting\n");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(100));   // let the line reach the terminal
    esp_restart();
    return 0;
}

int console_start(void)
{
    const esp_console_cmd_t cmds[] = {
        { .command = "status", .help = "Running time, WiFi, listener, push results", .func = cmd_status },
        { .command = "info",   .help = "Firmware version, build date, MAC address",  .func = cmd_info },
        { .command = "config", .help = "Show or change settings; 'config' alone lists the forms",
          .hint = "get | push | sample | window | clock | listener | lowv | wifi ...", .func = cmd_config },
        { .command = "push",   .help = "Push now, outside the schedule",              .func = cmd_push },
        { .command = "quiet",  .help = "Hide (on) or show (off) successful pushes on this console; on at boot",
          .hint = "[on|off]", .func = cmd_quiet },
        { .command = "reboot", .help = "Restart the logger; settings are kept",       .func = cmd_reboot },
    };

    esp_err_t err = esp_console_register_help_command();
    for (size_t i = 0; err == ESP_OK && i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        err = esp_console_cmd_register(&cmds[i]);
    }

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t rc = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    rc.prompt = "ambient>";
    if (err == ESP_OK) err = esp_console_new_repl_stdio(&rc, &repl);
    if (err == ESP_OK) err = esp_console_start_repl(repl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "console failed to start: %s; carrying on without it", esp_err_to_name(err));
        return -1;
    }
    return 0;
}
