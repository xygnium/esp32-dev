// Stage 1 tests on one ADS1115 board. Pick one with TEST.
// 1d, timing (TEST_TIMING): nothing needed on A0. Reads the Config register before
//   and after a general-call reset, then times one single-shot conversion per
//   data-rate code (DR 000-111) by polling the OS bit. DR 000 takes about 125 ms
//   on an ADS1115 and about 7.8 ms on an ADS1015.
// 1f, resolution (TEST_RESOLUTION): pot wiper on A0, PGA ±4.096 V, 8 SPS. Prints
//   each raw code, and a summary every SUMMARY_N readings. A genuine ADS1115 gives
//   odd codes and a varied low nibble as the pot turns; an ADS1015 gives only
//   multiples of 16.
// 1h, noise (TEST_NOISE): PGA ±0.256 V, 8 SPS. Prints NOISE_N readings, then mean,
//   min/max, spread and standard deviation. NOISE_MUX picks the input:
//   ADS_MUX_A0_GND with A0 jumpered to GND, or ADS_MUX_A0_A1 with A0 jumpered
//   to A1 (leaves ground wiring out of the reading).
// 1h2, inputs (TEST_INPUTS): PGA ±4.096 V, 8 SPS. Reads A0, A1, A2 and A3 against
//   GND about once a second and prints the four voltages on one line. Move the
//   pot wiper from input to input: on a genuine ADS1115 only that input's column
//   follows the pot. An ADS1114 has no A2/A3 (and no input selection), so those
//   columns can't follow their own pins. Unconnected inputs float and may read
//   anything.
// I2C bus 0: SDA = D21, SCL = D22. ADS at 0x48 (ADDR pulled low on the board).
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ads1115.h"
#include "ads_port.h"

#define TEST_TIMING     1
#define TEST_RESOLUTION 2
#define TEST_NOISE      3
#define TEST_INPUTS     4
#define TEST            TEST_INPUTS

#define I2C_PORT   0
#define PIN_SDA    GPIO_NUM_21
#define PIN_SCL    GPIO_NUM_22
#define ADS_ADDR   0x48
#define SCL_HZ     100000
#define TIMEOUT_US 500000
#define SUMMARY_N  40    // about 5 s at 8 SPS
#define LSB_UV     125   // PGA ±4.096 V
#define NOISE_N    100   // about 13 s at 8 SPS
#define NOISE_LSB_UV 7.8125  // PGA ±0.256 V
#define NOISE_MUX   ADS_MUX_A0_GND

// Nominal conversion time per DR code, in µs (1 / data rate).
// ADS1115: 8, 16, 32, 64, 128, 250, 475, 860 SPS.
// ADS1015: 128, 250, 490, 920, 1600, 2400, 3300, 3300 SPS.
// Both tables recalled, not checked against the datasheets.
static const uint32_t nominal_1115_us[8] = { 125000, 62500, 31250, 15625, 7813, 4000, 2105, 1163 };
static const uint32_t nominal_1015_us[8] = { 7813, 4000, 2041, 1087, 625, 417, 303, 303 };

static void print_config(const ads_hal_t *hal, const char *label)
{
    uint16_t cfg;
    if (ads_read_reg(hal, ADS_REG_CONFIG, &cfg) != 0) {
        printf("config (%s): read FAILED\n", label);
        return;
    }
    printf("config (%s): 0x%04x%s\n", label, cfg,
           cfg == ADS_CFG_DEFAULT ? " (power-on default)" : "");
}

