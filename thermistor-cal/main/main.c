// Stage 1f: resolution test. Single-shot conversions of A0 (pot wiper) at 8 SPS,
// PGA ±4.096 V. Prints each raw code, and a summary every SUMMARY_N readings.
// A genuine ADS1115 gives odd codes and a varied low nibble as the pot turns;
// an ADS1015 gives only multiples of 16.
// I2C bus 0: SDA = D21, SCL = D22. ADS at 0x48 (ADDR pulled low on the board).
#include <stdint.h>
#include <stdio.h>

#include "ads1115.h"
#include "ads_port.h"

#define I2C_PORT   0
#define PIN_SDA    GPIO_NUM_21
#define PIN_SCL    GPIO_NUM_22
#define ADS_ADDR   0x48
#define SCL_HZ     100000
#define SUMMARY_N  40    // about 5 s at 8 SPS
#define LSB_UV     125   // PGA ±4.096 V

void app_main(void)
{
    i2c_master_bus_handle_t bus;
    ads_hal_t ads;
    ESP_ERROR_CHECK(ads_port_bus_init(I2C_PORT, PIN_SDA, PIN_SCL, &bus));
    ESP_ERROR_CHECK(ads_port_device_init(bus, ADS_ADDR, SCL_HZ, &ads));

    // One conversion, then read Config back: shows the chip took the MUX/PGA/DR bits.
    int16_t code;
    uint16_t cfg;
    int err = ads_convert(&ads, ADS_MUX_A0_GND, ADS_PGA_4V096, ADS_DR_8SPS, &code);
    if (err == 0 && ads_read_reg(&ads, ADS_REG_CONFIG, &cfg) == 0) {
        printf("config after first conversion: 0x%04x (expect 0xc303)\n", cfg);
    } else {
        printf("first conversion or config read FAILED (%d)\n", err);
    }

    for (;;) {
        int16_t min = INT16_MAX, max = INT16_MIN;
        int odd = 0, errors = 0, good = 0;
        int nibble[16] = { 0 };

        for (int i = 0; i < SUMMARY_N; i++) {
            err = ads_convert(&ads, ADS_MUX_A0_GND, ADS_PGA_4V096, ADS_DR_8SPS, &code);
            if (err != 0) {
                printf("conversion FAILED (%d)\n", err);
                errors++;
                continue;
            }
            printf("%6d  0x%04x  %8d uV\n", code, (uint16_t)code, code * LSB_UV);
            good++;
            if (code < min) min = code;
            if (code > max) max = code;
            odd += code & 1;
            nibble[code & 0xF]++;
        }

        printf("--- %d readings: min %d  max %d  odd %d  errors %d\n",
               good, min, max, odd, errors);
        printf("--- low nibble:");
        for (int n = 0; n < 16; n++) {
            printf(" %x:%d", n, nibble[n]);
        }
        printf("\n\n");
    }
}
