# Regression tests

The tests to re-run after a change to the `ambient` firmware: an ESP-IDF upgrade, a new project, or new hardware. The test hooks (`AMBIENT_TEST_FAULTS`) are kept for this. When a change touches a path that a hook exercises, the hook is updated in the same commit, and a new path that is hard to trigger by hand gets a hook. Design and decisions are in [FIRMWARE.md](FIRMWARE.md).

## How to run

- **Builds:** `./build.sh` and `./flash.sh` for production. `./build-test.sh` and `./flash-test.sh` for the test build, which adds the `test` command, and `info` says `TEST`. Flash production again when testing is done.
- **Before each test:** check that the port is there and look at `status`.
- **Opening the serial port resets the board**, and the first sample comes one sample interval (2 min by default) after boot. Allow for both.
- **Reading the card without removing it:** `tail` lists the files, and `tail <file> [lines]` shows the end of one.

## Console and settings

| # | Test | How | Pass |
|---|---|---|---|
| K1 | Interval rules | `config interval 2m 10m`, `2m 5m`, `10s 1m` | The first is accepted. The others are refused (push shorter than sample; sample under 15 s). `config get` shows the line |
| K2 | Settings survive a reflash | Reflash, then `config get` | WiFi, listener and intervals unchanged |
| K3 | Production has no hooks | Production build: `test` | Unknown command |

## SD card

| # | Test | How | Pass |
|---|---|---|---|
| S1 | Card present | `status` | Mounted, with its size, free space, and empty or not at boot |
| S2 | No card at boot | Boot without the card | Hard fault on the console, and `fault sd=missing` at the listener |
| S3 | Card pulled while running | Pull it | A fault at the next sample: `write_failed` or `not_responding` |
| S4 | Fault state | In a fault: other commands; then `reboot` with the card back | The commands work, and the reboot clears it |
| S5 | Not-empty card | Boot with logged files on the card | `status` says `not empty at boot` |
| S6 | Injected fault | Test build: `test fault sd not_responding` (also `full`, `write_failed`) | Same console output and fault line as a real fault |

## Clock

| # | Test | How | Pass |
|---|---|---|---|
| C1 | Boot sync | Run, watch `status` | Once corrected, the ack offset stays within a few ms of dev10 |
| C2 | `clock auto` corrects | Offset the DS3231 by more than 50 ms | The next ack corrects it, and a `clock,corrected` event is logged |
| C3 | `clock report` leaves it | `config clock report`, offset over 50 ms | The offset is shown, and the DS3231 is unchanged |
| C4 | DS3231 missing | Unplug it, or test build: `test rtc-absent on` and reboot | `fault rtc=missing` on the console and at the listener |
| C5 | Remediation | Stop flag set (test build: `test rtc-stop`, then power off a few seconds), card formatted (FAT32), boot | Waits with `clock=unset`. The first ack sets the time (`clock,set_from_ack,stop flag cleared`), then it runs normally |
| C6 | Invalid clock with data | Stop flag set (as C5), data on the card, boot | Hard fault `rtc=invalid` |
| C7 | Injected fault | Test build: `test fault rtc not_responding` | Same output and fault line as a real fault |
| C8 | Resync | Test build: `test resync 15` | The ESP32 is within ±1 ms of the DS3231 at each resync |
| C9 | Drift | Several days of normal running | The hourly resync and ack offsets stay small |

## Sensors

| # | Test | How | Pass |
|---|---|---|---|
| N1 | All read | `read`, or wait for a sample | All three read. The thermistor is within ±0.5 °C of the SHT45 |
| N2 | CAT5 #2 out | Unplug it | Thermistor `open`, SHT45 `nack`, BMP388 reading. Both recover at the first reading after it's plugged back in |
| N3 | CAT5 #1 out | Unplug it | Thermistor and SHT45 `nack`. Both recover |
| N4 | BMP388 out | Pull it from its socket with the power off, boot | `nack`, the others reading. It recovers when put back |

## Logging

| # | Test | How | Pass |
|---|---|---|---|
| L1 | Normal logging | Run a few samples, then `tail` the day file and `events.csv` | Header, then 6 rows per sample with the stamps from `status`. Boot and clock-set events |
| L2 | Failed sensor rows | Unplug CAT5 #2 for two samples | Rows with an empty value and the reason as status. `fail` and `recover` events |
| L3 | Day change | Test build: `test dayfile minute`, `config interval 15s 1m` | A new file each minute, each with a header, and no row lost or doubled across the change |
| L4 | Power cut | Pull the power at a sample time, several times | Every file reads, and at most the last sample is missing or cut short |
| L5 | `sd low` | Fill the card on a PC with a dummy file to under 10 % free | Flagged at boot, one `sd,low` event, shown in `status` |
| L6 | `sd=full` | Fill the card to about 100 KB free, then run at 15 s | Hard fault `full`. The rows before it are whole |
| L7 | Events before the clock | Remediated clock (as C5) | Events held while waiting are written once the ack sets the time, with stamps from before the ack |
| L8 | Header on an empty file | On a PC, replace `events.csv` with an empty file (this also removes the headerless one from 2026-11-07), then boot | The header is written before the first row |
| L9 | `tail` | `tail`, `tail events.csv`, `tail <day file> 8`, `tail ../x`, `tail nope.csv` | The list, the last lines, a name refused, and file not found |
| L10 | Stack headroom | `status` after a day | Main and console still have more than 1 KB free |
| L11 | Long run | Several days at 2 min | About 250 KB a day, with no failures |
