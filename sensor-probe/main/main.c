// Sensor probe, kept as a permanent diagnostic. Repeats every READ_PERIOD_MS.
// Bus 0: SDA = D21, SCL = D22, 100 kHz. The breakout boards' pull-ups carry the
//   bus; the ESP32's weak internal pull-ups are on as well.
// Each pass reads the BMP388 (0x76) and the SHT45 (0x44) and prints the
//   compensated values with a PASS or FAIL for each sensor.
// PASS means the reads succeeded, the checksums (SHT45) and chip id (BMP388)
//   were right, and the values are inside the part's specified operating range
//   (the SHT45's humidity is allowed 5 % either side of 0..100 %).
// It can't say the values are accurate; compare them with a reference.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SDA_PIN         21
#define SCL_PIN         22
#define SCL_HZ          100000
#define IO_TIMEOUT_MS   50
#define READ_PERIOD_MS  2000

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
//   234 + (392 + 32 * 2000) + (313 + 32 * 2000) us, about 129 ms (timing
//   constants from Bosch's BMP3 driver). The reset defaults use less
//   oversampling, so the measurement normally finishes much sooner.
#define BMP388_TIMEOUT_MS 150

#define SHT4X_ADDR      0x44
#define SHT4X_CMD_MEASURE_HIGH 0xFD
// Two ticks at the 100 Hz tick rate; the high-precision measurement takes
//   up to 8.3 ms.
#define SHT4X_WAIT_MS   20

typedef struct {
    double t1, t2, t3;
    double p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11;
} bmp388_cal_t;

static i2c_master_dev_handle_t add_device(i2c_master_bus_handle_t bus, uint8_t addr)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = SCL_HZ,
    };
    i2c_master_dev_handle_t dev = NULL;
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &dev);
    if (err != ESP_OK) {
        printf("  device 0x%02x setup FAILED: %s\n", addr, esp_err_to_name(err));
        return NULL;
    }
    return dev;
}

static esp_err_t bmp_read(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(dev, &reg, 1, buf, n, IO_TIMEOUT_MS);
}

static esp_err_t bmp_write(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val)
{
    const uint8_t b[2] = { reg, val };
    return i2c_master_transmit(dev, b, sizeof b, IO_TIMEOUT_MS);
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

// Returns true if the BMP388 answered with the right chip id, its calibration
//   was read, and one forced measurement gave a temperature in deg C and a
//   pressure in Pa.
static bool bmp388_read(i2c_master_bus_handle_t bus, double *temp_c, double *press_pa,
                        bool *in_range)
{
    i2c_master_dev_handle_t dev = add_device(bus, BMP388_ADDR);
    if (!dev) {
        return false;
    }
    bool ok = false;
    uint8_t id, st, err_reg, cal[21], d[6];
    bmp388_cal_t k;

    esp_err_t err = bmp_read(dev, BMP388_REG_CHIP_ID, &id, 1);
    if (err != ESP_OK) {
        printf("  BMP388 chip id read FAILED: %s\n", esp_err_to_name(err));
        goto out;
    }
    if (id != BMP388_CHIP_ID && id != BMP390_CHIP_ID) {
        printf("  BMP388 chip id 0x%02x: not a BMP388 or BMP390 (expected 0x%02x or 0x%02x)\n",
               id, BMP388_CHIP_ID, BMP390_CHIP_ID);
        goto out;
    }
    // The sensor stays powered across an ESP32 reset, so settings left by
    //   other firmware (oversampling, IIR filter, mode) would carry over.
    //   A soft reset puts every register back to its default.
    err = bmp_write(dev, BMP388_REG_CMD, BMP388_CMD_SOFT_RESET);
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(BMP388_RESET_MS));
        err = bmp_read(dev, BMP388_REG_ERR, &err_reg, 1);
    }
    if (err != ESP_OK) {
        printf("  BMP388 soft reset FAILED: %s\n", esp_err_to_name(err));
        goto out;
    }
    if (err_reg & BMP388_ERR_CMD) {
        printf("  BMP388 soft reset rejected: error 0x%02x\n", err_reg);
        goto out;
    }
    err = bmp_read(dev, BMP388_REG_CALIB, cal, sizeof cal);
    if (err != ESP_OK) {
        printf("  BMP388 calibration read FAILED: %s\n", esp_err_to_name(err));
        goto out;
    }
    bmp_parse_cal(cal, &k);

    err = bmp_write(dev, BMP388_REG_PWR_CTRL, BMP388_PWR_FORCED);
    if (err != ESP_OK) {
        printf("  BMP388 start of measurement FAILED: %s\n", esp_err_to_name(err));
        goto out;
    }
    // Poll until both results are ready, or give up after BMP388_TIMEOUT_MS.
    TickType_t start = xTaskGetTickCount();
    do {
        vTaskDelay(1);
        err = bmp_read(dev, BMP388_REG_STATUS, &st, 1);
    } while (err == ESP_OK && (st & BMP388_STATUS_DRDY) != BMP388_STATUS_DRDY
             && xTaskGetTickCount() - start < pdMS_TO_TICKS(BMP388_TIMEOUT_MS));
    if (err == ESP_OK) {
        err = bmp_read(dev, BMP388_REG_ERR, &err_reg, 1);
    }
    if (err != ESP_OK) {
        printf("  BMP388 status read FAILED: %s\n", esp_err_to_name(err));
        goto out;
    }
    if ((st & BMP388_STATUS_DRDY) != BMP388_STATUS_DRDY || (err_reg & 0x07)) {
        printf("  BMP388 not ready or in error: status 0x%02x, error 0x%02x\n", st, err_reg);
        goto out;
    }
    err = bmp_read(dev, BMP388_REG_DATA, d, sizeof d);
    if (err != ESP_OK) {
        printf("  BMP388 data read FAILED: %s\n", esp_err_to_name(err));
        goto out;
    }

    double up = (double)((uint32_t)d[0] | ((uint32_t)d[1] << 8) | ((uint32_t)d[2] << 16));
    double ut = (double)((uint32_t)d[3] | ((uint32_t)d[4] << 8) | ((uint32_t)d[5] << 16));

    double pd1 = ut - k.t1;
    double t = pd1 * k.t2 + pd1 * pd1 * k.t3;

    double po1 = k.p5 + k.p6 * t + k.p7 * t * t + k.p8 * t * t * t;
    double po2 = up * (k.p1 + k.p2 * t + k.p3 * t * t + k.p4 * t * t * t);
    double pd = (k.p9 + k.p10 * t) * up * up + up * up * up * k.p11;

    *temp_c = t;
    *press_pa = po1 + po2 + pd;
    // The BMP388 is specified for -40..85 deg C and 300..1250 hPa.
    *in_range = t >= -40 && t <= 85 && *press_pa >= 30000 && *press_pa <= 125000;
    ok = true;
