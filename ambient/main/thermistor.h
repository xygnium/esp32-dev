#ifndef AMBIENT_THERMISTOR_H
#define AMBIENT_THERMISTOR_H

#include <stdint.h>

// Thermistor through the ADS1115 at 0x48 (ported from ads1115-probe), read as
// in FIRMWARE.md, Table 1: excitation on (GPIO25 low), wait, convert A0, A1,
// A0, excitation off. The ratio is A1 over the mean of the two A0 readings.

// First thing at boot: drive GPIO25 high so the excitation is off.
void thermistor_pin_init(void);

// Adds the device. Returns 0, or -1 if it can't (reads then fail).
int thermistor_init(void);

// One reading. Returns NULL with temp_c, ratio and the UTC time (us) when the
// last conversion finished, or a reason: nack, timeout, bus, excitation,
// open, short. The excitation is always off again on return.
const char *thermistor_read(double *temp_c, double *ratio, int64_t *utc_us);

#endif
