# Ambient firmware

Decisions and open items for the `ambient` firmware. Wiring is in [WIRING.md](WIRING.md). The build, flash layout and memory use are in [BUILD.md](BUILD.md). The firmware samples the sensors every sample interval (see [Sensors](#sensors)), writes each reading and event to the SD card (see [SD logging](#sd-logging)), and sends each one to logserv, one message at a time until acked. It keeps time from the DS3231 (see [Time](#time)). How the whole system works, and the messages, are in [SYSTEM.md](SYSTEM.md).

The reading scheme (ratiometric, pulsed excitation on GPIO25, A0 for the excitation and A1 for the tap) is in [How the thermistor is read](WIRING.md#how-the-thermistor-is-read).

## Where the noise comes from

The firmware can't time conversions around WiFi activity: beacon wakes and delayed transmits happen on their own. What the noise is and how it was measured are in WIRING.md ("Why the ADS1115 box is separate" and Table 19a).

## Sampling

**Table 1: Thermistor sampling decisions (implemented in `thermistor.c`)**

| Setting | Choice | Why |
|---|---|---|
| Data rate | 8 to 32 SPS | Strong filtering; about 125 ms per conversion at 8 SPS |
| Mode | Single-shot | The firmware chooses when each conversion runs |
| Readings | One set per reading: A0, A1, A0 | The excitation is on for about 0.4 s at 8 SPS, which heats the thermistor about 0.006 °C. Sets back to back keep heating it (about 0.03 °C for five), so if more readings are ever wanted, space the sets about 15 s apart |
| Order | Energize, wait, convert A0, A1, A0, de-energize, publish | |
| Excitation | GPIO25 low (switch on) from 30 ms before the first conversion until the last conversion ends (`EXCITE_WAIT_MS`; at the 100 Hz tick rate that gives at least 20 ms) | The inputs climb to their final voltage through the filters: A1 takes about 7 ms at 25 °C and 11 ms at −20 °C to settle within one ADC step. A0 is driven directly by the switch and settles in microseconds. A conversion started at switch-on averages the climb in, so if A1 were converted first the error would be about +0.2 °C at 25 °C and +1.4 °C at −20 °C. With the A0, A1, A0 order A1 starts long after it has settled, so the wait is a margin that costs nothing and needs no bench check |

## WiFi power

**Table 2: WiFi power decisions**

| Option | Effect | Decision |
|---|---|---|
| Power-save (`WIFI_PS_MIN_MODEM`) | Stays associated and wakes at each DTIM beacon; the AP buffers packets; IP and DHCP lease survive | On (the ESP-IDF default), except during each message exchange (below). Needed for the solar/battery version |
| Power-save off for each exchange | `main.c` turns power save off before sending a message and back on after the ack or the 2 s timeout | In use. With power save on, the router holds the ack until the next beacon (up to about 100 ms), so the round trip is lopsided and the logserv time is up to about 50 ms wrong: enough to correct the DS3231 wrongly (seen 2026-11-08: −55 ms, then +51 ms). With it off the round trip was 11 ms |

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

- **Chain:** logserv time sets the DS3231, the DS3231 sets the ESP32 clock, and the ESP32 clock (UTC, microseconds) stamps each reading. Each reading gets its own stamp with milliseconds, taken when its measurement finishes. The BMP388's temperature and pressure come from one measurement, so they share a stamp.
- **Boot:** the logger waits for the DS3231's next second edge (polling its seconds register) and sets the ESP32 clock to that second, to within about 1 ms.
- **Sync to logserv:** on the first ack after boot, and on the first ack 24 h after the last sync. Each ack carries logserv's time in milliseconds. WiFi power save is off during the exchange, so the round trip is short and even (Table 2).
  1. True time when the ack arrived = logserv time + half the round trip.
  2. The logger catches the DS3231's next tick. The DS3231's time when the ack arrived = the tick's second − (ESP32 timer at the tick − ESP32 timer at the ack). Logserv minus DS3231 is the sync's diff, good to about ±6 ms (half the round trip, plus catching the tick).
  3. It waits until true time reaches the next whole second and writes the DS3231. Writing the seconds register restarts its 1 s countdown (datasheet).
  4. It sets the ESP32 clock from the DS3231, as at boot.
- **Sync record:** `status` shows the diff of the last 10 syncs with their dates, oldest first (`syncs` line). They're kept in NVS, so they survive a reboot. A reboot adds a sync, so the 10 aren't always a day apart.
- **Faults** (hard faults, as for the SD card, sent as the event `rtc,fault,<reason>`):
  - `missing`: the DS3231 doesn't answer at boot.
  - `invalid`: its oscillator-stop flag is set (it lost all power, so its time can't be trusted) and the card has data on it.
  - `not_responding`: a read fails while running, including at a sync.
  - Acks never clear a fault.
- **Remediation** for a failed clock: replace the board or battery, reformat the card on a PC, and restart. With the flag set and an empty card, the logger treats the restart as remediated: the clock is unset, `status` shows `clock  not set (waiting for ack)`, and the logger sends a `clock,unset` event, resent every 10 s until acked. That ack sets the DS3231, then the flag is cleared and the ESP32 synced. Sampling starts once the clock is set. Data from before a detected fault isn't used.

### Aging correction

`age [n]` shows or sets the DS3231's aging register. Negative values speed the clock up. The backup battery keeps the value, and a new value takes effect at the DS3231's next temperature conversion, within 64 s.

- **ppm rule:** any unitless ratio a/b can be given in ppm as a/b × 10⁶. a and b must be in the same unit, so the units cancel. Back the other way: ratio = ppm ÷ 10⁶.
- **Measured (2026-11-24):** a sync's diff is logserv minus DS3231, so a positive diff means the DS3231 is slow. The daily diffs at −18 were +41, +32, +28 ms: mean (41 + 32 + 28) ÷ 3 = +33.7 ms a day, slow. At −22 they are −9, −6, −6, −10, +0 ms: mean −31 ÷ 5 = −6.2 ms a day, fast. The change is 33.7 − (−6.2) = 39.9 ms a day: 0.0399 s ÷ 86 400 s × 10⁶ = 0.46 ppm, over 4 steps = 0.115 ppm a step (the datasheet gives about 0.1, typical at 25 °C).
- **Re-trim:** steps = mean daily diff in s ÷ 86 400 s × 10⁶ ÷ 0.115 ppm a step. A positive diff (slow) lowers `age` by that many steps; a negative one (fast) raises it. Now: −0.0062 s ÷ 86 400 s × 10⁶ = −0.072 ppm ÷ 0.115 = −0.6 steps, so −21 would leave about 0.115 − 0.072 = 0.043 ppm slow: 0.043 ÷ 10⁶ × 86 400 s = 3.7 ms a day. A gain of about 2 ms a day, below a sync's ±6 ms, so −22 stays.
- **Status:** −22 since 2026-11-18, an acceptable result for now. Not a pass or fail: the rate shifts with temperature and the crystal's aging, so the `syncs` diffs need watching continuously (see [Open items](#open-items)).

## Sensors

- **Code:** ported from the probes that ran on this hardware. The thermistor (`thermistor.c`) comes from `ads1115-probe`, not the shared `ads1115_core` library. The SHT45 (`sht45.c`) and the BMP388 (`bmp388.c`) come from `sensor-probe`. The probes stay as diagnostics.
- **Sample:** every sample interval, while the clock is set and there is no hard fault. The order is the thermistor (about 0.42 s), the SHT45 straight after (so the two temperatures are close in time), then the BMP388. The first sample is one sample interval after boot. At boot GPIO25 is driven high (excitation off) before anything else.
- **Thermistor:** Table 1. The ratio is A1 over the mean of the two A0 readings. The resistance is corrected for the ADC input (4.3 MΩ), then converted with Steinhart-Hart and the generic coefficients (WIRING.md). The probe's "A0 near 0 V when off" check is left out: it costs a 0.13 s conversion each sample and stays in `ads1115-probe`.
- **BMP388:** soft reset and calibration at the first reading, then one forced measurement per reading. After any failure the reset and calibration are redone, so a replaced chip comes back.
- **A failed sensor is a soft failure:** the others carry on, and it is tried again at the next sample. The first failure, a change of reason and the recovery are logged. Hard faults (SD, RTC) are unchanged.

**Table 3: Data types and failure reasons**

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
- **Events:** not written to the card; they're only sent (see "Sending from the card"). Format: `timestamp,source,event,detail`. An event from before the clock is set has an empty timestamp.

  | source | event | detail |
  |---|---|---|
  | logger | boot | (none) |
  | clock | synced | logserv minus DS3231 before the sync, ms; none when the DS3231 had no valid time |
  | clock | unset | waiting for ack |
  | thermistor, sht45, bmp388 | fail | reason; again when the reason changes |
  | thermistor, sht45, bmp388 | recover | after N failed |
  | supply | low / recovered | mV |
  | sd, rtc | fault | reason |

- **Failed readings:** a row for each of the sensor's data types with an empty value and the reason as the status (e.g. `nack`), so the gap is explicit in the data.
- **`read` on the console isn't logged:** it's a diagnostic, used in tests; only scheduled samples go to the card.
- **Value precision:** at least the sensor's own resolution, without digits that are pure noise.

  | data_type | Decimals | Why |
  |---|---|---|
  | temp_c | 2 | SHT45 resolution 0.01 °C |
  | rh_pct | 2 | SHT45 resolution 0.01 %RH |
  | ratio | 5 | One ADC step is about 0.00004; every step is kept for later recalibration |
  | pressure_pa | 1 | 0.1 Pa, about 1 cm of altitude |

## Sending from the card

Readings are sent to logserv from the card, one line at a time, each one acked before the next (`datalog.c`). No reading is lost while logserv or WiFi is down, or over a reboot: the backlog goes out when the link is back.

- **Events:** sent from a RAM queue of 32, ahead of readings, each resent until acked. They're lost if the board reboots before they're acked; one logged while the queue is full is dropped (console: `queue full; not sent`).
- **Order:** events first, then the readings files, oldest day first.
- **Position:** `/sdcard/sent.txt` holds the readings file being sent and the byte after its last acked line, e.g. `2026-11-08.csv 15134`. It's one fixed-width line, rewritten in place at most every 10 min, so it adds about 144 small writes a day.
- **Deleting a day file:** once a file is all sent and a newer day's file exists, `sent.txt` is moved to the next file and saved first, then the sent file is deleted (console: `deleted <file> (all sent)`). Today's file is never deleted. A read error is not taken as the end of a file, so it never causes a delete.
- **After a reboot:** sending restarts from `sent.txt`, so up to 10 min of lines go again. logserv ignores rows it already has (same timestamp, sensor and data_type) and prints them as `repeat`.
- **No `sent.txt`** (a new card): every day file on the card is sent.
- **Rate:** about 30 messages a second, so a day at 30 s (17,280 readings) takes about 10 min.
- **`status`** shows how far sending has got: `sent to    2026-11-09.csv byte 671791`.
- **Weak spots:** a power cut in the middle of a write can leave half a line, which the next write joins: that one row is refused by logserv (it's still acked, so sending carries on).

## Open items

- **DS3231 rate:** watch the daily diffs in `syncs` (`status`) continuously; re-trim `age` if their mean drifts outside about ±6 ms a day (see [Aging correction](#aging-correction)).
- **Remote commands (not decided):** the logger only sends and never listens; settings change only over the serial console. If remote control is wanted, carry commands in the ack: no open port and only the collector can send, but a command waits up to one sample interval. The other way, a UDP port on the logger, answers within about a beacon interval but is open to the whole LAN (needs a shared-secret check), needs the logger's IP, and rules out turning the radio off between messages.
