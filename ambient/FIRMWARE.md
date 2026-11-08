# Ambient firmware

Decisions and open items for the `ambient` firmware. Wiring is in [WIRING.md](WIRING.md). The build, flash layout and memory use are in [BUILD.md](BUILD.md). The firmware samples the sensors every sample interval (see [Sensors](#sensors)), writes each reading and event to the SD card (see [SD logging](#sd-logging)), and sends each one to logserv, one message at a time until acked. It keeps time from the DS3231 (see [Time](#time)). How the whole system works, and the messages, are in [SYSTEM.md](SYSTEM.md).

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

**Table 2: WiFi power decisions**

| Option | Effect | Decision |
|---|---|---|
| Power-save (`WIFI_PS_MIN_MODEM`) | Stays associated and wakes at each DTIM beacon; the AP buffers packets; IP and DHCP lease survive | On (the ESP-IDF default), except during each message exchange (below). Needed for the solar/battery version |
| Power-save off for each exchange | `main.c` turns power save off before sending a message and back on after the ack or the 2 s timeout | In use. With power save on, the router holds the ack until the next beacon (up to about 100 ms), so the round trip is lopsided and the ack's time is up to about 50 ms wrong: enough to correct the DS3231 wrongly (seen 2026-11-08: −55 ms, then +51 ms). With it off the round trip was 11 ms |
| Radio off (`esp_wifi_stop()`) | Sockets die; reconnecting takes about 1 to 3 s with DHCP (a static IP is faster) | Only for sample intervals of minutes or more; not needed for EMI |

## Remote commands

The logger only sends: it sends to the collector and waits for the ack on the same socket, and never listens on a port of its own. Settings change only over the serial console. If remote control is wanted, there are two ways to add it.

**Table 3: Remote command options (proposed, not decided)**

| Option | Gains | Costs |
|---|---|---|
| Logger listens on a UDP port | Commands and on-demand readings at any time, within about a beacon interval | An open port: anyone on the LAN can send, and UDP senders are easy to fake, so settings changes need a shared-secret check. The collector must know the logger's IP (DHCP reservation, or learn it from the messages). A receive task, a command parser (reusing the console commands) and replies with retries. Rules out turning the radio off between messages |
| Commands carried in the ack | No open port and no IPs to track; only the collector can send commands. Little firmware: parse the ack | A command waits for the next message, up to one sample interval |

Recommended: commands in the ack, unless replies within seconds are needed.

## SD card and hard faults

- **Mounting:** at boot, over SPI (VSPI: SCK 18, MISO 19, MOSI 23, CS 4, 4 MHz), at `/sdcard`. The logger **never formats** a card. Cards are formatted on a PC (FAT32), see [Formatting a card](#formatting-a-card).
- <a id="formatting-a-card"></a>**Formatting a card** on Linux. Find the card by its size first: the wrong device wipes a disk. A reader with no card shows 0 B. Below the card is `sdc`.
  ```
  lsblk
  sudo umount /dev/sdc1
  sudo parted /dev/sdc --script -- mklabel msdos mkpart primary fat32 1MiB 100%
  sudo mkfs.vfat -F32 -n AMBIENT /dev/sdc1
  ```
  Ask for FAT32 (`-F32`): a format tool may pick FAT16 for a small card, and its root holds only 512 entries, about 250 day files.
- **Health check:** each sample's write is the check. When no sample is written (clock unset, or a fault), the logger asks the card for its status at every sample interval instead (`sdmmc_get_status`, read-only).
- **Empty card:** at boot the logger notes whether the root holds anything except `System Volume Information` (Windows makes it when formatting). The clock uses this to recognise a remediated restart (see [Time](#time)).
- **Hard fault:** a missing card (`missing`), a card that doesn't mount (`mount_failed`), a card that stops answering (`not_responding`) or a failed write (`write_failed`) stops sampling. There is no carrying on without the card.
  - The console stays up, and `status` shows the fault.
  - The fault is printed and sent to logserv as an event, `sd,fault,<reason>`. It isn't written to the card, since the card is the problem.
  - Only a reboot clears it. The first fault is kept; later ones are only logged.

## Time

- **Chain:** the listener's ack time corrects the DS3231, the DS3231 syncs the ESP32 clock, and the ESP32 clock (UTC, microseconds) stamps each reading. Each reading gets its own stamp with milliseconds, taken when its measurement finishes. The BMP388's temperature and pressure come from one measurement, so they share a stamp.
- **ESP32 sync:** at boot the logger waits for the DS3231's next second edge (polling its seconds register) and sets the ESP32 clock to that second, to within about 1 ms. It resyncs the same way every hour, slewing with `adjtime` (ESP-IDF slews at 1/64 speed, so 50 ms takes about 3 s). An error over 1 s is stepped instead, with a warning.
- **Ack:** the listener sends milliseconds. The logger takes the ack time plus half the measured round trip as the true time; the difference from its own clock is the ack offset, shown in `status`. WiFi power save is off during the exchange, so the round trip is short and even (Table 2). With `clock auto`, an offset over 50 ms makes the logger rewrite the DS3231 at a whole-second moment of true time, then resync the ESP32 from it. Writing the seconds register restarts the DS3231's 1 s countdown (datasheet). No correction is made while a slew is still running. With `clock report`, the offset is only shown.
- **Faults** (hard faults, as for the SD card, sent as the event `rtc,fault,<reason>`):
  - `missing`: the DS3231 doesn't answer at boot.
  - `invalid`: its oscillator-stop flag is set (it lost all power, so its time can't be trusted) and the card has data on it.
  - `not_responding`: a read fails while running, including at the hourly resync.
  - Acks never clear a fault.
- **Remediation** for a failed clock: replace the board or battery, reformat the card on a PC, and restart. With the flag set and an empty card, the logger treats the restart as remediated: the clock is unset, `status` shows `clock  not set (waiting for ack)`, and the logger sends a `clock,unset` event, resent every 10 s until acked. That ack sets the DS3231, then the flag is cleared and the ESP32 synced. Sampling starts once the clock is set. Data from before a detected fault isn't used.

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

## SD logging

- **Write every sample:** open for append, write the sample's rows, `fsync`, close. A power cut or crash loses at most the sample being taken. The card does its own wear levelling; a worn card shows up only as a failed write (`sd=write_failed`).
- **Readings:** one CSV per UTC day, `/sdcard/YYYY-MM-DD.csv`, one row per value: `timestamp,sensor,data_type,value,status`. A new file starts with that header. Why one row per value: [SYSTEM.md](SYSTEM.md#why-one-row-per-value).
- **Events:** `/sdcard/events.csv`, kept for good: `timestamp,source,event,detail`. An event from before the clock is set has an empty timestamp.

  | source | event | detail |
  |---|---|---|
  | logger | boot | firmware version |
  | clock | set | from DS3231 |
  | clock | stepped / corrected | offset in ms |
  | clock | set_from_ack | stop flag cleared |
  | clock | unset | waiting for ack |
  | thermistor, sht45, bmp388 | fail | reason; again when the reason changes |
  | thermistor, sht45, bmp388 | recover | after N failed |
  | supply | low / recovered | mV |
  | sd, rtc | fault | reason (sd faults are sent only, not written) |

- **Failed readings:** a row for each of the sensor's data types with an empty value and the reason as the status (e.g. `nack`), so the gap is explicit in the data.
- **`read` on the console isn't logged:** it's a diagnostic, used in tests; only scheduled samples go to the card.
- **Value precision:** at least the sensor's own resolution, without digits that are pure noise.

  | data_type | Decimals | Why |
  |---|---|---|
  | temp_c | 2 | SHT45 resolution 0.01 °C |
  | rh_pct | 2 | SHT45 resolution 0.01 %RH |
  | ratio | 5 | One ADC step is about 0.00004; every step is kept for later recalibration |
  | pressure_pa | 1 | 0.1 Pa, about 1 cm of altitude |

- **Never deletes or formats** anything on the card.

## Open items

- **Clock test, remediation:** set the oscillator-stop flag (DS3231 battery out with the board unpowered), restart with an empty card, and check the first ack sets the time and clears the flag. Skipped: needs the board out of the warm box; do it next time it's removed.
- **Clock test, invalid clock with data on the card:** should be a hard fault (`invalid`). Needs the DS3231 battery out with the board unpowered, so the board out of the warm box.
- **Clock drift (measured 2026-11-08):** the ESP32 drifts about 40 ms an hour against the DS3231 (the hourly `resynced from the DS3231` line), so reading stamps can be up to about 40 ms off just before a resync. The DS3231 lost about 46 ms in 4.5 h against logserv (about 2.8 ppm; the DS3231SN is rated ±2 ppm, the DS3231M ±5 ppm), so the ack corrects it by about +50 ms every 4 to 5 hours. That's within the design (±50 ms). Options for tighter time, for later:
  1. **Resync the ESP32 every 10 min** instead of hourly (`RESYNC_US` in `clock.c`): stamps within about 7 ms of the DS3231. The ESP32's drift is the larger error.
  2. **Lower the correction limit** from 50 ms to about 20 ms (`CLOCK_CORRECT_MS`): safe now that acks are good to about ±6 ms (12 ms round trip).
  3. **Trim the DS3231's aging register** (about 0.1 ppm a step; around −28 for this chip, to be measured over a day or two): fewer corrections, and better time when logserv or WiFi is down for long. It fixes only the average rate; the ack corrections are still needed.
- **Clock test, `clock report`:** with an offset over 50 ms, check `report` leaves the DS3231 alone. Not yet run.
- **SD test, not-empty card:** with files on the card, `status` should say `not empty at boot`. Runnable now; the 2026-11-08 `status` showed it, not yet declared passed.
- **Card full and low:** not built. A full card shows as `sd=write_failed`; there is no `sd low` warning.
- **Thermistor noise test:** the noise test in WIRING.md (build step 3), with WiFi idle and with WiFi plus SD writes (`ads1115-probe` does both).
- **Alerting (separate project):** a sensor failure or hard fault shows only on the console, on the card and in logserv's database. A way to be told (email, phone) is still to be chosen.
