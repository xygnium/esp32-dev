#ifndef AMBIENT_DATALOG_H
#define AMBIENT_DATALOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sensors.h"

// The logger's files on the SD card (FIRMWARE.md, SD logging).
//
// - Readings: one CSV per UTC day, /sdcard/YYYY-MM-DD.csv, one row per
//   reading: timestamp,sensor,data_type,value,status
//   A failed reading has an empty value and its reason as the status.
// - Events: /sdcard/events.csv: timestamp,source,event,detail
//
// Each write is append, fsync, close. A failed write raises sd=write_failed.
// Nothing is written while the clock is unset or during an SD fault.
//
// Every reading and event is sent to the listener, one message each:
//   data <timestamp,sensor,data_type,value,status>
//   event <timestamp,source,event,detail>
// They're sent from the card, so nothing is lost while the listener is away
// or over a reboot; sent.txt keeps the position (datalog.c). Events go ahead
// of readings. An event that isn't written (from before the clock is set, or
// during an SD fault) is sent from a RAM queue instead; one from before the
// clock is set has an empty timestamp.

#define DATALOG_MSG_LEN 160

// The oldest unsent message, copied into msg (DATALOG_MSG_LEN bytes). False
// if none.
bool datalog_next(char *msg);

// The message from datalog_next was acked: drop it.
void datalog_sent(void);

// Format a sample as CSV rows (no header) into buf. Returns the length.
size_t datalog_rows(const sensor_result_t r[SENSOR_COUNT], char *buf, size_t size);

// Write one scheduled sample (all three sensors).
void datalog_sample(const sensor_result_t r[SENSOR_COUNT]);

// Log an event. source and event are short words (sht45, fail); detail is
// printf-style. Safe from any task.
void datalog_event(const char *source, const char *event, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

typedef struct {
    char file[24];            // the readings file last written; "" none yet
    uint32_t file_bytes;      // its size after that write
    uint32_t rows;            // reading rows written since boot
    uint32_t events;          // event rows written since boot
    char sent_file[24];       // the readings file being sent; "" none yet
    uint32_t sent_bytes;      // sent and acked up to this byte of it
} datalog_info_t;

void datalog_get_info(datalog_info_t *out);

// Console `tail`: print the last `lines` lines of a file in the card's root
// (a bare name, e.g. events.csv). Returns 0, or -1 with a message printed.
int datalog_tail(const char *name, int lines);

// Console `tail` with no file: list the files in the card's root with sizes.
int datalog_list(void);


#endif
