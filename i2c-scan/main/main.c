// I2C scan, kept as a permanent diagnostic. Repeats every SCAN_PERIOD_MS.
// Bus 0: SDA = D21, SCL = D22. Bus 1: SDA = D32, SCL = D33.
// For each bus: first reads both lines as plain inputs (with the ESP32's weak
//   pull-ups on) and reports a line held low at idle, then probes every address
//   0x08-0x77 and prints the ones that answer.
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
#define BMP388_REG_CHIP_ID 0x00
#define BMP388_CHIP_ID  0x50
#define SHT4X_CMD_READ_SERIAL 0x89
// Two ticks at the 100 Hz tick rate, so the wait is never under 10 ms.
#define SHT4X_SERIAL_WAIT_MS 20

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

// Reads the chip id register reg of the BMPxxx device at addr and prints it,
//   with whether it matches expected_id, the id of the chip called name.
// A BMP388 returns 0x50 from register 0.
static void print_BMPxxx_chip_id(i2c_master_bus_handle_t bus, uint8_t addr,
                                 uint8_t reg, uint8_t expected_id,
                                 const char *name)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = SCL_HZ,
    };
    i2c_master_dev_handle_t dev;
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &dev);
    if (err != ESP_OK) {
        printf("        device setup FAILED: %s\n", esp_err_to_name(err));
        return;
    }

    uint8_t id;
    err = i2c_master_transmit_receive(dev, &reg, 1, &id, 1, PROBE_TIMEOUT_MS);
    if (err != ESP_OK) {
        printf("        chip id read of register 0x%02x FAILED: %s\n", reg,
               esp_err_to_name(err));
    } else if (id == expected_id) {
        printf("        chip id (register 0x%02x) 0x%02x: %s\n", reg, id, name);
    } else {
        printf("        chip id (register 0x%02x) 0x%02x: not a %s (expected 0x%02x)\n",
               reg, id, name, expected_id);
    }
    i2c_master_bus_rm_device(dev);
}

// Checksum of one 16-bit word as the SHT4x sends it: CRC-8, polynomial 0x31,
//   start value 0xFF. The datasheet's check value: 0xBE 0xEF gives 0x92.
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

// Sends the SHT4x read-serial-number command to the device at addr and prints
//   the serial number, with whether the answer is a valid SHT4x one.
// The answer is 6 bytes: two 16-bit words, each followed by its checksum.
// The SHT40, SHT41 and SHT45 all answer alike, so this can't tell them apart.
static void print_SHT4x_serial(i2c_master_bus_handle_t bus, uint8_t addr)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = SCL_HZ,
    };
    i2c_master_dev_handle_t dev;
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &dev);
    if (err != ESP_OK) {
        printf("        device setup FAILED: %s\n", esp_err_to_name(err));
        return;
    }

    const uint8_t cmd = SHT4X_CMD_READ_SERIAL;
    uint8_t buf[6];
    err = i2c_master_transmit(dev, &cmd, 1, PROBE_TIMEOUT_MS);
    if (err == ESP_OK) {
        // The SHT4x doesn't stretch the clock; it refuses a read until the
        //   answer is ready.
        vTaskDelay(pdMS_TO_TICKS(SHT4X_SERIAL_WAIT_MS));
        err = i2c_master_receive(dev, buf, sizeof buf, PROBE_TIMEOUT_MS);
    }
    if (err != ESP_OK) {
        printf("        serial number read FAILED: %s (not ready, or not an SHT4x)\n",
               esp_err_to_name(err));
    } else if (sht4x_crc(&buf[0]) != buf[2] || sht4x_crc(&buf[3]) != buf[5]) {
        printf("        serial number checksum wrong: not an SHT4x\n");
    } else {
        printf("        serial number 0x%02x%02x%02x%02x: SHT4x\n",
               buf[0], buf[1], buf[3], buf[4]);
    }
    i2c_master_bus_rm_device(dev);
}

// Probes every address and prints the ones that answer.
// Returns how many answered.
static int scan_addresses(i2c_master_bus_handle_t bus)
{
    int found = 0;

    for (uint8_t addr = ADDR_FIRST; addr <= ADDR_LAST; addr++) {
        if (i2c_master_probe(bus, addr, PROBE_TIMEOUT_MS) == ESP_OK) {
            printf("  0x%02x  %s\n", addr, expected_at(addr));
            // Chip id is read at the BMP388 addresses only: register 0 is the
            //   chip id on the BMP388 and BMP390 only. I2C has no standard id
            //   register, so on any other chip register 0 holds something
            //   else, or the write of the register number is taken as a
            //   command.
            // Not every BMPxxx has its id in register 0: the BMP280 and BME280
            //   answer at these same two addresses but keep their id at
            //   register 0xD0, so they print as "not a BMP388".
            if (addr == 0x76 || addr == 0x77) {
                print_BMPxxx_chip_id(bus, addr, BMP388_REG_CHIP_ID,
                                     BMP388_CHIP_ID, "BMP388");
            }
            // Serial number is read at the SHT45 address only: the one-byte
            //   command 0x89 means "read serial number" on the SHT4x family
            //   (SHT40, SHT41, SHT45) only, and on any other chip that byte
            //   would be taken as a register number or a different command.
            // An SHT31 with ADR low answers at this same address but takes
            //   two-byte commands, so it prints as a failed read.
            if (addr == 0x44) {
                print_SHT4x_serial(bus, addr);
            }
            found++;
        }
    }
    return found;
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

    int found = scan_addresses(bus);
    printf("  %d device%s answered\n", found, found == 1 ? "" : "s");
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
