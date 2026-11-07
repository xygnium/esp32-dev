#include "console_cmds.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "ambient.h"
#include "clock.h"
#include "datalog.h"
#include "fault.h"
#include "sd.h"
#include "sensors.h"
#include "settings.h"
#include "supply.h"
#include "wifi.h"

#define CONSOLE_STACK 6144

static TaskHandle_t s_console_task;     // known from the first `status`

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

static void print_listener(const settings_t *cfg)
{
    if (cfg->listener_host[0] == '\0') {
        printf("listener   not set (config listener <ip> <port>)\n");
    } else {
        printf("listener   %s:%u\n", cfg->listener_host, cfg->listener_port);
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

// --- sensors ---

static void print_utc(int64_t us)
{
    time_t sec = (time_t)(us / 1000000);
    struct tm t;
    gmtime_r(&sec, &t);
    printf("%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
           t.tm_hour, t.tm_min, t.tm_sec, (int)(us % 1000000 / 1000));
}

static void print_values(sensor_id_t id, const double *v)
{
    switch (id) {
    case SENSOR_THERMISTOR: printf("%.2f C, ratio %.5f", v[0], v[1]); break;
    case SENSOR_SHT45:      printf("%.2f C, %.1f %%RH", v[0], v[1]); break;
    case SENSOR_BMP388:     printf("%.2f C, %.2f hPa", v[0], v[1] / 100.0); break;
    default:                break;
    }
}

// One line per sensor: its last values and stamp, or its last failure.
static void print_sensors(void)
{
    for (int i = 0; i < SENSOR_COUNT; i++) {
        sensor_reading_t r;
        sensors_get((sensor_id_t)i, &r);
        printf("%-10s ", sensor_name((sensor_id_t)i));
        if (!r.read) {
            printf("not read yet\n");
            continue;
        }
        if (!r.ok) {
            printf("FAILED: %s (%" PRIu32 " in a row)", r.err, r.fails);
            if (r.utc_us) {
                printf(", last good ");
                print_utc(r.utc_us);
            }
            printf("\n");
            continue;
        }
        print_values((sensor_id_t)i, r.value);
        printf(" at ");
        print_utc(r.utc_us);
        printf("\n");
    }
}

static int cmd_read(int argc, char **argv)
{
    if (!clock_is_set()) {
        printf("clock not set: readings would have no time stamp\n");
        return 1;
    }
    sensors_sample(NULL);                 // a diagnostic: not logged
    print_sensors();
    return 0;
}

// tail [file] [lines]: the end of a file on the card, or the file list.
static int cmd_tail(int argc, char **argv)
{
    if (argc == 1) {
        return datalog_list() == 0 ? 0 : 1;
    }
    uint32_t lines = 10;
    if (argc > 3 || (argc == 3 && (!parse_u32(argv[2], &lines) || lines < 1 || lines > 200))) {
        printf("usage: tail [file [lines]]   (lines 1 to 200, default 10; no file: list the card)\n");
        return 1;
    }
    return datalog_tail(argv[1], (int)lines) == 0 ? 0 : 1;
}

static int cmd_status(int argc, char **argv)
{
    settings_t cfg;
    push_stats_t ps;
    settings_get(&cfg);
    ambient_get_push_stats(&ps);

    fault_src_t fsrc;
    const char *freason;
    if (fault_get(&fsrc, &freason)) {
        printf("FAULT      %s: %s (normal work stopped; fix it and reboot)\n", fault_src_name(fsrc), freason);
    }
    printf("uptime     %" PRIu64 " s\n", (uint64_t)(esp_timer_get_time() / 1000000));
    clock_info_t ck;
    clock_get_info(&ck);
    if (fault_get(&fsrc, &freason) && fsrc == FAULT_RTC) {
        printf("clock      FAULT: %s\n", freason);
    } else if (!ck.set) {
        printf("clock      not set (waiting for ack)\n");
    } else {
        char now[40];
        clock_now_str(now, sizeof now);
        printf("clock      %s, DS3231 synced %lld s ago", now,
               (long long)(esp_timer_get_time() / 1000000 - ck.last_sync_up_s));
        if (ck.ack_seen) {
            printf(", last ack offset %+ld ms (round trip %ld ms)", (long)ck.ack_offset_ms,
                   (long)ck.ack_rtt_ms);
        }
        if (ck.correction_up_s >= 0) {
            printf(", DS3231 corrected %+ld ms at uptime %lld s", (long)ck.correction_ms,
                   (long long)ck.correction_up_s);
        }
        printf("\n");
    }

    if (wifi_is_connected()) {
        char ip[16], ap[18];
        int rssi = 0;
        wifi_ip_str(ip, sizeof(ip));
        wifi_bssid_str(ap, sizeof(ap));
        wifi_rssi(&rssi);
        printf("wifi       connected to %s, rssi %d dBm, ap %s, ip %s, reconnects %" PRIu32 "\n",
               cfg.wifi_ssid, rssi, ap, ip, wifi_reconnect_count());
    } else if (cfg.wifi_ssid[0] == '\0') {
        printf("wifi       not set (config wifi <ssid> <password>)\n");
    } else {
        printf("wifi       not connected (network %s), reconnects %" PRIu32 "\n",
               cfg.wifi_ssid, wifi_reconnect_count());
    }

    print_listener(&cfg);
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
    print_sensors();
    sd_info_t sd;
    sd_get_info(&sd);
    if (fault_get(&fsrc, &freason) && fsrc == FAULT_SD) {
        printf("sd         FAULT: %s\n", freason);
    } else if (!sd.mounted) {
        printf("sd         not mounted\n");
    } else if (sd.size_ok) {
        printf("sd         mounted, %llu MB, %llu MB free, %s at boot\n",
               (unsigned long long)(sd.total_bytes >> 20), (unsigned long long)(sd.free_bytes >> 20),
               sd.empty ? "empty" : "not empty");
    } else {
        printf("sd         mounted (size unknown), %s at boot\n", sd.empty ? "empty" : "not empty");
    }
    datalog_info_t dl;
    datalog_get_info(&dl);
    if (dl.low) {
        printf("           LOW: under 10 %% free\n");
    }
    if (dl.file[0]) {
        printf("log        %s, %" PRIu32 " bytes; %" PRIu32 " rows, %" PRIu32 " events since boot",
               dl.file, dl.file_bytes, dl.rows, dl.events);
    } else {
        printf("log        nothing written yet; %" PRIu32 " events since boot", dl.events);
    }
    if (dl.held) {
        printf(", %" PRIu32 " held for the clock", dl.held);
    }
    if (dl.dropped) {
        printf(", %" PRIu32 " lost", dl.dropped);
    }
#ifdef AMBIENT_TEST_FAULTS
    if (datalog_test_minute_files_on()) {
        printf(" (TEST: a file per minute)");
    }
#endif
    printf("\n");
    if (!s_console_task) {
        s_console_task = xTaskGetCurrentTaskHandle();   // status runs in the console task
    }
    printf("stack      lowest free since boot: main %u B of %u, console %u B of %u\n",
           (unsigned)uxTaskGetStackHighWaterMark(ambient_main_task()), (unsigned)CONFIG_ESP_MAIN_TASK_STACK_SIZE,
           (unsigned)uxTaskGetStackHighWaterMark(s_console_task), (unsigned)CONSOLE_STACK);
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
#ifdef AMBIENT_TEST_FAULTS
    printf("build      TEST: fault injection enabled; not for production\n");
#endif
    return 0;
}

#ifdef AMBIENT_TEST_FAULTS
// --- test (test builds only) ---

// Raise a fault through the same path as a real one.
static int cmd_test(int argc, char **argv)
{
    static const char *const sd_reasons[] = { "missing", "mount_failed", "not_responding",
                                              "full", "write_failed", NULL };
    static const char *const rtc_reasons[] = { "missing", "invalid", "not_responding", NULL };
    if (argc == 4 && strcmp(argv[1], "fault") == 0) {
        bool sd = strcmp(argv[2], "sd") == 0, rtc = strcmp(argv[2], "rtc") == 0;
        const char *const *reasons = sd ? sd_reasons : rtc ? rtc_reasons : NULL;
        for (size_t i = 0; reasons && reasons[i]; i++) {
            if (strcmp(argv[3], reasons[i]) == 0) {
                fault_raise(sd ? FAULT_SD : FAULT_RTC, reasons[i]);
                ambient_wake();       // push the fault line now
                return 0;
            }
        }
    }
    if (argc == 3 && strcmp(argv[1], "rtc-absent") == 0
        && (strcmp(argv[2], "on") == 0 || strcmp(argv[2], "off") == 0)) {
        bool on = strcmp(argv[2], "on") == 0;
        int rc = report(settings_set_test_rtc_absent(on));
        if (rc == 0) {
            printf("DS3231 %s at the next boot; reboot to apply\n",
                   on ? "simulated missing" : "looked for normally");
        }
        return rc;
    }
    uint32_t secs;
    if (argc == 3 && strcmp(argv[1], "resync") == 0 && parse_u32(argv[2], &secs) && secs >= 10) {
        clock_test_set_resync(secs);
        printf("DS3231 resync every %" PRIu32 " s until reboot\n", secs);
        ambient_wake();
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "rtc-stop") == 0) {
        if (clock_test_stop_on_battery() != 0) {
            printf("DS3231 didn't answer\n");
            return 1;
        }
        printf("DS3231 oscillator set to stop on the battery. Unplug the power for a few\n"
               "seconds and plug it back in: the stop flag is then really set. The next\n"
               "boot clears the setting. Not a fault until then\n");
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "dayfile") == 0
        && (strcmp(argv[2], "minute") == 0 || strcmp(argv[2], "day") == 0)) {
        bool minute = strcmp(argv[2], "minute") == 0;
        datalog_test_minute_files(minute);
        printf("a readings file per %s until reboot\n", minute ? "UTC minute" : "UTC day");
        return 0;
    }
    printf("usage: test fault sd missing|mount_failed|not_responding|full|write_failed\n"
           "       test fault rtc missing|invalid|not_responding\n"
           "       test rtc-absent on|off   (at boot, look for the DS3231 at an unused address)\n"
           "       test resync <seconds>    (resync from the DS3231 this often, 10 s or more, until reboot)\n"
           "       test dayfile minute|day  (a readings file per UTC minute or day, until reboot)\n"
           "       test rtc-stop            (DS3231 stops on the battery: a power cut sets its stop flag)\n");
    return 1;
}
#endif

