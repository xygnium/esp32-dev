// ADS1115 thermistor probe, kept as a permanent diagnostic. Repeats every
//   READ_PERIOD_MS. Sequence from ambient/FIRMWARE.md, Table 1.
// Bus 0: SDA = D21, SCL = D22, 100 kHz. ADS1115 at 0x48. GPIO25 drives the
//   AO3401 gate, low = on.
// Each pass:
//   - reads A0 with the switch off, to show the switch really is off,
//   - energizes, waits, converts A0, A1, A0, and de-energizes,
//   - works out the ratio A1 / A0, the resistance and the temperature.
// PASS means the conversions finished and the voltages are sensible for this
//   circuit: A0 near 0 V when off and near 3.3 V when on, steady across the
//   reading, and a ratio that isn't an open or a short.
// It can't say the temperature is accurate; compare it with a reference.
// Noise test: the readings alternate between idle and load phases of SPREAD_SETS
//   good readings, starting idle, with a summary line after each. Idle has the
//   radio stopped and the SD card quiet. Load runs active WiFi scans, which
//   transmit without joining a network, and continuous SD writes to LOAD_FILE,
//   which is deleted when the phase ends. Scans transmit less than a sustained
//   data stream, so this is not the worst case.
// With a fixed resistor in place of the thermistor (build step 3) the
//   temperature is only an apparent one, but its spread is the noise figure.
// READ_PERIOD_MS keeps the excitation on about 20 % of the time, which is
//   about 0.03 C of self-heating, more than the logger's 15 s spacing. That
//   doesn't matter for a diagnostic or for a fixed resistor.
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sdmmc_cmd.h"

#define SDA_PIN         21
#define SCL_PIN         22
#define SCL_HZ          100000
#define IO_TIMEOUT_MS   50
#define READ_PERIOD_MS  2000
#define PIN_SWITCH      GPIO_NUM_25

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
// The ADC input acts like a leak across the thermistor (ambient/WIRING.md,
//   Thermistor). An estimate, not checked against the datasheet's input model.
#define R_ADC_INPUT     4.3e6       // ohms

// Generic 10k NTC coefficients, from ambient/WIRING.md.
#define SH_A            1.129148e-3
#define SH_B            2.34125e-4
#define SH_C            8.76741e-8

#define A0_OFF_MAX_V    0.05
#define A0_ON_MIN_V     3.1
#define A0_ON_MAX_V     3.5
#define A0_DRIFT_MAX_V  0.01
// -40 C is about 330 k and 100 C about 680 ohms, so a ratio outside these is
//   an open or a short.
#define RATIO_MIN       0.02
#define RATIO_MAX       0.98

#define SPREAD_SETS     32

// SD pins and speed as in sd-probe.
#define PIN_SCK         18
#define PIN_MISO        19
#define PIN_MOSI        23
#define PIN_CS          4
#define SD_FREQ_KHZ     4000
#define MOUNT_POINT     "/sdcard"
#define LOAD_FILE       MOUNT_POINT "/noise.tmp"
#define SD_BLOCK_BYTES  4096
#define SD_BLOCKS       8

#define LOAD_STACK      4096
#define LOAD_IDLE_MS    100

static volatile bool s_load_on;
static volatile uint32_t s_scans, s_sd_writes, s_load_errors;

static esp_err_t ads_read_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint16_t *val)
{
    uint8_t b[2];
    esp_err_t err = i2c_master_transmit_receive(dev, &reg, 1, b, sizeof b, IO_TIMEOUT_MS);
    if (err == ESP_OK) {
        *val = (uint16_t)((b[0] << 8) | b[1]);
    }
    return err;
}

// One single-shot conversion, returned in volts.
static esp_err_t ads_convert(i2c_master_dev_handle_t dev, uint16_t mux, double *volts)
{
    const uint16_t cfg = ADS_CFG_OS | mux | ADS_CFG_PGA | ADS_CFG_SINGLE | ADS_CFG_DR_8
                         | ADS_CFG_NO_COMP;
    const uint8_t b[3] = { ADS_REG_CFG, (uint8_t)(cfg >> 8), (uint8_t)cfg };

    esp_err_t err = i2c_master_transmit(dev, b, sizeof b, IO_TIMEOUT_MS);
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(ADS_CONV_MS));

    TickType_t start = xTaskGetTickCount();
    uint16_t status;
    for (;;) {
        err = ads_read_reg(dev, ADS_REG_CFG, &status);
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
    err = ads_read_reg(dev, ADS_REG_CONV, &raw);
    if (err == ESP_OK) {
        *volts = (int16_t)raw * ADS_LSB_V;
    }
    return err;
}

