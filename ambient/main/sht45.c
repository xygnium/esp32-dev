#include "sht45.h"

#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "i2c_bus.h"
#include "sensor_util.h"

#define SCL_HZ          100000
#define IO_TIMEOUT_MS   50

#define SHT4X_ADDR      0x44
#define SHT4X_CMD_MEASURE_HIGH 0xFD
// Two ticks at the 100 Hz tick rate; the high-precision measurement takes
//   up to 8.3 ms.
#define SHT4X_WAIT_MS   20

static i2c_master_dev_handle_t s_dev;

int sht45_init(void)
{
    i2c_master_bus_handle_t bus = i2c_bus_get();
    if (!bus) {
        return -1;
    }
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SHT4X_ADDR,
        .scl_speed_hz = SCL_HZ,
    };
    if (i2c_master_bus_add_device(bus, &cfg, &s_dev) != ESP_OK) {
        s_dev = NULL;
        return -1;
    }
    return 0;
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

const char *sht45_read(double *temp_c, double *rh_pct, int64_t *utc_us)
{
    if (!s_dev) {
        return "bus";
    }
    const uint8_t cmd = SHT4X_CMD_MEASURE_HIGH;
    uint8_t b[6];

    esp_err_t err = i2c_master_transmit(s_dev, &cmd, 1, IO_TIMEOUT_MS);
    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(SHT4X_WAIT_MS));
        err = i2c_master_receive(s_dev, b, sizeof b, IO_TIMEOUT_MS);
    }
    int64_t done = sensor_utc_us();
    if (err != ESP_OK) {
        return sensor_err_reason(err);
    }
    if (sht4x_crc(&b[0]) != b[2] || sht4x_crc(&b[3]) != b[5]) {
        return "crc";
    }
    double t = -45.0 + 175.0 * ((b[0] << 8) | b[1]) / 65535.0;
    double rh_raw = -6.0 + 125.0 * ((b[3] << 8) | b[4]) / 65535.0;
    // The datasheet says to clamp to 0..100 %. A reading more than 5 % outside
    //   that range is a failure.
    if (rh_raw < -5.0 || rh_raw > 105.0 || t < -40 || t > 125) {
        return "range";
    }
    *temp_c = t;
    *rh_pct = rh_raw < 0 ? 0 : (rh_raw > 100 ? 100 : rh_raw);
    *utc_us = done;
    return NULL;
}
