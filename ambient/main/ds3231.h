#ifndef AMBIENT_DS3231_H
#define AMBIENT_DS3231_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

// DS3231 real-time clock at 0x68 on I2C bus 0 (ported from rtc-probe). The
// clock is kept in UTC, 24-hour mode. Its time registers count whole seconds.
// All functions return 0 on success, -1 on a bus error or an impossible value.

#define DS3231_ADDR 0x68
int ds3231_init(uint8_t addr);

// Full time (UTC) and the oscillator-stop flag (set: the clock lost all power,
// so its time can't be trusted).
int ds3231_read(struct tm *utc, bool *osf);

// The seconds register only (0-59), for finding the second edge.
int ds3231_read_seconds(int *sec);

// Write the time. Writing the seconds register restarts the DS3231's 1 s
// countdown (datasheet), so call this at a whole-second moment.
int ds3231_write(const struct tm *utc);

int ds3231_clear_osf(void);

// Aging offset (register 0x10), kept by the backup battery. About 0.1 ppm a
// step at 25 C; positive slows the clock, negative speeds it up. The write
// forces a temperature conversion so it takes effect now (if one is already
// running, it takes effect at the next automatic one, within 64 s).
int ds3231_read_aging(int8_t *v);
int ds3231_write_aging(int8_t v);


#endif
