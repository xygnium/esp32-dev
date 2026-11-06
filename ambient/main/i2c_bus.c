#include "i2c_bus.h"

#include "esp_log.h"

static const char *TAG = "i2c";

#define PIN_SDA 21
#define PIN_SCL 22

static i2c_master_bus_handle_t s_bus;

i2c_master_bus_handle_t i2c_bus_get(void)
{
    if (s_bus) {
        return s_bus;
    }
    i2c_master_bus_config_t cfg = {
        .i2c_port = 0,
        .sda_io_num = PIN_SDA,
        .scl_io_num = PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,   // as the probes; the breakouts have their own too
    };
    esp_err_t err = i2c_new_master_bus(&cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bus init: %s", esp_err_to_name(err));
        s_bus = NULL;
    }
    return s_bus;
}
