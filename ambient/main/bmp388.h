#ifndef AMBIENT_BMP388_H
#define AMBIENT_BMP388_H

#include <stdint.h>

// BMP388 (or BMP390) at 0x76 (ported from sensor-probe). Soft reset and
// calibration read once, then one forced measurement per reading. After any
// failure the reset and calibration are redone at the next reading, so a
// replaced or power-cycled chip comes back.

int bmp388_init(void);

// One reading. Returns NULL with the values and the UTC time (us) the result
// was read, or a reason: nack, timeout, bus, chip_id, reset, not_ready,
// range (outside -40..85 C or 300..1250 hPa).
const char *bmp388_read(double *temp_c, double *pressure_pa, int64_t *utc_us);

#endif
