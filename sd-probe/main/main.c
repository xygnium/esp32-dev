// SD card probe, kept as a permanent diagnostic. Runs once after reset.
// SPI pins: SCK = D18, MISO = D19, MOSI = D23, CS = D4 (VSPI host).
// Mounts the card (never formats it), prints its info, writes a small file,
//   reads it back and compares, deletes it, then prints the card's size and
//   free space. Prints PASS or FAIL at the end.
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "esp_err.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

#define PIN_SCK     18
#define PIN_MISO    19
#define PIN_MOSI    23
#define PIN_CS      4
// Hand-wired perfboard: start slow. Raise toward 20000 once it works.
#define SD_FREQ_KHZ 4000
#define MOUNT_POINT "/sdcard"
#define TEST_FILE   MOUNT_POINT "/probe.txt"
#define TEST_TEXT   "sd-probe: 0123456789 abcdefghijklmnopqrstuvwxyz\n"

static bool file_round_trip(void)
{
    FILE *f = fopen(TEST_FILE, "w");
    if (!f) {
        printf("  FAIL: cannot open %s for writing\n", TEST_FILE);
        return false;
    }
    fputs(TEST_TEXT, f);
    fclose(f);

    f = fopen(TEST_FILE, "r");
    if (!f) {
        printf("  FAIL: cannot open %s for reading\n", TEST_FILE);
        return false;
    }
    char buf[96] = { 0 };
    fgets(buf, sizeof buf, f);
    fclose(f);
    unlink(TEST_FILE);

    if (strcmp(buf, TEST_TEXT) != 0) {
        printf("  FAIL: read back \"%s\", wrote \"%s\"\n", buf, TEST_TEXT);
        return false;
    }
    printf("  wrote, read back and deleted %s: OK\n", TEST_FILE);
    return true;
}

void app_main(void)
{
    printf("\nsd probe: SCK = GPIO %d, MISO = GPIO %d, MOSI = GPIO %d, CS = GPIO %d, %d kHz\n",
           PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CS, SD_FREQ_KHZ);

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
        printf("  FAIL: SPI bus init: %s\n", esp_err_to_name(err));
        return;
    }

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
    if (err != ESP_OK) {
        printf("  FAIL: mount: %s\n", esp_err_to_name(err));
        if (err == ESP_FAIL) {
            printf("  the card answered but has no FAT filesystem it can mount\n");
        } else {
            printf("  no card answered: check the wires against WIRING.md, and that the card is seated\n");
        }
        spi_bus_free(host.slot);
        return;
    }

    sdmmc_card_print_info(stdout, card);
    bool ok = file_round_trip();

    uint64_t total = 0, free_bytes = 0;
    if (esp_vfs_fat_info(MOUNT_POINT, &total, &free_bytes) == ESP_OK) {
        printf("  filesystem: %llu MB total, %llu MB free\n",
               (unsigned long long)(total >> 20), (unsigned long long)(free_bytes >> 20));
    }

    // Unmounting prints "gpio: conflict found for GPIO[4]". It is harmless: the
    //   SD driver's teardown puts the CS pin back to an input while the pin is
    //   still reserved as an output, and the GPIO driver only warns (ESP-IDF 6.1).
    printf("  note: a \"gpio: conflict found for GPIO[%d]\" warning next is harmless (SD driver teardown)\n",
           PIN_CS);
    esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);
    spi_bus_free(host.slot);
    printf("sd probe: %s\n", ok ? "PASS" : "FAIL");
}