out:
    i2c_master_bus_rm_device(dev);
    return ok;
}

// CRC-8 of one 16-bit word as the SHT4x sends it: polynomial 0x31, start 0xFF.
static uint8_t sht4x_crc(const uint8_t *word)
{
    uint8_t crc = 0xFF;

    for (int i = 0; i < 2; i++) {
        crc ^= word[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

// Returns true if the SHT45 gave a measurement with both checksums right.
static bool sht45_read(i2c_master_bus_handle_t bus, double *temp_c, double *rh,
                       bool *in_range)
{
    i2c_master_dev_handle_t dev = add_device(bus, SHT4X_ADDR);
    if (!dev) {
        return false;
    }
    bool ok = false;
    const uint8_t cmd = SHT4X_CMD_MEASURE_HIGH;
    uint8_t b[6];

    esp_err_t err = i2c_master_transmit(dev, &cmd, 1, IO_TIMEOUT_MS);
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(SHT4X_WAIT_MS));
        err = i2c_master_receive(dev, b, sizeof b, IO_TIMEOUT_MS);
    }
    if (err != ESP_OK) {
        printf("  SHT45 measurement FAILED: %s\n", esp_err_to_name(err));
        goto out;
    }
    if (sht4x_crc(&b[0]) != b[2] || sht4x_crc(&b[3]) != b[5]) {
        printf("  SHT45 checksum wrong\n");
        goto out;
    }
    double raw_t = (double)((b[0] << 8) | b[1]);
    double raw_rh = (double)((b[3] << 8) | b[4]);
    *temp_c = -45.0 + 175.0 * raw_t / 65535.0;
    double rh_raw = -6.0 + 125.0 * raw_rh / 65535.0;
    // The datasheet says to clamp to 0..100 %. Report the clamped value, but
    //   flag a reading more than 5 % outside that range as out of range.
    *in_range = rh_raw >= -5.0 && rh_raw <= 105.0 && *temp_c >= -40 && *temp_c <= 125;
    *rh = rh_raw < 0 ? 0 : (rh_raw > 100 ? 100 : rh_raw);
    ok = true;
out:
    i2c_master_bus_rm_device(dev);
    return ok;
}

void app_main(void)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = 0,
        .sda_io_num = SDA_PIN,
        .scl_io_num = SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = 1,
    };
    i2c_master_bus_handle_t bus;
    esp_err_t err = i2c_new_master_bus(&cfg, &bus);
    if (err != ESP_OK) {
        printf("bus setup FAILED: %s\n", esp_err_to_name(err));
        return;
    }

    for (int pass = 1;; pass++) {
        printf("\nsensor probe, pass %d\n", pass);

        double bt = 0, bp = 0, st = 0, srh = 0;
        bool b_range = false, s_range = false;
        bool b_ok = bmp388_read(bus, &bt, &bp, &b_range);
        bool s_ok = sht45_read(bus, &st, &srh, &s_range);

        if (b_ok) {
            printf("  BMP388  %.2f C  %.2f hPa  %s\n", bt, bp / 100.0,
                   b_range ? "PASS" : "FAIL (value out of range)");
        } else {
            printf("  BMP388  FAIL\n");
        }
        if (s_ok) {
            printf("  SHT45   %.2f C  %.1f %%RH  %s\n", st, srh,
                   s_range ? "PASS" : "FAIL (value out of range)");
        } else {
            printf("  SHT45   FAIL\n");
        }
        if (b_ok && s_ok) {
            printf("  temperature difference (BMP388 - SHT45): %+.2f C\n", bt - st);
        }
        vTaskDelay(pdMS_TO_TICKS(READ_PERIOD_MS));
    }
}
