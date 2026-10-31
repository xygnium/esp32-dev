// Thermistor switch probe, kept as a permanent diagnostic. Repeats until reset.
// The switch pin drives the AO3401 gate, low = on. The 100k pull-up holds the
//   switch off while the pin floats during boot and reset.
// PIN_SWITCH is GPIO17 (TX2) for the breadboard, which has no free socket beside
//   D25. The perfboard uses GPIO25: change PIN_SWITCH to test there.
// Holds each state for HOLD_MS so it can be measured with a meter:
//   - on: pin low, drain at about 3.3 V,
//   - off: pin high, drain at about 0 V. Also measure the gate against the
//     3.3 V bus here: it has to stay within about 0.4 V, or the switch doesn't
//     turn fully off.
// There is no PASS or FAIL, since the ESP32 can't see the drain. The meter decides.
#include <stdio.h>

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define PIN_SWITCH  GPIO_NUM_17
#define HOLD_MS     10000

void app_main(void)
{
    // Set the level first, so the pin comes up high and the switch never pulses on.
    gpio_set_level(PIN_SWITCH, 1);
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PIN_SWITCH,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));

    printf("switch-probe: GPIO%d drives the AO3401 gate, %d s per state\n",
           PIN_SWITCH, HOLD_MS / 1000);

    for (int pass = 1; ; pass++) {
        gpio_set_level(PIN_SWITCH, 1);
        printf("pass %d: GPIO%d high, switch OFF, expect drain about 0 V, gate within 0.4 V of 3.3 V\n",
               pass, PIN_SWITCH);
        vTaskDelay(pdMS_TO_TICKS(HOLD_MS));

        gpio_set_level(PIN_SWITCH, 0);
        printf("pass %d: GPIO%d low, switch ON, expect drain about 3.3 V\n", pass, PIN_SWITCH);
        vTaskDelay(pdMS_TO_TICKS(HOLD_MS));
    }
}
