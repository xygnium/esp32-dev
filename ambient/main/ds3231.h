#ifndef AMBIENT_DS3231_H
#define AMBIENT_DS3231_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

// DS3231 real-time clock at 0x68 on I2C bus 0 (ported from rtc-probe). The
// clock is kept in UTC, 24-hour mode. Its time registers count whole seconds.
// All functions return 0 on success, -1 on a bus error or an impossible value.

// addr is normally DS3231_ADDR; test builds can pass an unused address to
// simulate a missing chip.
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

#endif
