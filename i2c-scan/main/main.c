// I2C scan, kept as a permanent diagnostic. Repeats every SCAN_PERIOD_MS.
// Bus 0: SDA = D21, SCL = D22. Bus 1: SDA = D32, SCL = D33.
// For each bus: first reads both lines as plain inputs (with the ESP32's weak
//   pull-ups on) and reports a line held low at idle, then probes every address
//   0x08-0x77 and prints the ones that answer.
// If 0x70 answers it is taken to be the TCA9548A mux: each of its 8 channels is
//   switched on in turn and scanned, then all channels are switched off.
// The names printed beside addresses are what this project expects there; a scan
//   can't tell which chip actually answered.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SCL_HZ          100000
#define PROBE_TIMEOUT_MS 50
#define SCAN_PERIOD_MS  5000
#define ADDR_FIRST      0x08
#define ADDR_LAST       0x77
#define MUX_ADDR        0x70
#define MUX_CHANNELS    8

typedef struct {
    int port;
    gpio_num_t sda;
    gpio_num_t scl;
} bus_pins_t;

static const bus_pins_t buses[] = {
    { 0, GPIO_NUM_21, GPIO_NUM_22 },
    { 1, GPIO_NUM_32, GPIO_NUM_33 },
};
#define N_BUSES (sizeof buses / sizeof buses[0])

static const char *expected_at(uint8_t addr)
{
    switch (addr) {
    case 0x44: return "SHT45, or SHT31 with ADR low";
    case 0x45: return "SHT31 with ADR high";
    case 0x48: return "ADS1115";
    case 0x57: return "memory chip on the DS3231 board";
    case 0x68: return "DS3231";
    case 0x70: return "TCA9548A mux";
    case 0x76: return "BMP388 with SDO low";
    case 0x77: return "BMP388 with SDO high";
    default:   return "not expected in this project";
    }
}

// Reads the two lines before the I2C driver takes the pins.
// Returns false if either is held low.
static bool lines_idle_high(const bus_pins_t *b)
{
    const gpio_num_t pins[2] = { b->sda, b->scl };
    int level[2];

    for (int i = 0; i < 2; i++) {
        gpio_reset_pin(pins[i]);
        gpio_set_direction(pins[i], GPIO_MODE_INPUT);
        gpio_set_pull_mode(pins[i], GPIO_PULLUP_ONLY);
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    for (int i = 0; i < 2; i++) {
        level[i] = gpio_get_level(pins[i]);
    }
    printf("  idle levels: SDA %s, SCL %s\n",
           level[0] ? "high" : "LOW (stuck)", level[1] ? "high" : "LOW (stuck)");
    return level[0] && level[1];
}

// Probes every address and prints the ones that answer. skip is left out
// (the mux itself, while one of its channels is being scanned); 0 for none.
// Returns how many answered.
static int scan_addresses(i2c_master_bus_handle_t bus, const char *indent, uint8_t skip)
{
    int found = 0;

    for (uint8_t addr = ADDR_FIRST; addr <= ADDR_LAST; addr++) {
        if (addr == skip) {
            continue;
        }
        if (i2c_master_probe(bus, addr, PROBE_TIMEOUT_MS) == ESP_OK) {
            printf("%s0x%02x  %s\n", indent, addr, expected_at(addr));
            found++;
        }
    }
    return found;
}

// channels is the mux control byte: one bit per channel, 0 for all off.
static esp_err_t mux_select(i2c_master_dev_handle_t mux, uint8_t channels)
{
    return i2c_master_transmit(mux, &channels, 1, PROBE_TIMEOUT_MS);
}

static void scan_mux(i2c_master_bus_handle_t bus)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = MUX_ADDR,
        .scl_speed_hz = SCL_HZ,
    };
    i2c_master_dev_handle_t mux;

    if (i2c_master_bus_add_device(bus, &cfg, &mux) != ESP_OK) {
        printf("  mux: could not open 0x%02x\n", MUX_ADDR);
        return;
    }
    for (int ch = 0; ch < MUX_CHANNELS; ch++) {
        if (mux_select(mux, 1u << ch) != ESP_OK) {
            printf("  mux channel %d: select FAILED\n", ch);
            continue;
        }
        printf("  mux channel %d:\n", ch);
        if (scan_addresses(bus, "    ", MUX_ADDR) == 0) {
            printf("    nothing\n");
        }
    }
    if (mux_select(mux, 0) != ESP_OK) {
        printf("  mux: switching all channels off FAILED\n");
    }
    i2c_master_bus_rm_device(mux);
}

static void scan_bus(const bus_pins_t *b)
{
    printf("bus %d (SDA = GPIO %d, SCL = GPIO %d)\n", b->port, b->sda, b->scl);
    if (!lines_idle_high(b)) {
        printf("  not scanned: a line is held low\n");
        return;
    }

    i2c_master_bus_config_t cfg = {
        .i2c_port = b->port,
        .sda_io_num = b->sda,
        .scl_io_num = b->scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = 1,
    };
    i2c_master_bus_handle_t bus;
    esp_err_t err = i2c_new_master_bus(&cfg, &bus);
    if (err != ESP_OK) {
        printf("  bus setup FAILED: %s\n", esp_err_to_name(err));
        return;
    }

    int found = scan_addresses(bus, "  ", 0);
    printf("  %d device%s answered\n", found, found == 1 ? "" : "s");
    if (i2c_master_probe(bus, MUX_ADDR, PROBE_TIMEOUT_MS) == ESP_OK) {
        scan_mux(bus);
    }
    i2c_del_master_bus(bus);
}

void app_main(void)
{
    for (int pass = 1;; pass++) {
        printf("\ni2c scan, pass %d\n", pass);
        for (size_t i = 0; i < N_BUSES; i++) {
            scan_bus(&buses[i]);
        }
        vTaskDelay(pdMS_TO_TICKS(SCAN_PERIOD_MS));
    }
}
