# Plan: SD card (project 1 of 2)

Temporary. When this is implemented, its decisions move into [FIRMWARE.md](FIRMWARE.md) and this file is deleted. Project 2 is [PLAN-CLOCK.md](PLAN-CLOCK.md).

## Decided

- The logger mounts the SD card at boot and **never formats it**. Cards are formatted off-system, on a PC (FAT32).
- A missing or failing card is a **hard fault**. There is no carrying on without it.
- **Hard fault:** normal work stops, the console stays up and keeps printing the fault, and each push interval sends a fault line to the server instead of the status line. Only a reboot clears it. Project 2 uses the same mechanism for the clock.
- No logging in this project. Logging needs time stamps (project 2) and sensor samples.

## Firmware (ambient)

1. **Mount.** Port from `sd-probe`: SPI on the VSPI host, SCK 18, MISO 19, MOSI 23, CS 4, 4 MHz, mounted at `/sdcard`, FAT, no format on failure.
2. **Hard-fault state** in `main.c`, shared with project 2: a fault code and reason, a console message every push interval, and the fault line pushed to the server.
3. **Faults raised:** card missing, mount failed, or the card stops answering.
4. **Health check while running:** at each sample interval, ask the card for its status (`sdmmc_get_status`). That's a read-only check, so it doesn't write to the card. Once logging exists, each sample's write is the check, and a failed write is the fault.
5. **Empty-card check** at boot, for project 2. Empty means no files or directories in the root except `System Volume Information`, which Windows creates when formatting.
6. **Fault injection, test builds only:** a build flag (off by default) adds a console command, `test fault sd <reason>`, which raises the fault through the same path as a real one. It's for faults that can't be caused by hand, such as a card that is present but stops responding. Production builds don't contain it. Project 2 adds `test fault rtc`.
7. **Console:** `status` gains an `sd` line, for example `sd  mounted, 119 MB, 118 MB free, empty`, or `sd  FAULT: card missing`.
8. Build, both with and without the test flag.

## Fault line format

It replaces the `hello` line while in fault, and keeps the logger's identity and timing:

```
fault sd=<missing|mount_failed|not_responding> seq=<n> up=<s> mac=<own MAC>
```

## Docs

1. `FIRMWARE.md`: an "SD card" section (mounting, never formatting, hard faults, the health check), and update Open items.
2. `main.c` header comment.
3. logserv `PROTOCOL.md`: the fault line.

## Test (on the board)

1. Card present: `status` shows it mounted, with its size, free space and whether it's empty.
2. Boot with no card: a hard fault on the console, and the fault line on dev10.
3. Pull the card while running: a fault within one sample interval.
4. In fault: other console commands still work, and `reboot` with the card back in clears it.
5. Card with files on it: `status` shows it not empty.
6. Test build: `test fault sd not_responding` gives the same console output and fault line as a real fault. A production build has no `test` command.

Then commit, and the user pushes.