// Energizes for the A0, A1, A0 set. The switch is always off again on return.
static esp_err_t read_excited(i2c_master_dev_handle_t dev, double *a0_first, double *a1,
                              double *a0_last)
{
    gpio_set_level(PIN_SWITCH, 0);
    vTaskDelay(pdMS_TO_TICKS(EXCITE_WAIT_MS));
    esp_err_t err = ads_convert(dev, ADS_CFG_MUX_A0, a0_first);
    if (err == ESP_OK) {
        err = ads_convert(dev, ADS_CFG_MUX_A1, a1);
    }
    if (err == ESP_OK) {
        err = ads_convert(dev, ADS_CFG_MUX_A0, a0_last);
    }
    gpio_set_level(PIN_SWITCH, 1);
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

// Mean, standard deviation and min-to-max of the last n values.
static void spread(const double *v, int n, double *mean, double *sd, double *range)
{
    double total = 0, sum = 0, lo = v[0], hi = v[0];

    for (int i = 0; i < n; i++) {
        total += v[i];
        lo = v[i] < lo ? v[i] : lo;
        hi = v[i] > hi ? v[i] : hi;
    }
    *mean = total / n;
    for (int i = 0; i < n; i++) {
        sum += (v[i] - *mean) * (v[i] - *mean);
    }
    *sd = n > 1 ? sqrt(sum / (n - 1)) : 0;
    *range = hi - lo;
}

static void wifi_load_task(void *arg)
{
    wifi_scan_config_t scan = { .show_hidden = true, .scan_type = WIFI_SCAN_TYPE_ACTIVE };
    bool running = false;

    for (;;) {
        if (!s_load_on) {
            if (running) {
                esp_wifi_stop();
                running = false;
            }
            vTaskDelay(pdMS_TO_TICKS(LOAD_IDLE_MS));
            continue;
        }
        esp_err_t err = running ? ESP_OK : esp_wifi_start();
        running = running || err == ESP_OK;
        if (err == ESP_OK) {
            err = esp_wifi_scan_start(&scan, true);
        }
        if (err == ESP_OK) {
            esp_wifi_clear_ap_list();
            s_scans++;
        } else {
            s_load_errors++;
            vTaskDelay(pdMS_TO_TICKS(LOAD_IDLE_MS));
        }
    }
}

static void sd_load_task(void *arg)
{
    static uint8_t block[SD_BLOCK_BYTES];
    bool written = false;

    memset(block, 0x5A, sizeof block);
    for (;;) {
        if (!s_load_on) {
            if (written) {
                unlink(LOAD_FILE);
                written = false;
            }
            vTaskDelay(pdMS_TO_TICKS(LOAD_IDLE_MS));
            continue;
        }
        FILE *f = fopen(LOAD_FILE, "w");
        bool ok = f != NULL;
        written = written || ok;
        for (int i = 0; ok && i < SD_BLOCKS; i++) {
            ok = fwrite(block, 1, sizeof block, f) == sizeof block;
        }
        if (f && fclose(f) != 0) {
            ok = false;
        }
        if (ok) {
            s_sd_writes++;
        } else {
            s_load_errors++;
            vTaskDelay(pdMS_TO_TICKS(LOAD_IDLE_MS));
        }
    }
}

// Sets up the radio without starting it. NVS is never erased: ambient keeps its
//   settings there, and WiFi only uses it for calibration data.
static bool wifi_setup(void)
{
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        printf("NVS init: %s, carrying on\n", esp_err_to_name(err));
    }
    err = esp_event_loop_create_default();
    if (err == ESP_OK) {
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        err = esp_wifi_init(&cfg);
    }
    if (err == ESP_OK) {
        err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    }
    if (err == ESP_OK) {
        err = esp_wifi_set_mode(WIFI_MODE_STA);
    }
    if (err != ESP_OK) {
        printf("WiFi setup FAILED: %s\n", esp_err_to_name(err));
        return false;
    }
    esp_log_level_set("wifi", ESP_LOG_WARN);
    return true;
}

