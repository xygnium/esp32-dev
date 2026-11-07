# Plan: SD logging (project 4)

Temporary. When this is implemented, its decisions move into [FIRMWARE.md](FIRMWARE.md) and this file is deleted. Builds on projects 1 to 3 (SD and hard faults, clocks, sensor sampling). Pushing the logged data from the card is project 5.

## Decided

- **Loss rule:** best effort at no loss, with power use and wear weighed against the impact of a loss and the value of the data. This replaces "nothing may be lost".
- **Write every sample:** append, then sync, after each sample. That's 720 writes a day at the 2-minute default. A power cut or crash loses at most the sample being taken. The logger runs from mains, so the power cost is negligible, and the card does its own wear levelling.
- **Readings:** one CSV per UTC day, in the long format: one row per reading, with the same columns as logserv's SQLite `readings` table, minus `logger_id`, which the server adds. About 250 KB a day, or 90 MB a year.
- **Card full:** hard fault, `sd=full`. The logger never deletes anything.
- **Early warning:** `sd low` when less than 10 % of the card is free.
- **Events:** faults, sensor failures and recoveries, and the other events below go to a file on the card. Project 5 sends them to the listener with the readings, acked and resent the same way.
- **Alerting** (email, phone) is a separate project and stays in Open items. It should cover `sd low`.
- **Failed readings:** when a sensor fails, write a row for each of its data types with an empty value and the failure reason as the status (e.g. `nack`). Why: the gap is explicit in the data. Without a row, a failed sensor would look the same as data lost on the way, and the SQLite `status` column exists to carry this.
- **`read` on the console isn't logged:** it's a diagnostic. Only scheduled samples go to the card. Why: `read` is used in tests (unplugging cables, checking sensors), so its readings would mix test data into the record and break the regular spacing of the samples.
- **Events before the clock is set** (boot, and the wait for the first ack after a remediated clock) are kept in RAM, up to 32 of them. They're written once the clock is set, with each stamp worked back from uptime. Why: every row needs a UTC stamp, but the boot event comes before the clock is read, and after a remediated clock there's no time until the first ack. Uptime is exact to the millisecond, so the worked-back stamp is as good as a live one. 32 is far more than a boot produces (a few), and costs about 3 KB of RAM.
- **Value precision:** keep at least the sensor's own resolution, so nothing is lost, and leave out digits that are pure noise. The extra digits cost a few bytes per row.

  | data_type | Decimals | Sensor limit |
  |---|---|---|
  | temp_c | 2 (0.01 °C) | SHT45 resolution 0.01 °C (accuracy ±0.1). The BMP388 and the thermistor resolve about 0.005 °C, but the thermistor's tolerance is ±0.5 °C |
  | rh_pct | 2 (0.01 %) | SHT45 resolution 0.01 %RH (accuracy ±1) |
  | ratio | 5 | One ADC step (125 µV over about 3.3 V on A0) is about 0.00004. Every step is kept, because the ratio exists for later recalibration |
  | pressure_pa | 1 (0.1 Pa, about 1 cm of altitude) | BMP388 resolution is well under 1 Pa. At the chip's default oversampling (not set by the firmware) the noise is a few Pa, from memory, not checked |

## Firmware (ambient)

1. **`datalog.c`:** a new module that owns the files on the card.
   - **Day file** `/sdcard/YYYY-MM-DD.csv`, named for the UTC day of the sample. A new file starts with a header line:
     ```
     timestamp,sensor,data_type,value,status
     2026-11-07T13:35:48.134Z,thermistor,temp_c,21.57,ok
     2026-11-07T13:35:48.134Z,thermistor,ratio,0.53724,ok
     2026-11-07T13:35:48.153Z,sht45,temp_c,21.74,ok
     2026-11-07T13:35:48.153Z,sht45,rh_pct,54.00,ok
     2026-11-07T13:35:48.184Z,bmp388,temp_c,23.56,ok
     2026-11-07T13:35:48.184Z,bmp388,pressure_pa,100733.0,ok
     ```
   - **Per sample:** open for append, write the sample's 6 rows (one buffer), `fsync`, close.
   - **Events file** `/sdcard/events.csv`, one file kept for good, written the same way:
     ```
     timestamp,source,event,detail
     2026-11-07T13:35:48.153Z,sht45,fail,nack
     2026-11-07T13:37:48.150Z,sht45,recover,after 1
     ```
