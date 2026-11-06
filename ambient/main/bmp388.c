#include "bmp388.h"

#include <stdbool.h>

#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "i2c_bus.h"
#include "sensor_util.h"

#define SCL_HZ          100000
#define IO_TIMEOUT_MS   50

#define BMP388_ADDR     0x76
#define BMP388_CHIP_ID  0x50
#define BMP390_CHIP_ID  0x60   // same registers and compensation as the BMP388
#define BMP388_REG_CHIP_ID 0x00
#define BMP388_REG_ERR  0x02
#define BMP388_REG_STATUS 0x03
#define BMP388_REG_DATA 0x04
#define BMP388_REG_PWR_CTRL 0x1B
#define BMP388_REG_CALIB 0x31
#define BMP388_REG_CMD  0x7E
#define BMP388_CMD_SOFT_RESET 0xB6
#define BMP388_ERR_CMD  0x02
// Bosch's driver waits 2 ms after a soft reset. Two ticks at the 100 Hz tick
//   rate give at least 10 ms.
#define BMP388_RESET_MS 20
// Pressure on, temperature on, forced mode (one measurement, then sleep).
#define BMP388_PWR_FORCED 0x13
#define BMP388_STATUS_DRDY (0x20 | 0x40)   // pressure and temperature ready
// The longest forced measurement, both channels at x32 oversampling, takes
//   about 129 ms (timing constants from Bosch's BMP3 driver). The reset
//   defaults use less oversampling, so it normally finishes much sooner.
#define BMP388_TIMEOUT_MS 150

typedef struct {
    double t1, t2, t3;
    double p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11;
} bmp388_cal_t;

static i2c_master_dev_handle_t s_dev;
static bmp388_cal_t s_cal;
static bool s_ready;        // reset done and calibration read

int bmp388_init(void)
{
    i2c_master_bus_handle_t bus = i2c_bus_get();
    if (!bus) {
        return -1;
    }
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BMP388_ADDR,
        .scl_speed_hz = SCL_HZ,
    };
    if (i2c_master_bus_add_device(bus, &cfg, &s_dev) != ESP_OK) {
        s_dev = NULL;
        return -1;
    }
    return 0;
}

static esp_err_t bmp_read(uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, IO_TIMEOUT_MS);
}

static esp_err_t bmp_write(uint8_t reg, uint8_t val)
{
    const uint8_t b[2] = { reg, val };
    return i2c_master_transmit(s_dev, b, sizeof b, IO_TIMEOUT_MS);
}

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static int16_t  s16(const uint8_t *p) { return (int16_t)u16(p); }

// Converts the 21 calibration bytes to the floating-point coefficients of the
//   BMP388 datasheet, section 9.1.
static void bmp_parse_cal(const uint8_t *c, bmp388_cal_t *k)
{
    k->t1  = u16(&c[0]) * 256.0;
    k->t2  = u16(&c[2]) / 1073741824.0;                 // 2^30
    k->t3  = (int8_t)c[4] / 281474976710656.0;          // 2^48
    k->p1  = (s16(&c[5]) - 16384.0) / 1048576.0;        // 2^20
    k->p2  = (s16(&c[7]) - 16384.0) / 536870912.0;      // 2^29
    k->p3  = (int8_t)c[9] / 4294967296.0;               // 2^32
    k->p4  = (int8_t)c[10] / 137438953472.0;            // 2^37
    k->p5  = u16(&c[11]) * 8.0;
    k->p6  = u16(&c[13]) / 64.0;
    k->p7  = (int8_t)c[15] / 256.0;
    k->p8  = (int8_t)c[16] / 32768.0;                   // 2^15
    k->p9  = s16(&c[17]) / 281474976710656.0;           // 2^48
    k->p10 = (int8_t)c[19] / 281474976710656.0;         // 2^48
    k->p11 = (int8_t)c[20] / 36893488147419103232.0;    // 2^65
}

