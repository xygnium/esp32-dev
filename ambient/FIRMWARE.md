# Ambient firmware

Decisions and open items for the `ambient` firmware. Wiring is in [WIRING.md](WIRING.md). The firmware currently sends a status line over UDP to the listener (`udp_listener.py` in the `logger-server` repo, which also holds the protocol description), mounts and checks the SD card, keeps time from the DS3231 (see [Time](#time)), and samples the sensors (see [Sensors](#sensors)). It doesn't log or push the readings yet. It doesn't call `esp_wifi_set_ps`, so it runs with ESP-IDF's default power-save mode, `WIFI_PS_MIN_MODEM` (`esp_wifi.h`).

The reading scheme (ratiometric, pulsed excitation on GPIO25, A0 for the excitation and A1 for the tap) is in [How the thermistor is read](WIRING.md#how-the-thermistor-is-read).

## Where the noise comes from

The ADS1115 is delta-sigma with a digital filter. At low data rates (8 to 128 SPS) it rejects energy far above its passband, so 2.4 GHz rarely corrupts the signal directly. The mechanisms that matter:

- **RF rectification** in the input ESD diodes or front end, which gives a DC offset.
- **Supply and ground disturbance** from WiFi transmit bursts (about 200 to 400 mA), which causes rail droop and ground bounce.
- **Long sensor leads** acting as antennas.

The firmware can't time conversions around WiFi activity: beacon wakes and delayed transmits happen on their own. The effect is expected to be far below the thermistor's ±0.5 °C tolerance. The bench test in "Why the ADS1115 box is separate" in WIRING.md measures the effect.

## Sampling

**Table 1: Thermistor sampling decisions (implemented in `thermistor.c`)**

| Setting | Choice | Why |
|---|---|---|
| Data rate | 8 to 32 SPS | Strong filtering; about 125 ms per conversion at 8 SPS |
| Mode | Single-shot | The firmware chooses when each conversion runs |
| Readings | One set per reading: A0, A1, A0 | The excitation is on for about 0.4 s at 8 SPS, which heats the thermistor about 0.006 °C. Sets back to back keep heating it (about 0.03 °C for five), so if more readings are ever wanted, space the sets about 15 s apart |
| Order | Energize, wait, convert A0, A1, A0, de-energize, publish | |
| Excitation | GPIO25 low (switch on) from about 20 ms before the first conversion until the last conversion ends | The inputs climb to their final voltage through the filters: A1 takes about 7 ms at 25 °C and 11 ms at −20 °C to settle within one ADC step. A0 is driven directly by the switch and settles in microseconds. A conversion started at switch-on averages the climb in, so if A1 were converted first the error would be about +0.2 °C at 25 °C and +1.4 °C at −20 °C. With the A0, A1, A0 order A1 starts long after it has settled, so the wait is a margin that costs nothing. The 20 ms figure is from `ads1115-dev/DESIGN.md`, still to be confirmed on the bench |

## WiFi power

**Table 2: WiFi power decisions (proposed)**

| Option | Effect | Decision |
|---|---|---|
| Power-save (`WIFI_PS_MIN_MODEM`) | Stays associated and wakes at each DTIM beacon; the AP buffers packets; IP and DHCP lease survive | Already in use: it is the ESP-IDF default, so no call is needed |
| Radio off (`esp_wifi_stop()`) | Sockets die; reconnecting takes about 1 to 3 s with DHCP (a static IP is faster) | Only for sample intervals of minutes or more; not needed for EMI |

## Remote commands

The logger only pushes: it sends to the collector and waits for the ack on the same socket, and never listens on a port of its own. Settings change only over the serial console. If remote control is wanted, there are two ways to add it.

**Table 3: Remote command options (proposed, not decided)**

| Option | Gains | Costs |
|---|---|---|
| Logger listens on a UDP port | Commands and on-demand readings at any time, within about a beacon interval | An open port: anyone on the LAN can send, and UDP senders are easy to fake, so settings changes need a shared-secret check. The collector must know each logger's IP (DHCP reservation, or learn it from the pushes). A receive task, a command parser (reusing the console commands) and replies with retries. Rules out turning the radio off between pushes |
| Commands carried in the ack | No open port and no IPs to track; only the collector can send commands. Little firmware: parse the ack | A command waits for the next push, up to one push interval |

Recommended: commands in the ack, unless replies within seconds are needed.

## SD card and hard faults

- **Mounting:** at boot, over SPI (VSPI: SCK 18, MISO 19, MOSI 23, CS 4, 4 MHz), at `/sdcard`. The logger **never formats** a card. Cards are formatted on a PC (FAT32).
- **Health check:** at every sample interval the logger asks the card for its status (`sdmmc_get_status`). That's read-only, so nothing is written. Once logging exists, each sample's write is the check.
- **Empty card:** at boot the logger notes whether the root holds anything except `System Volume Information` (Windows makes it when formatting). The clock uses this to recognise a remediated restart (see [Time](#time)).
- **Hard fault:** a missing card (`missing`), a card that doesn't mount (`mount_failed`) or a card that stops answering (`not_responding`) stops normal work. There is no carrying on without the card.
  - The console stays up, and `status` shows the fault.
  - The fault is printed and pushed to the listener at once, then every push interval, in place of the status line: `fault sd=<reason> seq=<n> up=<s> mac=<own MAC>`.
  - Only a reboot clears it. The first fault is kept; later ones are only logged.
- **Fault injection (test builds only):** `./build-test.sh` and `./flash-test.sh` build into `build-test/` with `AMBIENT_TEST_FAULTS`, which adds `test fault sd <reason>`. It raises the fault through the same path as a real one, for faults that can't be caused by hand. `info` says `TEST` in a test build. Production builds (`./build.sh`, `./flash.sh`) don't contain the command.

## Time

- **Chain:** the listener's ack time corrects the DS3231, the DS3231 syncs the ESP32 clock, and the ESP32 clock (UTC, microseconds) stamps each reading. Each reading will get its own stamp with milliseconds, taken when its measurement finishes. The BMP388's temperature and pressure come from one measurement, so they share a stamp.
- **ESP32 sync:** at boot the logger waits for the DS3231's next second edge (polling its seconds register) and sets the ESP32 clock to that second, to within about 1 ms. It resyncs the same way every hour, slewing with `adjtime` (ESP-IDF slews at 1/64 speed, so 50 ms takes about 3 s). An error over 1 s is stepped instead, with a warning.
- **Ack:** the listener sends milliseconds. The logger takes the ack time plus half the measured round trip as the true time; the difference from its own clock is the ack offset, shown in `status`. With `clock auto`, an offset over 50 ms makes the logger rewrite the DS3231 at a whole-second moment of true time, then resync the ESP32 from it. Writing the seconds register restarts the DS3231's 1 s countdown (datasheet). No correction is made while a slew is still running. With `clock report`, the offset is only shown.
- **Faults** (hard faults, as for the SD card, with `fault rtc=<reason>`):
  - `missing`: the DS3231 doesn't answer at boot.
  - `invalid`: its oscillator-stop flag is set (it lost all power, so its time can't be trusted) and the card has data on it.
  - `not_responding`: a read fails while running, including at the hourly resync.
  - Acks never clear a fault.
- **Remediation** for a failed clock: replace the board or battery, reformat the card on a PC, and restart. With the flag set and an empty card, the logger treats the restart as remediated: the clock is unset, `status` shows `clock  not set (waiting for ack)`, and the status line carries `clock=unset` and goes out every 10 s as a time request. The first ack sets the DS3231, then the flag is cleared and the ESP32 synced. Data from before a detected fault isn't used.
- **Test builds:** `test fault rtc <reason>` injects a fault. `test rtc-absent on|off` (saved, takes effect at boot) makes the logger look for the DS3231 at 0x69, where nothing answers, so a missing chip is detected for real. `test resync <seconds>` resyncs that often instead of hourly, until reboot.
- **First run (2026-11-06):** the DS3231 was found 80.3 s slow, left over from setting it with `rtc-probe` from the build time. The first ack corrected it; after that the ack offset stayed at about +1 to +8 ms (round trip 13 to 20 ms), under the 50 ms threshold. A steady +8 ms may be the DS3231 slightly behind dev10, or an asymmetric network delay; not investigated. With `test resync 15` the resync ran every 16 s (15 s plus the edge wait) and found the ESP32 within ±1 ms of the DS3231 each time.

## Sensors

- **Code:** ported from the probes that ran on this hardware. The thermistor (`thermistor.c`) comes from `ads1115-probe`, not the shared `ads1115_core` library. The SHT45 (`sht45.c`) and the BMP388 (`bmp388.c`) come from `sensor-probe`. The probes stay as diagnostics.
- **Sample:** every sample interval, while the clock is set and there is no hard fault. The order is the thermistor (about 0.42 s), the SHT45 straight after (so the two temperatures are close in time), then the BMP388. The first sample is one sample interval after boot. At boot GPIO25 is driven high (excitation off) before anything else.
- **Time stamps:** each reading gets its own UTC stamp with milliseconds, taken when its measurement finishes. A sensor's values share its stamp.
- **Thermistor:** Table 1. The ratio is A1 over the mean of the two A0 readings. The resistance is corrected for the ADC input (4.3 MΩ), then converted with Steinhart-Hart and the generic coefficients (WIRING.md). The probe's "A0 near 0 V when off" check is left out: it costs a 0.13 s conversion each sample and stays in `ads1115-probe`.
- **BMP388:** soft reset and calibration at the first reading, then one forced measurement per reading. After any failure the reset and calibration are redone, so a replaced chip comes back.
- **A failed sensor is a soft failure:** the others carry on, and it is tried again at the next sample. The first failure, a change of reason and the recovery are logged. Hard faults (SD, RTC) are unchanged.

**Table 4: Data types and failure reasons**

| Sensor | data_type | Failure reasons |
|---|---|---|
| thermistor | temp_c, ratio | nack, timeout, bus, excitation (A0 outside 3.1 to 3.5 V or moving more than 10 mV), open (ratio over 0.98), short (under 0.02) |
| sht45 | temp_c, rh_pct | nack, timeout, bus, crc, range (outside -40 to 125 °C, or humidity more than 5 % outside 0 to 100 %) |
| bmp388 | temp_c, pressure_pa | nack, timeout, bus, chip_id, reset, not_ready, range (outside -40 to 85 °C or 300 to 1250 hPa) |

The ratio is kept so a later calibration of the thermistor can be applied to past data.

- **Console:** `status` has a line per sensor with its last values and stamp, or its failure and how many readings in a row have failed. `read` takes one reading of all three now and prints it (refused while the clock is unset).
- **Tested (2026-11-06):** all three read, the thermistor about 0.1 °C below the SHT45. CAT5 #2 out: thermistor `open`, SHT45 `nack`, BMP388 still reading. CAT5 #1 out: thermistor and SHT45 `nack`. BMP388 pulled with the power off: `nack`, the others reading. Each recovered at the first reading after it was reconnected.

## Open items

- **Clock test, remediation:** set the oscillator-stop flag (DS3231 battery out with the board unpowered), restart with an empty card, and check the first ack sets the time and clears the flag. Skipped: needs the board out of the warm box; do it next time it's removed.
- **Clock test, invalid clock with data on the card:** should be a hard fault (`invalid`). Suspended until the logger writes to the card.
- **Clock drift:** the resync itself is tested (below). How far the ESP32 drifts in a real hour shows in the hourly `resynced from the DS3231` log line and in the ack offset; check it after a few days of normal running.
- **Clock test, `clock report`:** with an offset over 50 ms, check `report` leaves the DS3231 alone. Not yet run: after the first correction there was no offset that large to test with.
- **SD test, not-empty card:** with files on the card, `status` should say `not empty at boot`. Suspended until the logger writes to the card.
- **Thermistor noise test:** the noise test in WIRING.md (build step 3), with WiFi idle and with WiFi plus SD writes (`ads1115-probe` does both).
- **Partition table (separate project):** the app is 975 KB of a 1 MB partition, and the flash is 4 MB. Use a custom table with a single 3 MB app, which leaves room for OTA later, and keep the NVS offset so the settings survive.
- **Alerting (separate project):** a sensor failure or hard fault shows only on the console and in logserv's output. A way to be told (email, phone) is still to be chosen.
- **Keepalive:** if the firmware ever moves from UDP status lines to MQTT, set the keepalive longer than any quiet period. The broker drops a session after 1.5 times the keepalive with no traffic.