// Start a single-shot conversion at DR code dr and time it.
// Returns false on a bus error or timeout; *saw_busy says whether OS ever read 0.
static bool time_conversion(const ads_hal_t *hal, unsigned dr, int64_t *us,
                            bool *saw_busy, int *polls)
{
    uint16_t cfg = (ADS_CFG_DEFAULT & ~ADS_CFG_DR_MASK) | (dr << ADS_CFG_DR_SHIFT) | ADS_CFG_OS;
    uint16_t v;

    *saw_busy = false;
    *polls = 0;
    if (ads_write_reg(hal, ADS_REG_CONFIG, cfg) != 0) {
        return false;
    }
    int64_t t0 = esp_timer_get_time();  // just after the start write completes

    // Wait for OS 1 -> 0 (conversion running), then 0 -> 1 (done).
    for (;;) {
        int64_t now = esp_timer_get_time();
        if (now - t0 > TIMEOUT_US) {
            *us = now - t0;
            return false;
        }
        if (ads_read_pointed(hal, &v) != 0) {
            return false;
        }
        (*polls)++;
        if (!(v & ADS_CFG_OS)) {
            *saw_busy = true;
        } else if (*saw_busy) {
            *us = esp_timer_get_time() - t0;
            return true;
        }
    }
}

static void timing_sweep(const ads_hal_t *hal)
{
    printf("DR  measured_us  polls  nominal_1115  nominal_1015  result\n");
    for (unsigned dr = 0; dr < 8; dr++) {
        int64_t us = 0;
        bool saw_busy;
        int polls;
        bool ok = time_conversion(hal, dr, &us, &saw_busy, &polls);
        const char *result = ok        ? "ok"
                             : !saw_busy ? "FAIL: OS never read 0"
                                         : "FAIL: timeout or bus error";
        printf("%u%u%u %11" PRId64 "  %5d  %12" PRIu32 "  %12" PRIu32 "  %s\n",
               (dr >> 2) & 1, (dr >> 1) & 1, dr & 1, us, polls,
               nominal_1115_us[dr], nominal_1015_us[dr], result);
    }
}

