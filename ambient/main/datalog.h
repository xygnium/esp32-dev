#ifndef AMBIENT_DATALOG_H
#define AMBIENT_DATALOG_H

#include <stdbool.h>
#include <stdint.h>

#include "sensors.h"

// The logger's files on the SD card (FIRMWARE.md, SD logging).
//
// - Readings: one CSV per UTC day, /sdcard/YYYY-MM-DD.csv, one row per
//   reading, the columns of logserv's readings table:
//     timestamp,sensor,data_type,value,status
//   A failed reading has an empty value and its reason as the status.
// - Events: /sdcard/events.csv, kept for good:
//     timestamp,source,event,detail
//   Events seen before the clock is set are held in RAM (up to 32) and
//   written once it is, with stamps worked back from uptime.
//
// Each write is append, fsync, close, so a power cut loses at most the
// sample being written. A failed write is cut back to the file's old size,
// so rows stay whole, and raises the hard fault sd=full (little or no space
// left) or sd=write_failed. Nothing is written during an SD fault.
//
// Free space is checked at boot, at each new day file and hourly; under 10 %
// free the logger is "low" and logs an sd low event once.

// After sd_init and clock_init.
void datalog_init(void);

// Write one scheduled sample (all three sensors).
void datalog_sample(const sensor_result_t r[SENSOR_COUNT]);

// Log an event. source and event are short words (sht45, fail); detail is
// printf-style and may be empty. Safe from any task.
void datalog_event(const char *source, const char *event, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

// From the main loop: writes held events once the clock is set, and runs the
// hourly free-space check.
void datalog_poll(void);

typedef struct {
    bool low;                 // under 10 % free
    char file[24];            // the readings file last written; "" none yet
    uint32_t file_bytes;      // its size after that write
    uint32_t rows;            // reading rows written since boot
    uint32_t events;          // event rows written since boot
    uint32_t held;            // events waiting for the clock
    uint32_t dropped;         // events lost: held list full
} datalog_info_t;

void datalog_get_info(datalog_info_t *out);

// Console `tail`: print the last `lines` lines of a file in the card's root
// (a bare name, e.g. events.csv). Returns 0, or -1 with a message printed.
int datalog_tail(const char *name, int lines);

// Console `tail` with no file: list the files in the card's root with sizes.
int datalog_list(void);

#ifdef AMBIENT_TEST_FAULTS
// Test builds only: one readings file per UTC minute (YYYY-MM-DDTHHMM.csv)
// instead of per day, until reboot.
void datalog_test_minute_files(bool on);
bool datalog_test_minute_files_on(void);
#endif

#endif