// Chip id, soft reset (settings left by other firmware survive an ESP32
//   reset), calibration.
static const char *setup(void)
{
    uint8_t id, err_reg, cal[21];
    esp_err_t err = bmp_read(BMP388_REG_CHIP_ID, &id, 1);
    if (err != ESP_OK) {
        return sensor_err_reason(err);
    }
    if (id != BMP388_CHIP_ID && id != BMP390_CHIP_ID) {
        return "chip_id";
    }
    err = bmp_write(BMP388_REG_CMD, BMP388_CMD_SOFT_RESET);
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(BMP388_RESET_MS));
        err = bmp_read(BMP388_REG_ERR, &err_reg, 1);
    }
    if (err != ESP_OK) {
        return sensor_err_reason(err);
    }
    if (err_reg & BMP388_ERR_CMD) {
        return "reset";
    }
    err = bmp_read(BMP388_REG_CALIB, cal, sizeof cal);
    if (err != ESP_OK) {
        return sensor_err_reason(err);
    }
    bmp_parse_cal(cal, &s_cal);
    return NULL;
}

static const char *measure(double *temp_c, double *pressure_pa, int64_t *utc_us)
{
    uint8_t st = 0, err_reg = 0, d[6];
    esp_err_t err = bmp_write(BMP388_REG_PWR_CTRL, BMP388_PWR_FORCED);
    if (err != ESP_OK) {
        return sensor_err_reason(err);
    }
    // Poll until both results are ready, or give up after BMP388_TIMEOUT_MS.
    TickType_t start = xTaskGetTickCount();
    do {
        vTaskDelay(1);
        err = bmp_read(BMP388_REG_STATUS, &st, 1);
    } while (err == ESP_OK && (st & BMP388_STATUS_DRDY) != BMP388_STATUS_DRDY
             && xTaskGetTickCount() - start < pdMS_TO_TICKS(BMP388_TIMEOUT_MS));
    if (err == ESP_OK) {
        err = bmp_read(BMP388_REG_ERR, &err_reg, 1);
    }
    if (err != ESP_OK) {
        return sensor_err_reason(err);
    }
    if ((st & BMP388_STATUS_DRDY) != BMP388_STATUS_DRDY || (err_reg & 0x07)) {
        return "not_ready";
    }
    err = bmp_read(BMP388_REG_DATA, d, sizeof d);
    int64_t done = sensor_utc_us();
    if (err != ESP_OK) {
        return sensor_err_reason(err);
    }

    const bmp388_cal_t *k = &s_cal;
    double up = (double)((uint32_t)d[0] | ((uint32_t)d[1] << 8) | ((uint32_t)d[2] << 16));
    double ut = (double)((uint32_t)d[3] | ((uint32_t)d[4] << 8) | ((uint32_t)d[5] << 16));

    double pd1 = ut - k->t1;
    double t = pd1 * k->t2 + pd1 * pd1 * k->t3;

    double po1 = k->p5 + k->p6 * t + k->p7 * t * t + k->p8 * t * t * t;
    double po2 = up * (k->p1 + k->p2 * t + k->p3 * t * t + k->p4 * t * t * t);
    double pd = (k->p9 + k->p10 * t) * up * up + up * up * up * k->p11;
    double p = po1 + po2 + pd;

    // The BMP388 is specified for -40..85 deg C and 300..1250 hPa.
    if (t < -40 || t > 85 || p < 30000 || p > 125000) {
        return "range";
    }
    *temp_c = t;
    *pressure_pa = p;
    *utc_us = done;
    return NULL;
}

const char *bmp388_read(double *temp_c, double *pressure_pa, int64_t *utc_us)
{
    if (!s_dev) {
        return "bus";
    }
    const char *why = NULL;
    if (!s_ready) {
        why = setup();
    }
    if (!why) {
        why = measure(temp_c, pressure_pa, utc_us);
    }
    s_ready = (why == NULL);
    return why;
}
