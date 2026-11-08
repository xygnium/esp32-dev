#ifndef AMBIENT_CLOCK_H
#define AMBIENT_CLOCK_H

#include <stdbool.h>
#include <stdint.h>

// Time chain: the listener's ack time corrects the DS3231, the DS3231 syncs
// the ESP32 clock, and the ESP32 clock (UTC, microseconds) stamps readings.
//
// - Boot: wait for the DS3231's next second edge and set the ESP32 clock to
//   that second. Resync the same way every hour, slewing with adjtime (no
//   backward jumps) unless the error is over 1 s.
// - Ack: the true time is the ack time plus half the round trip. With
//   `clock auto` and an error over CLOCK_CORRECT_MS, the DS3231 is rewritten
//   at a whole-second moment and the ESP32 resynced from it.
// - Faults (fault.h, source "rtc"): DS3231 missing; invalid (oscillator-stop
//   flag set) with data on the card; or failing a read while running
//   (not_responding). Acks never clear a fault.
// - Invalid with an empty card is a remediated restart: the clock stays unset
//   until the first ack sets the DS3231 (and clears the flag).

#define CLOCK_CORRECT_MS 50

// At boot, after sd_init(). Raises a fault on failure (returns -1).
int clock_init(void);

// True once the ESP32 clock is synced from a valid DS3231.
bool clock_is_set(void);

// esp_timer time of the next hourly resync; INT64_MAX when none is due.
int64_t clock_next_due_us(void);

// The hourly resync, when due.
void clock_poll(void);

// After an acked push: reply is the ack text, rtt_us the round trip.
void clock_on_ack(const char *reply, int64_t rtt_us, bool auto_correct);

typedef struct {
    bool set;
    int64_t last_sync_up_s;       // uptime of the last DS3231 sync; -1 none
    int32_t last_sync_ms;         // DS3231 minus ESP32 at that sync
    bool ack_seen;
    int32_t ack_offset_ms;        // ack time (+ rtt/2) minus ESP32, last ack
    int32_t ack_rtt_ms;
    int64_t correction_up_s;      // uptime of the last DS3231 correction; -1 none
    int32_t correction_ms;        // how far it moved the DS3231
} clock_info_t;

void clock_get_info(clock_info_t *out);


// Current UTC time as 2026-11-06T07:08:19.482Z.
void clock_now_str(char *buf, size_t size);

#endif
