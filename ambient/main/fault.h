#ifndef AMBIENT_FAULT_H
#define AMBIENT_FAULT_H

#include <stdbool.h>

// Hard faults: a missing or failing SD card or DS3231 clock. A fault
// stops normal work; the console stays up and the push loop reports the
// fault on the console and to the server each push interval. Only a reboot
// clears it. The first fault raised is kept; later ones are logged only.

typedef enum {
    FAULT_NONE = 0,
    FAULT_SD,
    FAULT_RTC,
} fault_src_t;

// Raise a fault. reason must be a string literal (it is kept by pointer),
// one word, as sent in the fault line, e.g. "missing".
void fault_raise(fault_src_t src, const char *reason);

// True when a fault is active; fills in the source and reason if non-NULL.
bool fault_get(fault_src_t *src, const char **reason);

// "sd", "rtc", or "none".
const char *fault_src_name(fault_src_t src);

#endif
