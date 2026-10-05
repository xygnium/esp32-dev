#ifndef AMBIENT_SUPPLY_H
#define AMBIENT_SUPPLY_H

#include <stdbool.h>
#include <stdint.h>

// Supply voltage (VIN, the USB 5 V) read on GPIO 34 through a 2 x 100 kOhm
// divider, so the pin sees half of VIN. ESP-IDF's ADC calibration (from the
// chip's eFuse reference) converts the raw reading to millivolts.
//
// Low-supply watch: when VIN drops below the low threshold (a setting) a
// "supply low" warning is logged and counted; it clears ("recovered") once VIN
// is back above the threshold plus a small margin, so a reading hovering at
// the threshold doesn't flap.

typedef struct {
    bool valid;            // a reading has been taken
    uint32_t vin_mv;       // last VIN reading
    bool low;              // currently below the threshold
    uint32_t n_low;        // times it went low since boot
} supply_state_t;

// Set up the ADC. Returns 0 when readings are available; -1 if not (logged;
// the logger carries on without a voltage reading).
int supply_init(void);

// Read VIN (average of several samples), update the low-supply watch against
// low_mv, and log a warning on each change. Returns 0 and sets *vin_mv, or -1
// if the ADC isn't available.
int supply_check(uint32_t low_mv, uint32_t *vin_mv);

// Copy of the latest reading and low-supply state (safe from any task).
void supply_get(supply_state_t *out);

// How readings are calibrated: "eFuse Vref", "eFuse two-point", "default Vref"
// or "none".
const char *supply_cal_name(void);

#endif
