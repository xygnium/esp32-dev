#ifndef AMBIENT_CLOCK_H
#define AMBIENT_CLOCK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Time chain: logserv sets the RTC (DS3231), the RTC sets the ESP32 clock,
// and the ESP32 clock (UTC, microseconds) stamps readings.
//
// - Boot: wait for the RTC's next second edge and set the ESP32 clock to
//   that second.
// - Sync to logserv: on the first ack after boot, and on the first ack 24 h
//   after the last sync. True time is logserv time plus half the round trip,
//   at the moment the ack arrived. The RTC's time at that moment is found
//   from its next tick; logserv minus RTC is kept (the last CLOCK_SYNCS, in
//   NVS). The RTC is then written at the next whole second of true time, and
//   the ESP32 clock set from it.
// - Faults (fault.h, source "rtc"): RTC missing; invalid (oscillator-stop
//   flag set) with data on the card; or failing a read while running
//   (not_responding). Acks never clear a fault.
// - Invalid with an empty card is a remediated restart: the clock stays unset
//   until the first ack sets the RTC (and clears the flag).

#define CLOCK_SYNCS 10

// At boot, after settings_init() and sd_init(). Raises a fault on failure
// (returns -1).
int clock_init(void);

// True once the ESP32 clock is set from a valid RTC.
bool clock_is_set(void);

// On every ack: reply is the ack text, rtt_us the round trip, arrived_us the
// esp_timer time the ack arrived. Syncs to logserv when due.
void clock_on_ack(const char *reply, int64_t rtt_us, int64_t arrived_us);

typedef struct {
    int64_t utc_s;                // when the sync was done
    int32_t diff_ms;              // logserv minus RTC, just before the sync
} clock_sync_t;

typedef struct {
    bool set;
    int64_t last_sync_up_s;       // uptime of the last logserv sync; -1 none since boot
    clock_sync_t syncs[CLOCK_SYNCS];  // oldest first
    int n_syncs;
} clock_info_t;

void clock_get_info(clock_info_t *out);

// Current UTC time as 2026-11-06T07:08:19.482Z.
void clock_now_str(char *buf, size_t size);

#endif
