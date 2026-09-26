// Stage 1a: scan I2C bus 0 (SDA = D21, SCL = D22) and print the addresses found.
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ads_port.h"

#define I2C_PORT 0
#define PIN_SDA  GPIO_NUM_21
#define PIN_SCL  GPIO_NUM_22

static void scan(i2c_master_bus_handle_t bus)
{
    int found = 0, timeouts = 0;
    printf("scan:");
    for (uint16_t addr = 0x08; addr < 0x78; addr++) {
        esp_err_t err = i2c_master_probe(bus, addr, 50);
        if (err == ESP_OK) {
            printf(" 0x%02x", addr);
            found++;
        } else if (err == ESP_ERR_TIMEOUT) {
            timeouts++;
        }
    }
    printf("%s (found %d, timeouts %d)\n", found ? "" : " none", found, timeouts);
}

void app_main(void)
{
    i2c_master_bus_handle_t bus;
    ESP_ERROR_CHECK(ads_port_bus_init(I2C_PORT, PIN_SDA, PIN_SCL, &bus));

    while (1) {
        scan(bus);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
