// Stage 1f / 1h tests on A0, single-shot conversions at 8 SPS. Pick one with TEST_NOISE.
// 1f, resolution (TEST_NOISE 0): pot wiper on A0, PGA ±4.096 V. Prints each raw
//   code, and a summary every SUMMARY_N readings. A genuine ADS1115 gives odd codes
//   and a varied low nibble as the pot turns; an ADS1015 gives only multiples of 16.
// 1h, noise (TEST_NOISE 1): PGA ±0.256 V. Prints NOISE_N readings, then mean,
//   min/max, spread and standard deviation. NOISE_MUX picks the input:
//   ADS_MUX_A0_GND with A0 jumpered to GND, or ADS_MUX_A0_A1 with A0 jumpered
//   to A1 (leaves ground wiring out of the reading).
// I2C bus 0: SDA = D21, SCL = D22. ADS at 0x48 (ADDR pulled low on the board).
#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "ads1115.h"
#include "ads_port.h"

#define TEST_NOISE 1

#define I2C_PORT   0
#define PIN_SDA    GPIO_NUM_21
#define PIN_SCL    GPIO_NUM_22
#define ADS_ADDR   0x48
#define SCL_HZ     100000
#define SUMMARY_N  40    // about 5 s at 8 SPS
#define LSB_UV     125   // PGA ±4.096 V
#define NOISE_N    100   // about 13 s at 8 SPS
#define NOISE_LSB_UV 7.8125  // PGA ±0.256 V
#define NOISE_MUX   ADS_MUX_A0_A1

static void resolution_test(const ads_hal_t *ads)
{
    int16_t code;
    int err;

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

void app_main(void)
{
    i2c_master_bus_handle_t bus;
    ads_hal_t ads;
    ESP_ERROR_CHECK(ads_port_bus_init(I2C_PORT, PIN_SDA, PIN_SCL, &bus));
    ESP_ERROR_CHECK(ads_port_device_init(bus, ADS_ADDR, SCL_HZ, &ads));

    // One conversion, then read Config back: shows the chip took the MUX/PGA/DR bits.
    unsigned mux = TEST_NOISE ? NOISE_MUX : ADS_MUX_A0_GND;
    unsigned pga = TEST_NOISE ? ADS_PGA_0V256 : ADS_PGA_4V096;
    uint16_t expect = ADS_CFG_OS | (mux << ADS_CFG_MUX_SHIFT) |
                      (pga << ADS_CFG_PGA_SHIFT) | ADS_CFG_MODE_SINGLE |
                      (ADS_DR_8SPS << ADS_CFG_DR_SHIFT) | ADS_CFG_COMP_OFF;
    int16_t code;
    uint16_t cfg;
    int err = ads_convert(&ads, mux, pga, ADS_DR_8SPS, &code);
    if (err == 0 && ads_read_reg(&ads, ADS_REG_CONFIG, &cfg) == 0) {
        printf("config after first conversion: 0x%04x (expect 0x%04x)\n", cfg, expect);
    } else {
        printf("first conversion or config read FAILED (%d)\n", err);
    }

    if (TEST_NOISE) {
        noise_test(&ads);
    } else {
        resolution_test(&ads);
    }
}
