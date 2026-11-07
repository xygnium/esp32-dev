#include "ds3231.h"

#include "driver/i2c_master.h"
#include "esp_log.h"

#include "i2c_bus.h"

static const char *TAG = "ds3231";

#define SCL_HZ          100000
#define XFER_TIMEOUT_MS 100

#define REG_SECONDS     0x00
#define REG_CONTROL     0x0E
#define CONTROL_EOSC    0x80            // 1: oscillator stops when on the battery
#define REG_STATUS      0x0F
#define STATUS_OSF      0x80

static i2c_master_dev_handle_t s_dev;

static int from_bcd(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
static uint8_t to_bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
static bool bcd_ok(uint8_t v) { return (v >> 4) <= 9 && (v & 0x0F) <= 9; }

static int read_regs(uint8_t reg, uint8_t *buf, size_t n)
{
    if (!s_dev) {
        return -1;
    }
    esp_err_t err = i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, XFER_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "read 0x%02x: %s", reg, esp_err_to_name(err));
        return -1;
    }
    return 0;
}

static int write_regs(const uint8_t *buf, size_t n)
{
    if (!s_dev) {
        return -1;
    }
    esp_err_t err = i2c_master_transmit(s_dev, buf, n, XFER_TIMEOUT_MS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "write 0x%02x: %s", buf[0], esp_err_to_name(err));
        return -1;
    }
    return 0;
}

int ds3231_init(uint8_t addr)
{
    i2c_master_bus_handle_t bus = i2c_bus_get();
    if (!bus) {
        return -1;
    }
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = SCL_HZ,
    };
    if (i2c_master_bus_add_device(bus, &cfg, &s_dev) != ESP_OK) {
        s_dev = NULL;
        return -1;
    }
    uint8_t status;
    return read_regs(REG_STATUS, &status, 1);   // answers at all?
}

int ds3231_read(struct tm *utc, bool *osf)
{
    uint8_t r[REG_STATUS + 1];
    if (read_regs(REG_SECONDS, r, sizeof r) != 0) {
        return -1;
    }
    uint8_t sec = r[0] & 0x7F, min = r[1] & 0x7F, day = r[4] & 0x3F, mon = r[5] & 0x1F, yr = r[6];
    uint8_t hr = (r[2] & 0x40) ? (r[2] & 0x1F) : (r[2] & 0x3F);
    if (!bcd_ok(sec) || !bcd_ok(min) || !bcd_ok(hr) || !bcd_ok(day) || !bcd_ok(mon) || !bcd_ok(yr)) {
        ESP_LOGE(TAG, "time registers hold invalid BCD");
        return -1;
    }
    int hour = from_bcd(hr);
    if (r[2] & 0x40) {                    // 12-hour mode
        hour = hour % 12 + ((r[2] & 0x20) ? 12 : 0);
    }
    *utc = (struct tm){
        .tm_sec = from_bcd(sec), .tm_min = from_bcd(min), .tm_hour = hour,
        .tm_mday = from_bcd(day), .tm_mon = from_bcd(mon) - 1, .tm_year = 100 + from_bcd(yr),
    };
    if (utc->tm_sec > 59 || utc->tm_min > 59 || utc->tm_hour > 23 || utc->tm_mon < 0
        || utc->tm_mon > 11 || utc->tm_mday < 1 || utc->tm_mday > 31) {
        ESP_LOGE(TAG, "time out of range");
        return -1;
    }
    *osf = (r[REG_STATUS] & STATUS_OSF) != 0;
    return 0;
}

int ds3231_read_seconds(int *sec)
{
    uint8_t v;
    if (read_regs(REG_SECONDS, &v, 1) != 0 || !bcd_ok(v & 0x7F)) {
        return -1;
    }
    *sec = from_bcd(v & 0x7F);
    return *sec > 59 ? -1 : 0;
}

int ds3231_write(const struct tm *utc)
{
    // One transaction from the seconds register, so the seconds write (which
    // restarts the countdown) comes first.
    uint8_t buf[8] = {
        REG_SECONDS, to_bcd(utc->tm_sec), to_bcd(utc->tm_min), to_bcd(utc->tm_hour),
        (uint8_t)(utc->tm_wday + 1), to_bcd(utc->tm_mday), to_bcd(utc->tm_mon + 1),
        to_bcd((utc->tm_year + 1900) % 100),
    };
    return write_regs(buf, sizeof buf);
}

int ds3231_clear_osf(void)
{
    uint8_t status;
    if (read_regs(REG_STATUS, &status, 1) != 0) {
        return -1;
    }
    uint8_t buf[2] = { REG_STATUS, (uint8_t)(status & ~STATUS_OSF) };
    return write_regs(buf, sizeof buf);
}

int ds3231_get_eosc(bool *stop_on_battery)
{
    uint8_t ctrl;
    if (read_regs(REG_CONTROL, &ctrl, 1) != 0) {
        return -1;
    }
    *stop_on_battery = (ctrl & CONTROL_EOSC) != 0;
    return 0;
}

int ds3231_set_eosc(bool stop_on_battery)
{
    uint8_t ctrl;
    if (read_regs(REG_CONTROL, &ctrl, 1) != 0) {
        return -1;
    }
    ctrl = stop_on_battery ? (ctrl | CONTROL_EOSC) : (ctrl & ~CONTROL_EOSC);
    uint8_t buf[2] = { REG_CONTROL, ctrl };
    return write_regs(buf, sizeof buf);
}