2. **Events:**

   | source | event | detail |
   |---|---|---|
   | logger | boot | firmware version, reset reason |
   | clock | set | from DS3231 |
   | clock | stepped | offset in ms |
   | clock | corrected | offset in ms (from the ack) |
   | clock | set_from_ack | the stop flag was cleared |
   | thermistor, sht45, bmp388 | fail | reason; logged again when the reason changes |
   | thermistor, sht45, bmp388 | recover | after N failed |
   | supply | low / recovered | mV |
   | sd | low | MB free |
   | rtc | fault | reason |

   An SD fault can't be written to the card, so it still goes only to the console and the listener's fault line.
3. **Write errors:** `ENOSPC` raises `sd=full`. Any other failure to open, write or sync raises `sd=write_failed`. Both are hard faults through the existing path.
4. **Health check:** a sample's write is now the card check. `sd_check()` (the read-only status query) runs only when no sample was written in that interval, i.e. while the clock is unset.
5. **Free space:** checked at boot, at each new day file, and hourly. Below 10 % free, the `sd low` event is logged once, and `status` shows `low`. The low state clears only if space goes back above 10 % (e.g. a new card after a reboot).
6. **`status`:** the `sd` line adds today's file and its size, rows written since boot, and `low` when it applies.
7. **Empty card:** files written by the logger count as data, so the existing "empty at boot" check and the clock's `invalid` fault now see them.
8. **Test build only:** `test dayfile minute|day`, kept until reboot. With `minute`, files are per UTC minute (`YYYY-MM-DDTHHMM.csv`), to test the change of file without waiting for midnight.
9. Build.

## Server (logserv)

No change in this project.

## Docs

1. `FIRMWARE.md`: an "SD logging" section with the decisions above, the new SD faults (`full`, `write_failed`), and the loss rule. Update Open items: the not-empty test and the invalid-clock test become runnable, and alerting should cover `sd low`.
2. `main.c` header comment.

## Test (on the board)

Before each test, check the port and `status` first.

1. **Normal run:** after a few samples, take the card to a PC. The day file has the header and 6 rows per sample with the stamps from `status`, and `events.csv` has the boot and clock-set events.
2. **Failed sensor:** unplug CAT5 #2 for two samples. The day file has rows with empty values and the failure reason for the thermistor and SHT45. `events.csv` has `fail` and `recover`.
3. **Day change:** test build, `test dayfile minute`, 15 s sample interval. A new file starts each minute, with no rows lost or duplicated across the change.
4. **Power cut:** pull the power at a sample time, several times. Each file is readable on the PC, and at most the last sample is missing or cut short.
5. **Not-empty card:** reboot with logged files on the card. `status` says `not empty at boot` (the suspended SD test).
6. **Invalid clock with data:** set the real oscillator-stop flag (DS3231 battery out with the board unpowered) and boot with data on the card. It's a hard fault, `invalid` (the suspended clock test). `test fault rtc invalid` would skip the boot decision being tested. This needs the board out of the warm box.
7. **`sd low`:** fill the card on a PC with a dummy file until less than 10 % is free. It's flagged at boot, logged once in `events.csv`, and shows in `status`.
8. **`sd=full`:** fill the card on a PC until about 100 KB is free, then run at the 15 s interval (about 1.2 KB a minute) until it fills. It raises the `full` hard fault, and the last written rows are intact.
9. **Card pulled while running:** `sd=write_failed` (or `not_responding`) at the next sample.
10. **Long run:** several days at the 120 s default, with the file sizes as expected (about 250 KB a day).

Then commit, and the user pushes.
