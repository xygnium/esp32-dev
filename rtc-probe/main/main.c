// DS3231 real-time clock probe, kept as a permanent diagnostic. Runs once after reset.
// I2C bus 0: SDA = D21, SCL = D22. DS3231 at 0x68.
// Reads the clock twice, about 2 s apart, and checks that:
//   - the date and time are valid (BCD digits and ranges),
//   - the oscillator-stop flag (OSF) is clear (set means the clock lost power
//     and its time can't be trusted),
//   - the seconds advanced by about 2 between the reads.
// Also prints the chip's temperature. Prints PASS or FAIL at the end.
// The clock is never set unless SET_FROM_BUILD is 1. That sets UTC from the build
//   time and clears OSF. Flash it once, then set it back to 0 and reflash, or the
//   clock is reset on every boot. The clock is kept in UTC, so there is no DST shift.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define PIN_SDA         GPIO_NUM_21
#define PIN_SCL         GPIO_NUM_22
#define SCL_HZ          100000
#define DS3231_ADDR     0x68
#define XFER_TIMEOUT_MS 100
#define SET_FROM_BUILD  0

#define REG_SECONDS     0x00
#define REG_STATUS      0x0F
#define REG_TEMP_MSB    0x11
#define STATUS_OSF      0x80
#define NUM_REGS        0x13    // 0x00 to 0x12

typedef struct {
    int year, month, day, hour, minute, second, dow;
    float temp_c;
    bool osf;
} rtc_time_t;

