#ifndef AMBIENT_H
#define AMBIENT_H

#include <stdbool.h>
#include <stdint.h>

// What the push loop (main.c) shares with the console commands.

typedef enum {
    PUSH_NONE = 0,        // no push yet this boot
    PUSH_OK,              // ack received
    PUSH_NO_REPLY,        // sent, no ack within the timeout
    PUSH_SEND_FAILED,     // socket or send error
    PUSH_NOT_CONNECTED,   // WiFi down at push time; nothing sent
} push_result_t;

typedef struct {
    uint32_t seq;               // seq of the last push attempt
    push_result_t last;
    uint32_t last_uptime_s;     // when the last attempt finished
    char last_reply[48];        // start of the last ack, if any
    uint32_t n_ok, n_no_reply, n_send_failed, n_not_connected;
    uint32_t next_in_s;         // seconds until the next scheduled push
} push_stats_t;

// Copy of the push loop's counters.
void ambient_get_push_stats(push_stats_t *out);

// Push now, outside the schedule.
void ambient_request_push(void);

// Wake the push loop so it re-reads settings (push interval, listener).
void ambient_wake(void);

// Quiet mode: when on, successful pushes are not printed on the serial
// console (problems still are), so log lines don't land in the middle of
// typing. On at every boot; not saved. `quiet off` shows them while
// debugging.
void ambient_set_quiet(bool on);
bool ambient_quiet(void);

#endif
