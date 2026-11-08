# How the system works

One ESP32 logger reads three sensors every 2 minutes, keeps everything on its SD card, and sends each reading and event to logserv on dev10, which stores them in SQLite.

```
 Sensors ─────────────►  Logger (ESP32)  ── data / event ──►  logserv (dev10)
 thermistor, SHT45,      samples every    ◄── ack + time ───  udp_listener.py
 BMP388                  2 min                                     │ --db
                               │ written first                     ▼
 DS3231 clock ──────────►      ▼                                 SQLite
 sets ESP32 time hourly     SD card                        readings + events
                         day CSVs + events.csv
```

## Logger (ESP32)

- **Sensors:** thermistor (via the ADS1115), SHT45 (temperature, humidity), BMP388 (temperature, pressure). Each reading gets its own UTC timestamp in milliseconds.
- **Readings file:** one CSV per UTC day, `YYYY-MM-DD.csv`, one row per reading: `timestamp,sensor,data_type,value,status`.
- **Events file:** `events.csv`, one row per event: `timestamp,source,event,detail`. Events are boot, clock set or corrected, sensor fail or recover, supply low or recovered, and faults.
- **Clock:** the DS3231 sets the ESP32 clock at boot and every hour. Each ack from logserv carries its time; if the logger is off by more than 50 ms, it corrects the DS3231.
- **Console:** WiFi, logserv's address and the intervals are set over the USB serial console. `status` shows the current state.

## Messages

The logger sends one message at a time over UDP and waits for its ack before sending the next.

| Message | Carries |
| --- | --- |
| `data <timestamp,sensor,data_type,value,status>` | One reading |
| `event <timestamp,source,event,detail>` | One event |
| `ack <UTC time>` (logserv's reply) | Acceptance, plus the time for the clock |

- **No ack within 2 s:** the same message is sent again every 10 s, forever, until it's acked.
- **Order:** events go ahead of readings.
- **While waiting:** sampling and card writes carry on. Unsent messages queue in RAM: 200 readings (about 1 hour) and 32 events. A full queue drops new messages, which stay on the card.

## logserv (dev10)

`udp_listener.py` in the logger-server repo listens on UDP 8080.

- **Prints** every message with its local arrival time.
- **Stores** it with `--db <file>`: readings in the `readings` table, events in the `events` table.
- **Acks** every message with `ack <UTC time, ms>`.
- **Repeats:** a message identical to the one before (resent because an ack was lost) isn't stored again; logserv records its own event instead, `logserv,repeat,<message>`.

## Faults

| Fault | What the logger does |
| --- | --- |
| Sensor fails | Rows with an empty value and the reason as status, plus a `fail` event; the others carry on; tried again next sample; `recover` event when it works |
| SD card missing, not mounting, failing, full | Stops sampling; sends `sd,fault,<reason>` (not written: the card is the problem); only a reboot clears it |
| DS3231 missing, failing, or lost power with data on the card | Stops sampling; sends `rtc,fault,<reason>`; only a reboot clears it |
| DS3231 lost power, card empty (after a fix) | Clock unset: sends `clock,unset`; the first ack sets the time and sampling starts |

An event from before the clock is set has an empty timestamp.

## Not done yet

- The sending above is built but not flashed, tested or committed.
- Messages queued in RAM are lost on reboot or when the queue fills; they stay only on the card.
- No alerting (email, phone) when something fails.
- Leftovers: the push interval setting is unused, the console still says "push", and `FIRMWARE.md` and `PROTOCOL.md` describe the old messages.
