#include "sd.h"

#include <dirent.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#include "fault.h"

static const char *TAG = "sd";

#define PIN_SCK     18
#define PIN_MISO    19
#define PIN_MOSI    23
#define PIN_CS      4
#define SD_FREQ_KHZ 4000          // hand-wired perfboard, as sd-probe

static sdmmc_card_t *s_card;
static bool s_empty;
// The health check (main loop) and the size query (console) both talk to the
// card; one at a time.
static SemaphoreHandle_t s_lock;

// Empty: nothing in the root but the folder Windows makes when formatting.
// Long file names are off, so FAT reports that folder by its short name.
static bool root_is_empty(void)
{
    DIR *d = opendir(SD_MOUNT_POINT);
    if (!d) {
        return false;
    }
    bool empty = true;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcasecmp(e->d_name, "System Volume Information") != 0
            && strcasecmp(e->d_name, "SYSTEM~1") != 0) {
            empty = false;
            break;
        }
    }
    closedir(d);
    return empty;
}

int sd_init(void)
{
    s_lock = xSemaphoreCreateMutex();

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
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init: %s", esp_err_to_name(err));
        fault_raise(FAULT_SD, "mount_failed");
        return -1;
    }

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = PIN_CS;
    slot.host_id = host.slot;

    esp_vfs_fat_mount_config_t mount = {
        .format_if_mount_failed = false,  // never format: cards are formatted on a PC
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };
    err = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot, &mount, &s_card);
    if (err != ESP_OK) {
        // ESP_FAIL: a card answered but has no FAT filesystem that mounts.
        // Anything else: no card answered.
        ESP_LOGE(TAG, "mount: %s", esp_err_to_name(err));
        fault_raise(FAULT_SD, err == ESP_FAIL ? "mount_failed" : "missing");
        s_card = NULL;
        return -1;
    }
    s_empty = root_is_empty();
    ESP_LOGI(TAG, "mounted, %s", s_empty ? "empty" : "not empty");
    return 0;
}

void sd_check(void)
{
    if (!s_card || fault_get(NULL, NULL)) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = sdmmc_get_status(s_card);
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "status: %s", esp_err_to_name(err));
        fault_raise(FAULT_SD, "not_responding");
    }
}

void sd_get_info(sd_info_t *out)
{
    *out = (sd_info_t){ .mounted = s_card != NULL, .empty = s_empty };
    // In a fault the card may not answer; don't wait on it.
    if (!s_card || fault_get(NULL, NULL)) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->size_ok = esp_vfs_fat_info(SD_MOUNT_POINT, &out->total_bytes, &out->free_bytes) == ESP_OK;
    xSemaphoreGive(s_lock);
}

bool sd_is_empty(void)
{
    return s_card != NULL && s_empty;
}
