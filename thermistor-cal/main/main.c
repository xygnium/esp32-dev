// Stage 1d: read the ADS1115 Config register, then time one single-shot conversion
// per data-rate code (DR 000-111) by polling the OS bit.
// I2C bus 0: SDA = D21, SCL = D22. ADS at 0x48 (ADDR pulled low on the board).
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ads1115.h"
#include "ads_port.h"

#define I2C_PORT   0
#define PIN_SDA    GPIO_NUM_21
#define PIN_SCL    GPIO_NUM_22
#define ADS_ADDR   0x48
#define SCL_HZ     100000
#define TIMEOUT_US 500000

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

void app_main(void)
{
    i2c_master_bus_handle_t bus;
    ads_hal_t ads;
    ESP_ERROR_CHECK(ads_port_bus_init(I2C_PORT, PIN_SDA, PIN_SCL, &bus));
    ESP_ERROR_CHECK(ads_port_device_init(bus, ADS_ADDR, SCL_HZ, &ads));

    // The ADS keeps its config across an ESP32 reset, so read it before and after
    // a general-call reset. Only after a power cycle is "at boot" the true power-on value.
    print_config(&ads, "at boot");
    esp_err_t err = ads_port_general_call_reset(bus, SCL_HZ);
    printf("general-call reset: %s\n", esp_err_to_name(err));
    print_config(&ads, "after reset");

    for (int pass = 1;; pass++) {
        printf("\ntiming sweep, pass %d\n", pass);
        timing_sweep(&ads);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