static int from_bcd(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
#if SET_FROM_BUILD
static uint8_t to_bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
#endif

static bool bcd_ok(uint8_t v)
{
    return (v >> 4) <= 9 && (v & 0x0F) <= 9;
}

// Reads registers 0x00-0x12 and decodes them. Returns false on a bus error or
//   an impossible value.
static bool read_clock(i2c_master_dev_handle_t dev, rtc_time_t *t)
{
    uint8_t reg = REG_SECONDS;
    uint8_t r[NUM_REGS];
    esp_err_t err = i2c_master_transmit_receive(dev, &reg, 1, r, sizeof r, XFER_TIMEOUT_MS);
    if (err != ESP_OK) {
        printf("  FAIL: I2C read: %s\n", esp_err_to_name(err));
        return false;
    }
    for (int i = 0; i <= 6; i++) {
        uint8_t v = r[i];
        if (i == 2) v &= 0x3F;          // hours: drop the 12/24 and AM/PM bits
        else if (i == 5) v &= 0x1F;     // month: drop the century bit
        else if (i == 3) v &= 0x07;
        else if (i == 0 || i == 1) v &= 0x7F;
        if (!bcd_ok(v)) {
            printf("  FAIL: register 0x%02x holds 0x%02x, not valid BCD\n", i, r[i]);
            return false;
        }
    }
    t->second = from_bcd(r[0] & 0x7F);
    t->minute = from_bcd(r[1] & 0x7F);
    if (r[2] & 0x40) {                  // 12-hour mode
        t->hour = from_bcd(r[2] & 0x1F) % 12 + ((r[2] & 0x20) ? 12 : 0);
    } else {
        t->hour = from_bcd(r[2] & 0x3F);
    }
    t->dow = r[3] & 0x07;
    t->day = from_bcd(r[4]);
    t->month = from_bcd(r[5] & 0x1F);
    t->year = 2000 + from_bcd(r[6]);
    t->temp_c = (int8_t)r[REG_TEMP_MSB] + (r[REG_TEMP_MSB + 1] >> 6) * 0.25f;
    t->osf = (r[REG_STATUS] & STATUS_OSF) != 0;

    if (t->second > 59 || t->minute > 59 || t->hour > 23 ||
        t->month < 1 || t->month > 12 || t->day < 1 || t->day > 31) {
        printf("  FAIL: time out of range: %04d-%02d-%02d %02d:%02d:%02d\n",
               t->year, t->month, t->day, t->hour, t->minute, t->second);
        return false;
    }
    return true;
}

#if SET_FROM_BUILD
// Sets the clock to the build time in UTC, 24-hour mode, and clears OSF.
//   __DATE__ and __TIME__ are the build machine's local time. BUILD_TZ_OFFSET
//   (from CMakeLists.txt, for example "-0400") converts them to UTC. The time is
//   the moment main.c was compiled, so run `touch main/main.c && ./flash.sh` to
//   keep the gap to the flash short.
#ifndef BUILD_TZ_OFFSET
#define BUILD_TZ_OFFSET "+0000"
#endif
static bool set_from_build(i2c_master_dev_handle_t dev)
{
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char mon[4] = { 0 };
    int day, year, hh, mm, ss;
    sscanf(__DATE__, "%3s %d %d", mon, &day, &year);
    sscanf(__TIME__, "%d:%d:%d", &hh, &mm, &ss);
    const char *p = strstr(months, mon);
    int month = p ? (int)(p - months) / 3 + 1 : 1;

    int off_h = 0, off_m = 0;
    sscanf(BUILD_TZ_OFFSET + 1, "%2d%2d", &off_h, &off_m);
    long offset_s = (off_h * 3600L + off_m * 60L) * (BUILD_TZ_OFFSET[0] == '-' ? -1 : 1);

    // The ESP32's default time zone is UTC, so mktime() here works as timegm().
    struct tm local = { .tm_year = year - 1900, .tm_mon = month - 1, .tm_mday = day,
                        .tm_hour = hh, .tm_min = mm, .tm_sec = ss };
    time_t t = mktime(&local) - offset_s;
    struct tm utc;
    gmtime_r(&t, &utc);
    printf("  setting the clock to %04d-%02d-%02d %02d:%02d:%02d UTC (build time, offset %s)\n",
           utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min,
           utc.tm_sec, BUILD_TZ_OFFSET);

    uint8_t buf[8] = { REG_SECONDS, to_bcd(utc.tm_sec), to_bcd(utc.tm_min),
                       to_bcd(utc.tm_hour), (uint8_t)(utc.tm_wday + 1),
                       to_bcd(utc.tm_mday), to_bcd(utc.tm_mon + 1),
                       to_bcd((utc.tm_year + 1900) % 100) };
    if (i2c_master_transmit(dev, buf, sizeof buf, XFER_TIMEOUT_MS) != ESP_OK) {
        return false;
    }
    uint8_t clr[2] = { REG_STATUS, 0x00 };
    return i2c_master_transmit(dev, clr, sizeof clr, XFER_TIMEOUT_MS) == ESP_OK;
}
#endif

static void print_time(const char *label, const rtc_time_t *t)
{
    printf("  %s %04d-%02d-%02d %02d:%02d:%02d  (weekday register %d)\n", label,
           t->year, t->month, t->day, t->hour, t->minute, t->second, t->dow);
}

void app_main(void)
{
    printf("\nrtc probe: DS3231 at 0x%02x, SDA = GPIO %d, SCL = GPIO %d\n",
           DS3231_ADDR, PIN_SDA, PIN_SCL);

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = 0,
        .sda_io_num = PIN_SDA,
        .scl_io_num = PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK) {
        printf("  FAIL: I2C bus init\nrtc probe: FAIL\n");
        return;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = DS3231_ADDR,
        .scl_speed_hz = SCL_HZ,
    };
    i2c_master_dev_handle_t dev;
    if (i2c_master_bus_add_device(bus, &dev_cfg, &dev) != ESP_OK) {
        printf("  FAIL: add device\nrtc probe: FAIL\n");
        return;
    }

#if SET_FROM_BUILD
    printf("  set: %s\n", set_from_build(dev) ? "done" : "FAILED");
#endif

    bool ok = true;
    rtc_time_t a, b;
    if (!read_clock(dev, &a)) {
        printf("rtc probe: FAIL\n");
        return;
    }
    print_time("first read: ", &a);
    printf("  chip temperature: %.2f C\n", a.temp_c);

    if (a.osf) {
        printf("  FAIL: oscillator-stop flag is set: the clock lost power, so its time is not trustworthy\n");
        printf("        set the clock (see SET_FROM_BUILD), which also clears the flag\n");
        ok = false;
    } else {
        printf("  oscillator-stop flag: clear\n");
    }

    vTaskDelay(pdMS_TO_TICKS(2100));
    if (!read_clock(dev, &b)) {
        printf("rtc probe: FAIL\n");
        return;
    }
    print_time("second read:", &b);

    int dt = (b.second - a.second + 60) % 60;
    if (dt < 1 || dt > 3) {
        printf("  FAIL: seconds advanced by %d in about 2.1 s: the clock is not running\n", dt);
        ok = false;
    } else {
        printf("  seconds advanced by %d in about 2.1 s: OK\n", dt);
    }

    printf("rtc probe: %s\n", ok ? "PASS" : "FAIL");
}
