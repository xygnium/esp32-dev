// Serial link probe. Prints a fixed copy of an ads1115-probe pass every
//   READ_PERIOD_MS, with no sensors, I2C, SD or WiFi, so any lost or garbled
//   output comes from the serial link (CP2102, USB, hub, VM passthrough).
// Only the pass number changes, so a lost block can be counted.
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define READ_PERIOD_MS  30000

void app_main(void)
{
    for (int pass = 1;; pass++) {
        printf("\nads1115 probe, pass %d\n"
               "  SHT45  25.00 C  52.9 %%RH  PASS\n"
               "  switch off  A0 0.0018 V  PASS\n"
               "  switch on   A0 3.3101 V  A1 1.6558 V  A0 3.3101 V  PASS\n"
               "  ratio 0.50021  10031.7 ohm  24.93 C  PASS\n"
               "  thermistor - SHT45: -0.072 C\n"
               "  last 9: sd 0.0057 C, min-to-max 0.0172 C\n", pass);
        vTaskDelay(pdMS_TO_TICKS(READ_PERIOD_MS));
    }
}
