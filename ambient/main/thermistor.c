#include "thermistor.h"

#include <math.h>
#include <stdbool.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "i2c_bus.h"
#include "sensor_util.h"

static const char *TAG = "thermistor";

#define SCL_HZ          100000
#define IO_TIMEOUT_MS   50
#define PIN_SWITCH      GPIO_NUM_25     // AO3401 gate, low = excitation on

#define ADS_ADDR        0x48
#define ADS_REG_CONV    0x00
#define ADS_REG_CFG     0x01
#define ADS_CFG_OS      0x8000  // write: start a conversion; read: 1 = idle
#define ADS_CFG_MUX_A0  0x4000  // A0 against GND
#define ADS_CFG_MUX_A1  0x5000  // A1 against GND
#define ADS_CFG_PGA     0x0200  // +/-4.096 V
#define ADS_CFG_SINGLE  0x0100  // single-shot
#define ADS_CFG_DR_8    0x0000  // 8 SPS
#define ADS_CFG_NO_COMP 0x0003  // comparator off
#define ADS_LSB_V       (4.096 / 32768.0)
// 125 ms at 8 SPS, and the ADC's clock can run 10 % slow. Wait most of it,
//   then poll.
#define ADS_CONV_MS     130
#define ADS_TIMEOUT_MS  300

// Asks for 20 ms of settling. At the 100 Hz tick rate 30 ms gives at least 20 ms.
#define EXCITE_WAIT_MS  30

#define R_DIVIDER       10000.0     // ohms, nominal
// The ADC input acts like a leak across the thermistor (WIRING.md,
//   Thermistor). An estimate, not checked against the datasheet's input model.
#define R_ADC_INPUT     4.3e6       // ohms

// Generic 10k NTC coefficients, from WIRING.md.
#define SH_A            1.129148e-3
#define SH_B            2.34125e-4
#define SH_C            8.76741e-8

#define A0_ON_MIN_V     3.1
#define A0_ON_MAX_V     3.5
#define A0_DRIFT_MAX_V  0.01
// -40 C is about 330 k and 100 C about 680 ohms, so a ratio outside these is
//   an open or a short.
#define RATIO_MIN       0.02
#define RATIO_MAX       0.98

static i2c_master_dev_handle_t s_dev;

void thermistor_pin_init(void)
{
    // Set the level first, so the pin comes up high and the switch never pulses on.
    gpio_set_level(PIN_SWITCH, 1);
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PIN_SWITCH,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO25 setup: %s", esp_err_to_name(err));
    }
}

int thermistor_init(void)
{
    i2c_master_bus_handle_t bus = i2c_bus_get();
    if (!bus) {
        return -1;
    }
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ADS_ADDR,
        .scl_speed_hz = SCL_HZ,
    };
    if (i2c_master_bus_add_device(bus, &cfg, &s_dev) != ESP_OK) {
        s_dev = NULL;
        return -1;
    }
    return 0;
}

static esp_err_t ads_read_reg(uint8_t reg, uint16_t *val)
{
    uint8_t b[2];
    esp_err_t err = i2c_master_transmit_receive(s_dev, &reg, 1, b, sizeof b, IO_TIMEOUT_MS);
    if (err == ESP_OK) {
        *val = (uint16_t)((b[0] << 8) | b[1]);
    }
    return err;
}

// One single-shot conversion, returned in volts.
static esp_err_t ads_convert(uint16_t mux, double *volts)
{
    const uint16_t cfg = ADS_CFG_OS | mux | ADS_CFG_PGA | ADS_CFG_SINGLE | ADS_CFG_DR_8
                         | ADS_CFG_NO_COMP;
    const uint8_t b[3] = { ADS_REG_CFG, (uint8_t)(cfg >> 8), (uint8_t)cfg };

    esp_err_t err = i2c_master_transmit(s_dev, b, sizeof b, IO_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(ADS_CONV_MS));

    TickType_t start = xTaskGetTickCount();
    uint16_t status;
    for (;;) {
        err = ads_read_reg(ADS_REG_CFG, &status);
        if (err != ESP_OK) {
            return err;
        }
        if (status & ADS_CFG_OS) {
            break;
        }
        if (xTaskGetTickCount() - start >= pdMS_TO_TICKS(ADS_TIMEOUT_MS)) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);
    }

    uint16_t raw;
    err = ads_read_reg(ADS_REG_CONV, &raw);
    if (err == ESP_OK) {
        *volts = (int16_t)raw * ADS_LSB_V;
    }
    return err;
}

// Thermistor resistance from the ratio A1 / A0, corrected for the ADC input.
static double resistance_ohms(double ratio)
{
    double measured = R_DIVIDER * ratio / (1.0 - ratio);
    return 1.0 / (1.0 / measured - 1.0 / R_ADC_INPUT);
}

static double steinhart_hart_c(double r)
{
    double l = log(r);
    return 1.0 / (SH_A + SH_B * l + SH_C * l * l * l) - 273.15;
}

const char *thermistor_read(double *temp_c, double *ratio, int64_t *utc_us)
{
    if (!s_dev) {
        return "bus";
    }
    double a0_first = 0, a1 = 0, a0_last = 0;
    gpio_set_level(PIN_SWITCH, 0);
    vTaskDelay(pdMS_TO_TICKS(EXCITE_WAIT_MS));
    esp_err_t err = ads_convert(ADS_CFG_MUX_A0, &a0_first);
    if (err == ESP_OK) {
        err = ads_convert(ADS_CFG_MUX_A1, &a1);
    }
    if (err == ESP_OK) {
        err = ads_convert(ADS_CFG_MUX_A0, &a0_last);
    }
    gpio_set_level(PIN_SWITCH, 1);
    int64_t done = sensor_utc_us();
    if (err != ESP_OK) {
        return sensor_err_reason(err);
    }

    double a0 = (a0_first + a0_last) / 2.0;
    if (a0 < A0_ON_MIN_V || a0 > A0_ON_MAX_V || fabs(a0_first - a0_last) > A0_DRIFT_MAX_V) {
        ESP_LOGD(TAG, "excitation A0 %.4f V, %.4f V", a0_first, a0_last);
        return "excitation";
    }
    double r = a1 / a0;
    if (r < RATIO_MIN) {
        return "short";
    }
    if (r > RATIO_MAX) {
        return "open";
    }
    *ratio = r;
    *temp_c = steinhart_hart_c(resistance_ohms(r));
    *utc_us = done;
    return NULL;
}