// --- config ---

static void config_print(void)
{
    settings_t cfg;
    settings_get(&cfg);
    printf("interval   sample ");
    print_duration(cfg.sample_s);
    printf(", push ");
    print_duration(cfg.push_s);
    printf("\n");
    printf("window     %02u:%02u-%02u:%02u UTC   (not used yet)\n",
           cfg.window_start / 60, cfg.window_start % 60, cfg.window_end / 60, cfg.window_end % 60);
    printf("clock      %s\n", cfg.clock_auto ? "auto" : "report");
    print_listener(&cfg);
    printf("lowv       %lu.%02lu V   (supply-low threshold)\n", (unsigned long)(cfg.low_mv / 1000),
           (unsigned long)(cfg.low_mv % 1000 / 10));
    if (cfg.wifi_ssid[0] == '\0') {
        printf("wifi       not set\n");
    } else {
        printf("wifi       %s, password %s\n", cfg.wifi_ssid, cfg.wifi_pass[0] ? "set (hidden)" : "none");
    }
}

static int config_usage(void)
{
    printf("usage:\n"
           "  config get\n"
           "  config interval <sample> <push>  e.g. 2m 1h; units s, m or h, bare number = seconds.\n"
           "                                sample 15 s-1 h; push up to 24 h, a whole multiple of sample\n"
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
    if (strcmp(what, "interval") == 0 && argc == 4) {
        uint32_t sample, push;
        for (int i = 2; i < 4; i++) {
            if (!parse_duration(argv[i], 1, i == 2 ? &sample : &push)) {
                printf("not a duration: %s\n", argv[i]);
                return 1;
            }
        }
        int rc = report(settings_set_interval(sample, push));
        ambient_wake();               // re-plan the next push with the new interval
        return rc;
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
        { .command = "status", .help = "Running time, clock, WiFi, pushes, sensors, SD card", .func = cmd_status },
        { .command = "info",   .help = "Firmware version, build date, MAC address",  .func = cmd_info },
        { .command = "config", .help = "Show or change settings; 'config' alone lists the forms",
          .hint = "get | interval | window | clock | listener | lowv | wifi ...", .func = cmd_config },
        { .command = "push",   .help = "Push now, outside the schedule",              .func = cmd_push },
        { .command = "read",   .help = "Read the sensors now and print the readings", .func = cmd_read },
        { .command = "tail",   .help = "Last lines of a file on the SD card; alone, list the card's files",
          .hint = "[file [lines]]", .func = cmd_tail },
        { .command = "quiet",  .help = "Hide (on) or show (off) successful pushes on this console; on at boot",
          .hint = "[on|off]", .func = cmd_quiet },
        { .command = "reboot", .help = "Restart the logger; settings are kept",       .func = cmd_reboot },
#ifdef AMBIENT_TEST_FAULTS
        { .command = "test",   .help = "TEST BUILD: raise a hard fault as if it were real",
          .hint = "fault sd|rtc <reason> | rtc-absent on|off | resync <s> | dayfile minute|day | rtc-stop",
          .func = cmd_test },
#endif
    };

    esp_err_t err = esp_console_register_help_command();
    for (size_t i = 0; err == ESP_OK && i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        err = esp_console_cmd_register(&cmds[i]);
    }

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t rc = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    rc.prompt = "ambient>";
    rc.task_stack_size = CONSOLE_STACK;   // `read` and event logging write to the card
    if (err == ESP_OK) err = esp_console_new_repl_stdio(&rc, &repl);
    if (err == ESP_OK) err = esp_console_start_repl(repl);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "console failed to start: %s; carrying on without it", esp_err_to_name(err));
        return -1;
    }
    return 0;
}
