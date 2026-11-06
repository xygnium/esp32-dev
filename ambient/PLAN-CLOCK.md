# Plan: clocks and time stamps (project 2 of 2)

Temporary. When this is implemented, its decisions move into [FIRMWARE.md](FIRMWARE.md) and this file is deleted. Builds on project 1 (done; see "SD card and hard faults" in [FIRMWARE.md](FIRMWARE.md)): the hard-fault mechanism, `test fault` and the empty-card check (`sd_is_empty()`).

## Decided

- **Time chain:** the server's ack time corrects the DS3231, the DS3231 syncs the ESP32 clock, and the ESP32 clock stamps each sensor reading. Each reading gets its own UTC stamp with milliseconds, taken when its measurement finishes. The BMP388's temperature and pressure come from one measurement, so they share a stamp.
- **ESP32 sync:** at boot, wait for the DS3231's next second edge and set the ESP32 clock to that second. Resync the same way every hour, slewing with `adjtime` rather than stepping, so time never jumps backwards.
- **Ack:** the server sends milliseconds. The logger takes the ack time plus half the measured round trip as the true time. With `clock auto`, it corrects the DS3231, writing the seconds register at a whole-second moment. With `clock report`, it only reports the offset.
- **DS3231 faults:**
  - Missing: hard fault.
  - Invalid (oscillator-stop flag set) with data on the card: hard fault.
  - Invalid with an empty card: this is a remediated restart. Wait for the first ack to set the time, then run normally.
  - Fails a read while running, including at the hourly resync: hard fault, `fault rtc=not_responding`.
  - Acks never clear a fault.
- **Remediation** for a failed clock: replace the board or battery, reformat the card on a PC, and restart. The first ack sets the time. Data from before a detected fault isn't used.

## Firmware (ambient)

1. I2C bus 0: SDA 21, SCL 22, 100 kHz. The sensors will share it later.
2. **DS3231 driver,** ported from `rtc-probe`: read and write the time, read and clear the oscillator-stop flag.
3. **`clock.c`/`clock.h`:** the edge-aligned boot sync, the hourly slew, and the correction from the ack.
4. **Boot:** check the DS3231 and apply the fault rules above, using project 1's empty-card check. After remediation there is no data: sampling waits until the time is set. The logger sends a **time request**, the status line marked `clock=unset`, and retries every 10 s until an ack sets the time.
5. **Push:** time the round trip, parse the ack's milliseconds, and apply `clock auto` or `clock report`.
   - When to correct: only when the offset is over 50 ms, so the DS3231 isn't rewritten at every push.
   - After a DS3231 correction, resync the ESP32 at once.
6. **Fault injection:** add `test fault rtc <reason>` to project 1's test-build command, for faults that can't be caused by hand, such as a failed DS3231 read at the hourly resync.
7. **Console:** `status` gains a `clock` line (UTC time, last sync, last correction and the offset, any fault). The "(not used yet)" note on `clock` in `config get` goes.
8. Build, both with and without the test flag.

## Server (logserv)

1. `udp_listener.py`: send the ack with milliseconds, `ack 2026-11-06T07:08:19.482Z`.

## Docs

1. `FIRMWARE.md`: a "Time" section (the chain, the sync, the ack correction, the fault rules, remediation), and update Open items.
2. `main.c` header comment.
3. logserv `PROTOCOL.md`: the ack with milliseconds, the time request (`clock=unset`), and `fault rtc=<missing|invalid|not_responding>`. logserv `README.md`: time stamps are UTC with milliseconds.
4. logserv `TEST-AMBIENT.md`: `config interval` instead of `config push`.

## Test (on the board)

1. `config interval`: `2m 10m` is accepted, while `2m 5m` and `10s 1m` are refused. `config get` shows the new line. *(Left from the interval change, which is committed but untested on the board.)*
2. Boot sync: the logger's time matches dev10 to within a few ms.
3. Ack correction: offset the DS3231 deliberately and see `clock auto` correct it. Check `clock report` shows the offset without changing the clock.
4. Unplug the DS3231 and boot: `fault rtc=missing` on the console and dev10.
5. Remediation: set the oscillator-stop flag with an empty card. The logger waits, the first ack sets the time, and it runs normally. With data on the card, it's a hard fault.
6. Test build: `test fault rtc not_responding` gives the same console output and fault line as a real fault.
7. Run for several hours: the hourly corrections stay small.

Then commit, and the user pushes.
