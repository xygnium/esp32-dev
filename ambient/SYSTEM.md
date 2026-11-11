# How the system works

One ESP32 logger reads three sensors every 2 minutes, keeps its readings on its SD card until they're sent, and sends each reading and event to logserv on dev10, which stores them in SQLite.

```
 Sensors ─────────────►  Logger (ESP32)  ── data / event ──►  logserv (dev10)
 thermistor, SHT45,      samples every    ◄── ack + time ───  udp_listener.py
 BMP388                  2 min                                     │ --db
                               │ written first                     ▼
 DS3231 clock ──────────►      ▼                                 SQLite
 sets ESP32 time hourly     SD card                        readings + events
                         day CSVs (deleted once sent)
```

## Logger (ESP32)

- **Sensors:** thermistor (via the ADS1115), SHT45 (temperature, humidity), BMP388 (temperature, pressure). Each reading gets its own UTC timestamp in milliseconds.
- **Readings file:** one CSV per UTC day, `YYYY-MM-DD.csv`, one row per reading: `timestamp,sensor,data_type,value,status`. A past day's file is deleted once all of it is sent.
- **Events:** `timestamp,source,event,detail`, kept in RAM until sent, not on the card. Events are boot, clock set or corrected, sensor fail or recover, supply low or recovered, and faults.
- **Clock:** the DS3231 sets the ESP32 clock at boot and every hour. Every ack carries logserv's time; on message 6 of 6 of every Nth sample the logger compares it with its own, and if it's off by more than 50 ms, it corrects the DS3231.
- **Console:** WiFi, logserv's address and the sample interval are set over the USB serial console. `status` shows the current state.
- **WiFi power save** is on, except while a message waits for its ack, so logserv time is good for the clock. Details: [FIRMWARE.md](FIRMWARE.md).

## Why one row per value

Each value is its own row, so a sample of 3 sensors with 2 values each is 6 rows (and 6 messages). The same rows go on the card and into SQLite's `readings` table.

```
2026-11-07T13:35:48.134Z,thermistor,temp_c,21.57,ok
2026-11-07T13:35:48.134Z,thermistor,ratio,0.53724,ok
2026-11-07T13:35:48.153Z,sht45,temp_c,21.74,ok
2026-11-07T13:35:48.153Z,sht45,rh_pct,54.00,ok
2026-11-07T13:35:48.184Z,bmp388,temp_c,23.56,ok
2026-11-07T13:35:48.184Z,bmp388,pressure_pa,100733.0,ok
```

| Layout | Rows per sample | For | Against |
| --- | --- | --- | --- |
| **One row per value** (chosen) | 6 | Every row reads on its own; each value has its own timestamp and status; adding a sensor or value adds rows, never columns; one query shape for everything (`WHERE data_type = ... AND timestamp BETWEEN ...`) | Most rows and messages; the timestamp and sensor name repeat |
| One row per sensor: `timestamp,sensor,value1,value2,status` | 3 | Half the rows | `value1`/`value2` mean different things per sensor, so you must know which is which; a sensor with 3 values breaks the layout |
| One row per sample: `timestamp,therm_c,ratio,sht_c,rh,bmp_c,pa,...` | 1 | Fewest rows; easy to open in a spreadsheet | The 3 sensors have different timestamps, so one timestamp is wrong for two of them; needs a status column per sensor; adding a sensor changes the table |

The cost of the chosen layout is size: about 250 KB a day on the card and 100 to 150 MB a year in SQLite (estimated), which is small for both.

## Messages

The logger sends one message at a time over UDP and waits for its ack before sending the next.

| Message | Carries |
| --- | --- |
| `data <timestamp,sensor,data_type,value,status>` | One reading |
| `event <timestamp,source,event,detail>` | One event |
| `ack <UTC time> <CRC-16>` (logserv's reply) | Proof the message arrived intact, plus the time for the clock |

- **Ack:** `ack <UTC time> <CRC-16 of the message>` (CRC-16/XMODEM, 4 hex digits); a reply that doesn't match doesn't count.
- **No ack within 2 s:** the same message is sent again every 10 s, forever, until it's acked.
- **Order:** events go ahead of readings.
- **While waiting:** sampling and card writes carry on. Readings are sent from the card, so a backlog of any length goes out when logserv is back; `sent.txt` on the card keeps the position (FIRMWARE.md, "Sending from the card"). Events wait in a RAM queue of 32 and are lost on a reboot.

## logserv (dev10)

`udp_listener.py` in the logger-server repo listens on UDP 8080.

- **Prints** every message with its local arrival time.
- **Stores** it with `--db <file>`: readings in the `readings` table, events in the `events` table.
- **Acks** every message with `ack <UTC time, ms> <CRC-16 of the message>`.
- **Repeats:** a row already stored (resent because an ack was lost, or after a logger reboot) isn't stored again; logserv prints it as `repeat <message>`.

## Faults

| Fault | What the logger does |
| --- | --- |
| Sensor fails | Rows with an empty value and the reason as status, plus a `fail` event; the others carry on; tried again next sample; `recover` event when it works |
| SD card missing, not mounting, failing, full | Stops sampling; sends `sd,fault,<reason>`; only a reboot clears it |
| DS3231 missing, failing, or lost power with data on the card | Stops sampling; sends `rtc,fault,<reason>`; only a reboot clears it |
| DS3231 lost power, card empty (after a fix) | Clock unset: sends `clock,unset`; the first ack sets the time and sampling starts |

An event from before the clock is set has an empty timestamp.