// Mounts the card without ever formatting it, as in sd-probe.
static bool sd_setup(void)
{
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI3_HOST;
    host.max_freq_khz = SD_FREQ_KHZ;

    spi_bus_config_t bus = {
        .mosi_io_num = PIN_MOSI,
        .miso_io_num = PIN_MISO,
        .sclk_io_num = PIN_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };
    esp_err_t err = spi_bus_initialize(host.slot, &bus, SDSPI_DEFAULT_DMA);
    if (err == ESP_OK) {
        sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
        slot.gpio_cs = PIN_CS;
        slot.host_id = host.slot;
        esp_vfs_fat_mount_config_t mount = {
            .format_if_mount_failed = false,
            .max_files = 3,
            .allocation_unit_size = 16 * 1024,
        };
        sdmmc_card_t *card = NULL;
        err = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot, &mount, &card);
    }
    if (err != ESP_OK) {
        printf("SD setup FAILED: %s\n", esp_err_to_name(err));
        return false;
    }
    return true;
}

void app_main(void)
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
    ESP_ERROR_CHECK(gpio_config(&io));

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
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = ADS_ADDR,
        .scl_speed_hz = SCL_HZ,
    };
    i2c_master_dev_handle_t dev;
    err = i2c_master_bus_add_device(bus, &dev_cfg, &dev);
    if (err != ESP_OK) {
        printf("device setup FAILED: %s\n", esp_err_to_name(err));
        return;
    }

    // Without a load the idle and load phases are the same, so say so.
    if (wifi_setup()) {
        xTaskCreate(wifi_load_task, "wifi_load", LOAD_STACK, NULL, 1, NULL);
    } else {
        printf("no WiFi load\n");
    }
    if (sd_setup()) {
        xTaskCreate(sd_load_task, "sd_load", LOAD_STACK, NULL, 1, NULL);
    } else {
        printf("no SD load\n");
    }

    static double temps[SPREAD_SETS];
    int count = 0, next = 0;

    for (int pass = 1;; pass++) {
        printf("\nads1115 probe, pass %d\n", pass);

        double off = 0, a0_first = 0, a1 = 0, a0_last = 0;
        err = ads_convert(dev, ADS_CFG_MUX_A0, &off);
        if (err == ESP_OK) {
            err = read_excited(dev, &a0_first, &a1, &a0_last);
        }
        if (err != ESP_OK) {
            printf("  conversion FAILED: %s\n  FAIL\n", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(READ_PERIOD_MS));
            continue;
        }

        double a0 = (a0_first + a0_last) / 2.0;
        double ratio = a1 / a0;
        bool off_ok = fabs(off) <= A0_OFF_MAX_V;
        bool on_ok = a0 >= A0_ON_MIN_V && a0 <= A0_ON_MAX_V
                     && fabs(a0_first - a0_last) <= A0_DRIFT_MAX_V;
        bool ratio_ok = on_ok && ratio >= RATIO_MIN && ratio <= RATIO_MAX;

        printf("  switch off  A0 %.4f V  %s\n", off,
               off_ok ? "PASS" : "FAIL (switch not off)");
        printf("  switch on   A0 %.4f V  A1 %.4f V  A0 %.4f V  %s\n",
               a0_first, a1, a0_last, on_ok ? "PASS" : "FAIL (excitation out of range)");
        if (!on_ok) {
            printf("  no ratio without a good excitation\n");
        } else if (!ratio_ok) {
            printf("  ratio %.5f  FAIL (%s)\n", ratio, ratio < RATIO_MIN ? "short" : "open");
        } else {
            double r = resistance_ohms(ratio);
            double t = steinhart_hart_c(r);
            temps[next] = t;
            next = (next + 1) % SPREAD_SETS;
            count = count < SPREAD_SETS ? count + 1 : SPREAD_SETS;

            double mean, sd, range;
            spread(temps, count, &mean, &sd, &range);
            printf("  ratio %.5f  %.1f ohm  %.2f C  PASS\n", ratio, r, t);
            printf("  last %d: sd %.4f C, min-to-max %.4f C\n", count, sd, range);
            if (count == SPREAD_SETS) {
                printf("  PHASE %s: mean %.4f C, sd %.4f C, min-to-max %.4f C, "
                       "%lu scans, %lu SD writes, %lu load errors\n",
                       s_load_on ? "load" : "idle", mean, sd, range,
                       (unsigned long)s_scans, (unsigned long)s_sd_writes,
                       (unsigned long)s_load_errors);
                s_scans = s_sd_writes = s_load_errors = 0;
                s_load_on = !s_load_on;
                count = 0;
                next = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(READ_PERIOD_MS));
    }
}
