#ifndef AMBIENT_SHT45_H
#define AMBIENT_SHT45_H

#include <stdint.h>

// SHT45 at 0x44 (ported from sensor-probe): high-precision measurement, both
// CRCs checked, humidity clamped to 0..100 %.

int sht45_init(void);

// One reading. Returns NULL with the values and the UTC time (us) the result
// was read, or a reason: nack, timeout, bus, crc, range (a value outside the
// part's range, or humidity more than 5 % outside 0..100 %).
const char *sht45_read(double *temp_c, double *rh_pct, int64_t *utc_us);

#endif