static void timing_test(i2c_master_bus_handle_t bus, const ads_hal_t *ads)
{
    // The ADS keeps its config across an ESP32 reset, so read it before and after
    // a general-call reset. Only after a power cycle is "at boot" the true power-on value.
    print_config(ads, "at boot");
    esp_err_t err = ads_port_general_call_reset(bus, SCL_HZ);
    printf("general-call reset: %s\n", esp_err_to_name(err));
    print_config(ads, "after reset");

    for (int pass = 1;; pass++) {
        printf("\ntiming sweep, pass %d\n", pass);
        timing_sweep(ads);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

// One conversion, then read Config back: shows the chip took the MUX/PGA/DR bits.
static void check_config(const ads_hal_t *ads, unsigned mux, unsigned pga)
{
    uint16_t expect = ADS_CFG_OS | (mux << ADS_CFG_MUX_SHIFT) |
                      (pga << ADS_CFG_PGA_SHIFT) | ADS_CFG_MODE_SINGLE |
                      (ADS_DR_8SPS << ADS_CFG_DR_SHIFT) | ADS_CFG_COMP_OFF;
    int16_t code;
    uint16_t cfg;
    int err = ads_convert(ads, mux, pga, ADS_DR_8SPS, &code);
    if (err == 0 && ads_read_reg(ads, ADS_REG_CONFIG, &cfg) == 0) {
        printf("config after first conversion: 0x%04x (expect 0x%04x)\n", cfg, expect);
    } else {
        printf("first conversion or config read FAILED (%d)\n", err);
    }
}

static void resolution_test(const ads_hal_t *ads)
{
    int16_t code;
    int err;

    check_config(ads, ADS_MUX_A0_GND, ADS_PGA_4V096);
    for (;;) {
        int16_t min = INT16_MAX, max = INT16_MIN;
        int odd = 0, errors = 0, good = 0;
        int nibble[16] = { 0 };

        for (int i = 0; i < SUMMARY_N; i++) {
            err = ads_convert(ads, ADS_MUX_A0_GND, ADS_PGA_4V096, ADS_DR_8SPS, &code);
            if (err != 0) {
                printf("conversion FAILED (%d)\n", err);
                errors++;
                continue;
            }
            printf("%6d  0x%04x  %8d uV\n", code, (uint16_t)code, code * LSB_UV);
            good++;
            if (code < min) min = code;
            if (code > max) max = code;
            odd += code & 1;
            nibble[code & 0xF]++;
        }

        printf("--- %d readings: min %d  max %d  odd %d  errors %d\n",
               good, min, max, odd, errors);
        printf("--- low nibble:");
        for (int n = 0; n < 16; n++) {
            printf(" %x:%d", n, nibble[n]);
        }
        printf("\n\n");
    }
}

static void noise_test(const ads_hal_t *ads)
{
    int16_t code;
    int err;

    check_config(ads, NOISE_MUX, ADS_PGA_0V256);
    for (int block = 1;; block++) {
        int16_t min = INT16_MAX, max = INT16_MIN;
        int errors = 0, good = 0;
        double sum = 0, sum_sq = 0;

        printf("noise block %d\n", block);
        for (int i = 0; i < NOISE_N; i++) {
            err = ads_convert(ads, NOISE_MUX, ADS_PGA_0V256, ADS_DR_8SPS, &code);
            if (err != 0) {
                printf("conversion FAILED (%d)\n", err);
                errors++;
                continue;
            }
            printf("%5d%s", code, good % 10 == 9 ? "\n" : "");
            good++;
            if (code < min) min = code;
            if (code > max) max = code;
            sum += code;
            sum_sq += (double)code * code;
        }
        if (good == 0) {
            printf("--- no readings, errors %d\n\n", errors);
            continue;
        }

        double mean = sum / good;
        double var = sum_sq / good - mean * mean;
        double sd = var > 0 ? sqrt(var) : 0;
        printf("%s--- %d readings: mean %.2f  min %d  max %d  spread %d  sd %.2f  errors %d\n",
               good % 10 ? "\n" : "", good, mean, min, max, max - min, sd, errors);
        printf("--- in uV: mean %.1f  spread %.1f  sd %.1f\n\n",
               mean * NOISE_LSB_UV, (max - min) * NOISE_LSB_UV, sd * NOISE_LSB_UV);
    }
}

static void inputs_test(const ads_hal_t *ads)
{
    static const unsigned mux[4] = { ADS_MUX_A0_GND, ADS_MUX_A1_GND, ADS_MUX_A2_GND, ADS_MUX_A3_GND };

    // Config read-back per input: shows whether the chip keeps each MUX setting.
    for (int ch = 0; ch < 4; ch++) {
        printf("A%d: ", ch);
        check_config(ads, mux[ch], ADS_PGA_4V096);
    }
    printf("\n     A0 mV      A1 mV      A2 mV      A3 mV\n");
    for (;;) {
        for (int ch = 0; ch < 4; ch++) {
            int16_t code;
            int err = ads_convert(ads, mux[ch], ADS_PGA_4V096, ADS_DR_8SPS, &code);
            if (err != 0) {
                printf("  FAIL(%2d) ", err);
            } else {
                printf("%10.1f ", code * LSB_UV / 1000.0);
            }
        }
        printf("\n");
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void app_main(void)
{
    i2c_master_bus_handle_t bus;
    ads_hal_t ads;
    ESP_ERROR_CHECK(ads_port_bus_init(I2C_PORT, PIN_SDA, PIN_SCL, &bus));
    ESP_ERROR_CHECK(ads_port_device_init(bus, ADS_ADDR, SCL_HZ, &ads));

    printf("test: %s\n", TEST == TEST_TIMING ? "timing (1d)"
                          : TEST == TEST_RESOLUTION ? "resolution (1f)"
                          : TEST == TEST_NOISE ? "noise (1h)" : "inputs (1h2)");
    if (TEST == TEST_TIMING) {
        timing_test(bus, &ads);
    } else if (TEST == TEST_RESOLUTION) {
        resolution_test(&ads);
    } else if (TEST == TEST_NOISE) {
        noise_test(&ads);
    } else {
        inputs_test(&ads);
    }
}
