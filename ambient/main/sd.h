#ifndef AMBIENT_SD_H
#define AMBIENT_SD_H

#include <stdbool.h>
#include <stdint.h>

// The SD card, mounted at /sdcard. The logger never formats it: cards are
// formatted on a PC (FAT32). A missing card, a failed mount or a card that
// stops answering is a hard fault (fault.h).
//
// SPI on the VSPI host: SCK 18, MISO 19, MOSI 23, CS 4, 4 MHz (as sd-probe).

#define SD_MOUNT_POINT "/sdcard"

// Mount the card. On failure raises the fault (missing or mount_failed) and
// returns -1; 0 when mounted.
int sd_init(void);

// Health check, run at each sample interval: asks the card for its status
// (read-only, nothing written). Raises not_responding if it doesn't answer.
// Does nothing when the card isn't mounted or a fault is already active.
void sd_check(void);

typedef struct {
    bool mounted;
    bool empty;            // at boot: nothing in the root but System Volume Information
    bool size_ok;          // total_bytes and free_bytes are valid
    uint64_t total_bytes;
    uint64_t free_bytes;
} sd_info_t;

void sd_get_info(sd_info_t *out);

// True when the card was empty at boot (see sd_info_t.empty).
bool sd_is_empty(void);

// True when the card is mounted and there is no SD fault.
bool sd_usable(void);

// One user of the card at a time (health check, size query, logging). Not
// recursive: don't call sd_check or sd_get_info while holding it.
void sd_lock(void);
void sd_unlock(void);

#endif
