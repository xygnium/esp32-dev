# ESP32 SHT humidity/temperature: sensor comparison rig + outdoor ambient node

## Context

Two goals:
1. **Compare** SHT45 and SHT31 breakouts from different suppliers side by side, to confirm the production part and supplier.
2. **Production node (ambient logger)**: one ESP32 + one SHT45 (PTFE membrane) + one BMP388, outdoors in a covered spot with no direct sun, on a 5 V wall wart. It is a standalone logger built like the attic `temp-sense` Pico: it samples outside temperature, RH and barometric pressure on its own timer, stamps each reading from a DS3231 real-time clock, and stores it on an SD card. The collector (`~/dev/github/pico-dev/temp-sense/collector.py`) pulls the backlog over WiFi the same way it pulls from the Pico.

**Why a logger and not an on-demand sensor (changed 2026-10-02):** the earlier design took a reading only when the collector asked, stamped with the collector's time. Any WiFi or dev10 outage meant lost readings, and the timestamps weren't the sensor's own. Comparing attic to outside needs both sides sampling on their own clocks and keeping a backlog.

Development goes in **stages, each introducing exactly one new unknown**. A wiring change is tested with code that already works, and a code change is tested on wiring that already works. When a stage fails, the cause is whatever that stage changed.

## Decisions

- **Hardware:**
  - Comparison rig: 2× Adafruit SHT45 (one with PTFE), 1× generic SHT45, 3× generic SHT31.
  - Ambient logger: 1× generic BMP388 board (pressure), DS3231 clock board, SD card board, AMS1117 3.3 V module, breather vent (IP68), plus the SHT45-PTFE chosen by the comparison.
  - ESP32: Elegoo ESP-WROOM-32 dev boards, three of them, labelled by paint dots (SETUP-PLAN.md, "The three boards"): 1 = bench, 2 = ambient logger perf board, 3 = spare.
- **Addresses:** all SHT45s are fixed at 0x44; SHT31s are 0x44 or 0x45 via the ADR pin. The ESP32 has 2 hardware I2C buses, so without a mux the maximum is 2 SHT45 + 2 SHT31 at once.
- **Mux:** a TCA9548A is recommended, but nothing waits for it. The firmware sensor table holds `{label, model, supplier, ptfe, bus, mux_ch|none, addr}`, so batch and mux layouts use the same code.
  - Batch mode: the non-PTFE Adafruit SHT45 is the reference and stays in every batch.
  - The PTFE board is compared on steady-state periods only.
- **Integration: the logger pushes (decided 2026-10-02).** A sleeping logger can't wait to be asked, so the ambient logger starts every transfer: it wakes on its own schedule, connects, sends every record since its last confirmed one, waits for the acknowledgement, and goes back to sleep. The packets and acknowledgements stay close to temp-sense's (`temp-sense/temp-logger-udp-protocol.md`, data / acknowledge, resume from the logger's own watermark); only who starts the exchange changes. Still exactly one collector.
  - **Collector:** a listener on dev10 (the DC-powered Dell desktop), up whenever dev10 is, usually about 06:00–18:00. Its uptime doesn't matter: when it's down, records build up on the SD card and go out with the next successful push. No overnight collection needed. Nothing new runs 24/7.
  - **Push interval:** default every 60 minutes inside the push window (`config push`); set to 1–5 minutes during development for a quick feedback loop.
  - **Push window:** set by command, e.g. 06:00–18:00 by the logger's clock. No push attempts outside it, so the logger doesn't spend power on WiFi all night.
  - **Back-off:** after a failed push, wait longer before the next try (covers a day when dev10 is off).
  - **Clock check:** the acknowledgement carries dev10's UTC time. The logger records its clock's offset (and may correct it), which settles setting the clocks from one source.
  - The attic Pico keeps being pulled, unchanged, until a separate decision to move it to push (stage 26 is the natural point).
  - **Addresses:** the router can't reserve addresses by MAC. The logger's own address doesn't matter (nothing contacts it), so it takes whatever the router gives. The listener's address must stay put: dev10 either gets a fixed address in its own network settings, outside the router's pool, or asks the router for a particular address the way the Pico does (the router then knows it's taken; but dev10 is off overnight, so the address could be lent to something else, and dev10 would need to retry like the Pico). Which DHCP client dev10 uses isn't known here. It goes in `wifi_secrets.h` as `LISTENER_HOST`. (The attic Pico, which is pulled, asks the router for .120 and retries until it gets it.) Fallbacks if that proves awkward: find dev10 by name (mDNS) or broadcast to 192.168.1.255.
- **Clock:** DS3231 on I2C bus 0, kept in UTC, same convention as temp-sense. Coin cell keeps time across power outages.
- **Storage:** SD card on SPI, holding a ring of records that survives reboots, like temp-sense's `sd_ring`.
- **Pressure:** BMP388 on I2C bus 0. Compensation math from Bosch's BMP3 sensor API rather than our own.
- **Code reuse: copy first, share later.** The temp-sense storage and transfer code (`sd_ring`, `xfer_proto`, `xfer_session`, `config_store`, `crc32`, DS3231 driver) is copied into `ambient/` and adapted until the ESP32 logs and pushes to the dev10 listener. The packet format copies over; the session logic (`xfer_session`) is reversed, since the logger starts the transfer. Only after both loggers work is the common part extracted into a shared library repo (core + per-board ports, like ads1115-dev). The extraction is a separate step, never bundled with new work, and the Pico is re-checked on hardware afterward. Reason: the ambient record layout differs (finer temperature steps, pressure), and the real boundary between common and board-specific code is only visible once two working copies exist. Cost: fixes must be made in both copies by hand until then.
- **Outdoor:** three parts: wall charger, warm box, open box (see Wiring plan, Production). The SHT45 sits in its own open box on a cable (≤ ~1 m) away from the warm box's heat; the open box must let outside air move freely past the membrane. The SHT45 heater runs when RH stays above ~95%, and readings are `valid=0` while it recovers.
- **Power: battery-backed solar is a goal (2026-10-02).** The firmware sleeps between samples: wake, sample, write to SD, sleep; every Nth wake (inside the push window) it also pushes over WiFi. Built always-awake first, then sleep is added as its own stage (22b). The wall charger stays the supply until the solar setup is built (see Solar power).

## Wiring plan

### Rules for every stage
- **Power every sensor board from 3.3 V, never 5V/VIN.** Breakout boards pull SDA/SCL up to their own supply, and the ESP32's GPIOs are not 5 V tolerant.
  - Comparison rig: the ESP32's 3V3 pin.
  - Production logger: a separate AMS1117 3.3 V module (the same one used on the Pico loggers; sold as a "buck module" but it is a linear regulator, which is quieter). It powers the SHT45, BMP388, DS3231 and SD card boards, and keeps the ESP32's WiFi current bursts and the SD card's write bursts off the sensors' rail. **Power enters only through the ESP32's USB socket (decided 2026-10-04, replacing the barrel-jack design of 2026-10-03).** One power entry means two supplies can never meet, which matters because **board 2 has no USB-to-VIN diode**: VIN reads 5.06–5.09 V on USB alone (two meters), and a part near the USB socket marked "0" looks like a zero-ohm link where a diode would go. The AMS1117 module's input comes from the ESP32's VIN pin, which on this board is the full USB 5 V. Whatever feeds the USB socket powers both rails, so they switch on and off together (a live sensor rail with the ESP32 off would feed current into the ESP32's pins through the pull-ups). Ground shared with the ESP32. A 220 µF 25 V electrolytic capacitor across VIN and GND on the perf board (striped leg to the ground rail near pin 14, other leg on the VIN wire near pin 15, short leads) covers WiFi current bursts. Larger wasn't chosen: the bursts last milliseconds, and a bigger capacitor makes a bigger current surge at plug-in, which a laptop port may react to; drop to 100 µF if one does.
  - **Box entry:** a USB-C pass-through in the warm box wall (pigtail: female outside, short male lead inside to the ESP32; or coupler: female both sides, plus a short data-capable C-to-C cable inside). It must carry data (USB 2.0: VBUS, GND, D+, D−), not power only. The ones seen so far don't pass the CC lines, so **only USB-A sources** work through it (USB-A charger, laptop USB-A port, solar 12 V-to-USB adapter with USB-A outputs, each with an A-to-C cable); a USB-C charger or USB-C laptop port with a C-to-C cable won't turn on 5 V without CC. A pass-through that passes CC ("supports PD") removes that limit. Fit it on the bottom or side, with a cap; check hole size and thread length against the box wall.
  - **Flashing and console: swap, don't combine.** Unplug the charger's cable from the pass-through, plug in the laptop's; the laptop powers the logger while flashing or reading `log`; swap back. The logger restarts, but the SD ring and clock carry over. A USB power blocker (or Kapton tape over the 5 V pin of a USB-A plug) is optional, only for watching the console while the logger runs on its own charger.
  - **Current measurement:** a USB inline power meter between the source and the pass-through, instead of a jumper break point.
  - Board 3 powered up from a USB-C charger with a C-to-C cable straight into its socket, so it likely has the CC resistors; board 2 untested.
  - Not chosen: a barrel jack feeding VIN (two supplies could meet through the missing diode; a data-only adapter would be needed for every flash), and our own diode in an external lead (~0.3 V headroom lost).
Opening the console resets the ESP32 (the serial chip's reset lines), as on the bench; the SD ring and clock carry over. Not chosen either: feeding the module's 3.3 V into the ESP32's 3V3 pin. With USB plugged in, it would run in parallel with the board's own regulator, and it would put the WiFi bursts back on the sensors' rail. If the SD board has its own regulator, feed it 5 V instead.
- All GNDs are common.
- I2C clock is 100 kHz by default. It drops to 50 kHz for the 1 m outdoor cable if the error count says so.
- Check each generic board's silkscreen pin order before wiring. Generic boards don't all follow the same order.

### ESP32 pin assignment

| Function | GPIO | Board label | Notes |
|---|---|---|---|
| I2C bus 0 SDA | 21 | D21 | ESP-IDF default. Used by the rig and by production |
| I2C bus 0 SCL | 22 | D22 | |
| I2C bus 1 SDA | 32 | D32 | Rig batch mode only. Not a strapping or flash pin |
| I2C bus 1 SCL | 33 | D33 | |
| SD card SCK | 18 | D18 | Production only. ESP32 VSPI default |
| SD card MISO | 19 | D19 | |
| SD card MOSI | 23 | D23 | |
| SD card CS | 4 | D4 | Not the VSPI default (GPIO 5 is a strapping pin, avoided) |
| Supply voltage sense | 34 | D34 | Production only. Input-only analog pin (ADC1), via a resistor divider (2 × 100 kΩ) from VIN |
| Sensor power | — | 3V3 | |
| Ground | — | GND | |

Avoided: GPIO 0/2/5/12/15 (strapping), 6–11 (flash), 34–39 (input only).

**Physical positions on the 30-pin board** (component side up, antenna at top, USB at bottom; standard DevKit-V1 30-pin order, so check the silkscreen before wiring):
```
            LEFT                     RIGHT
  1  EN                       1  D23   ← SD MOSI
  2  VP (36)                  2  D22   ← bus 0 SCL
  3  VN (39)                  3  TX0
  4  D34   ← supply V sense    4  RX0
  5  D35                      5  D21   ← bus 0 SDA
  6  D32   ← bus 1 SDA        6  D19   ← SD MISO
  7  D33   ← bus 1 SCL        7  D18   ← SD SCK
  8  D25                      8  D5
  9  D26                      9  TX2
 10  D27                     10  RX2
 11  D14                     11  D4    ← SD CS
 12  D12                     12  D2
 13  D13                     13  D15
 14  GND                     14  GND
 15  VIN (5V — don't use     15  3V3   ← sensor power
          for sensors)
```
Each bus uses its own side of the board: bus 0 on the right, bus 1 on the left. The SD card (production only) is also on the right. 3V3 and GND are both at the bottom right.

**Breadboard:** the 30-pin board is wide enough that a single standard breadboard leaves only one free hole row on one side. Straddle two breadboards placed side by side (or use a breadboard with a split centre channel wide enough for it), so both sides have free rows for jumpers. Then run 3V3/GND from the bottom-right pins to the power rails on both breadboards.

### Sensor boards
- **Adafruit SHT45:** STEMMA QT (JST-SH 4-pin): black = GND, red = V+ → 3V3, blue = SDA, yellow = SCL. Use STEMMA-QT-to-female-jumper cables. On-board 10 kΩ pull-ups.
- **Generic SHT45:** VCC → 3V3, GND, SDA, SCL.
- **Generic SHT31 (GY-SHT31-D style):** VIN → 3V3, GND, SDA, SCL.
  - **ADR → GND gives 0x44; ADR → 3V3 gives 0x45.** Always tie ADR explicitly; don't leave it floating.
  - ALR is not connected.
- **Generic BMP388:** VCC → 3V3, GND, SDA, SCL. Check the silkscreen pin order and whether the board has its own regulator.
  - **SDO sets the address: SDO → 3V3 gives 0x77, SDO → GND gives 0x76.** Tie SDO explicitly unless the board already pulls it one way (check with a meter, unpowered). The plan assumes 0x77.
  - CSB → 3V3 (or left as the board pulls it) keeps the chip in I2C mode. INT is not connected.
  - Generic means it may be relabelled: read the chip ID at stage 18 (BMP388 0x50; BMP390 0x60; a BMP280 answers 0x58 at register 0xD0 instead) and check pressure against a nearby weather station reduced to station pressure.

### Layout A: batch mode, 2 buses, no mux
```
bus 0 (D21/D22): Adafruit SHT45 ref @0x44 + SHT31 #n @0x45 (ADR→3V3)
bus 1 (D32/D33): SHT45 under test @0x44 + SHT31 #m @0x45 (ADR→3V3)
```
- Batch 1: ref + generic SHT45 + SHT31 #1, #2.
- Batch 2: ref + PTFE SHT45 + SHT31 #3 + one of #1/#2 carried over, to link the two batches.

Wire on a breadboard, with each bus on its own pair of rails.

### Layout B: TCA9548A mux
- **Mux connections:**
  - VIN → 3V3, GND → GND.
  - SDA/SCL → D21/D22 (bus 0 only).
  - A0–A2 left open, giving mux address 0x70.
  - RST left open (it's pulled up on the Adafruit board).
- **Channels:** each sensor goes on its own channel (SDn/SCn), 0–5, at its default address. All SHT31s have ADR → GND, i.e. 0x44.
- **Pull-ups:** each channel gets its pull-ups from the sensor board on it. All six boards have their own.

### Production (outdoor)
- I2C bus 0 carries three devices, all different addresses: SHT45 0x44, DS3231 0x68 (most DS3231 boards also have an AT24C32 memory chip at 0x57), BMP388 0x77.
Three physical parts:

1. **5 V 1 A USB-A wall charger**, plugged into a covered outlet, with an A-to-C cable to the warm box's USB-C pass-through. Later replaced by a solar 12 V-to-USB adapter (USB-A output) on the same cable. Mains power stays out of both boxes. 1 A covers the ESP32's WiFi bursts (several hundred mA) plus the boards (~110 mA at most). Use a short, thick cable: a long or thin one lowers the voltage at the board and eats into the AMS1117's headroom.
2. **Warm box** (everything that makes heat): ESP32, AMS1117 3.3 V module, DS3231 board, SD card board, BMP388. Otherwise sealed, with one breather vent (screw-in plug with a PTFE membrane, sold as "IP67/IP68 breather vent" or "membrane vent plug", often M12): it passes air, so pressure inside equals outside, but keeps out rain and insects. Fit it on the bottom or side, never the top. A fully sealed box would make the BMP388 track the box's own temperature instead of the weather. The heat inside is well under a watt, so no cooling vents are needed. The BMP388 lives here on purpose (decided 2026-10-02): warmth doesn't affect pressure readings, the electronics' heat keeps the bare BMP388 board above the dew point, and the sheltered box smooths wind gusts out of the pressure reading.
3. **Open box**: SHT45-PTFE only, open to outside air (free airflow, shaded from sun and rain; no breather vent, which passes air too slowly for humidity and temperature to follow outside), away from the warm box's heat, on ≤ 1 m of 4-conductor cable (3.3 V, GND, SDA, SCL).
   - If you use Cat5/6, pair SDA with GND and SCL with 3V3 to reduce crosstalk between SDA and SCL.
   - Seal the cable entry so water can't wick along the cable, but keep the membrane open to outside air.
- Pins are bus 0: D21/D22, GND; SD card: D18/D19/D23/D4. Board power from the AMS1117 module (fed from VIN), not the ESP32's 3V3 pin.

The wiring notes go into the repo as `sht-compare/WIRING.md` and `ambient/WIRING.md`.

## Code layout

```
common/sht/        SHT3x + SHT4x drivers (ESP-IDF v5 i2c_master API), CRC-8, serial-number read,
                   optional TCA9548A channel select, SHT4x heater
common/wifi/       STA connect with reconnect + UDP command server; same handler-callback API as
                   pico-dev/common/wifi/wifi.h; reads wifi_secrets.h (gitignored)
common/ds3231/     DS3231 real-time clock: read/set UTC time (adapted from temp-sense api_ds3231)
common/bmp388/     wrapper around Bosch's BMP3 sensor API
i2c-scan/          diagnostic kept permanently: scans bus 0 and bus 1, then each mux channel if a mux is found;
                   reports a stuck bus (SDA/SCL held low at idle)
sd-probe/          diagnostic kept permanently: mount the card, write a file, read it back, report size/free
sht-compare/       sensor table + read loop → CSV on serial; log_serial.py, analyze.py, WIRING.md
ambient/           production logger: wake → sample → record → SD ring → sleep; pushes to the dev10
                   listener inside the push window, plus status commands while awake;
                   copies of temp-sense sd_ring/xfer_proto/config_store/crc32
                   (to be extracted into a shared repo later); udp_client.py, WIRING.md
```

Every project copies the `build.sh` / `flash.sh` / `monitor.sh` trio from `blink/`, and its top-level CMakeLists sets `EXTRA_COMPONENT_DIRS ../common`.

## Stages

Each stage lists what's **new** and its **pass check**. Don't start the next stage until the check passes.

| # | New unknown | Test code | Pass check |
|---|---|---|---|
| 0 | nothing (baseline) | blink | still builds/flashes/monitors |
| 1 | **code:** `i2c-scan` | no sensors wired | builds; reports empty bus 0 and bus 1, no stuck lines |
| 2 | **wiring:** 1 Adafruit SHT45 on bus 0 (STEMMA cable, least error-prone) | i2c-scan | finds 0x44 on bus 0. If not, use a multimeter: 3.3 V at the sensor's V+, SDA/SCL idle at ~3.3 V |
| 3 | **code:** `common/sht` SHT4x + `sht-compare` with a 1-entry table | stage-2 wiring | serial number prints; T/RH plausible; CRC ok; breathing on it raises RH |
| 4 | **wiring:** swap the SHT45 for 1 generic SHT31 (ADR→GND) | i2c-scan | 0x44 on bus 0 |
| 5 | **code:** SHT3x driver | stage-4 wiring | same checks as stage 3 |
| 6 | **wiring:** move ADR→3V3, then add the SHT45 back beside it | i2c-scan | 0x44 + 0x45 on bus 0 |
| 7 | **wiring:** bus 1 (D32/D33) with its own pair | i2c-scan | both buses show 0x44 + 0x45 (batch Layout A) |
| 8 | **code:** 4-entry table, CSV logging, `log_serial.py` | stage-7 wiring | overnight log, all 4 sensors, CRC errors ≈ 0 |
| 9 | **code:** `analyze.py` | stage-8 log | offset/noise/dropout table per sensor |
| 10 | *(when the mux arrives)* **wiring:** Layout B | i2c-scan (mux-aware) | 0x70 on bus 0; 0x44 on channels 0–5 |
| 11 | **code:** mux entries in table | stage-10 wiring | all 6 logged together overnight; analyze |
| 12 | **code:** `common/wifi` + `ambient` as a UDP client: status line pushed every 10 s, `udp_listener.py` acks with UTC; joins the strongest access point | nothing wired | **passed 2026-10-03 on board 2:** connects; acks every 10 s; listener stopped/restarted with no reboot; router restart → reconnects with no reboot; fresh start picks the stronger access point |
| 12b | **code + meter:** deep-sleep current of a bare dev board (board 3, the spare: a minimal program that goes straight to deep sleep) | nothing wired | current from the 5 V supply measured awake and asleep. Decides whether a dev board can run on solar, or needs trimming (LEDs) or a bare module |
| 12a | **wiring:** AMS1117 module input from ESP32 VIN, ground shared, nothing on the module's output yet; 220 µF 25 V capacitor across VIN–GND; 2 × 100 kΩ divider from VIN to GPIO 34. Powered through the USB socket only. (Done first: VIN on USB alone, board 2 read 5.06–5.09 V → no diode.) | meter only; stage-12 code | ESP32 boots and its pushes reach the listener; VIN ≥ 4.75 V while it sends over WiFi; 3.3 V (±2%) at the module output, also during WiFi; GPIO 34 reads about half of VIN; same on the USB-A charger and on the laptop. The pass-through and USB inline meter are added when the box is built |
| 12c | **code:** settings store (NVS) + serial console with the bench commands; settings apply immediately and survive a reboot | stage-12a node | each command in the list works on the serial console; `config` changes survive `reboot`; `config push` changes the status-push rate live; opening the console with the terminal set not to toggle reset lines leaves the logger running |
| 12d | **code:** supply-voltage reading in `status` and in each push | stage-12c node | reading within ±2% of the meter at VIN, on the wall charger and on a bench supply set to 4.5 and 5.5 V |
| 13 | **wiring:** DS3231 board on bus 0, powered from the AMS1117 module | i2c-scan | finds 0x68 (and 0x57 if the board has the memory chip) |
| 14 | **code:** `common/ds3231` + a `time` / `settime` command in `ambient` | stage-13 wiring | time set from the PC reads back; still correct after unplugging the ESP32 for a few minutes |
| 15 | **wiring:** SD card board on SPI (D18/D19/D23/D4) | meter only | 3.3 V at the card's supply pin (or 5 V at the board's input if it has its own regulator); no shorts between the four signal lines |
| 16 | **code:** `sd-probe` | stage-15 wiring | mounts; file written, read back identical; survives a reboot |
| 17 | **wiring:** generic BMP388 on bus 0, SDO tied for 0x77 | i2c-scan | finds 0x77 alongside 0x68 |
| 18 | **code:** `common/bmp388` | stage-17 wiring | chip ID reads 0x50; pressure agrees with a nearby weather station reduced to station pressure, within the sensor's ±0.5 hPa |
| 19 | **wiring:** SHT45-PTFE on bus 0, short leads | i2c-scan | 0x44 + 0x68 + 0x77 |
| 20 | **code:** `ambient` sample loop: SHT45 + BMP388 on a timer, stamped from the DS3231, plus supply voltage, DS3231 temperature and BMP388 temperature, printed on serial only; interval settable by command (seconds for calibration runs, normal outdoor rate otherwise) | stage-19 wiring | plausible T/RH/pressure at the set interval; timestamps match the clock; switching to a few-second interval and back works without a reflash |
| 21 | **code:** record format + SD ring (copied from temp-sense, adapted) | stage-20 node | records persist across reboot; sequence numbers continue, no repeats |
| 22 | **code:** push transfer (packet format copied from `xfer_proto`; session logic reversed so the logger starts) + listener on dev10 (pico-dev repo), logger still always awake; push window, back-off, clock check in the acknowledgement | stage-21 node | Also: commands carried in the ack (queued at the listener, applied by the logger, result reported in the next push), the listener's command log, and the health fields in each push. Pass: every record arrives; a queued remote command is applied and its result logged at the listener; with the logger's pushes blocked for over two push intervals, the listener raises the silence alert; listener stopped for an hour, then the backlog arrives with no gaps; no push attempts outside the window; logger reports its clock offset from dev10; Pico pull unaffected |
| 22b | **code:** sleep between samples (wake by the ESP32's sleep timer; DS3231 alarm only if timing drifts) | stage-22 node | readings keep their interval and timestamps; pushes still arrive; current measured awake and asleep for the whole logger |
| 23 | **wiring:** 1 m outdoor cable to the SHT45, on the bench | stage-22 code | hours of logging with zero `valid=0` on the SHT45. If there are errors, drop to 50 kHz |
| 24 | **code:** heater logic (RH > 95% sustained) | bench | `heat` pulse visible as a temp spike; next readings marked invalid, then recover |
| 25 | install outdoors; listener running on dev10 (manual on dev10) | — | records arrive every push during dev10's hours; the overnight backlog drains on the first push each morning |
| 26 | *(separate, later)* extract the shared storage/transfer code into its own repo; temp-sense and ambient both switch to it | both loggers working | both rebuilt, flashed, and pulled by the collector with no change in output |

## Commands and reporting (decided 2026-10-03)

The logger never listens, so commands reach it two ways, sharing one set of command handlers:
- **Bench:** a serial console over USB (ESP-IDF's `console` component). Opening the port normally resets the ESP32; set the terminal program not to toggle the reset lines when the logger must keep running.
- **Remote:** commands queued at the listener ride in the next ack; the logger applies them and reports the result in its following push. They wait for the next push (up to the push interval). The listener keeps a command log (queued → sent with an ack → done, with result and times) and a view of pending and finished commands.

Settings are saved (NVS, or a settings file on the SD card like temp-sense's `config.dat`) and applied immediately, without a reboot.

| Command | What it does | Where |
|---|---|---|
| `status` | running time; clock time and whether set; last clock offset; WiFi (signal, access point, address); last push result; backlog; supply voltage | both |
| `read` | latest temperature, RH, pressure with timestamp | both |
| `sd` | storage ring: capacity, records, sequence range, last confirmed, backlog | both |
| `info` | firmware version and build date, SHT45 serial number, BMP388 chip ID, board MAC | both |
| `config get` | every setting and its value | both |
| `log [n]` | the last n entries of the problem log (WiFi connect failures with the router's reason code, push failures, restarts with cause, sensor/SD errors), plus counters since the log was cleared | both |
| `config sample <seconds>` | sampling interval (seconds-scale for calibration runs) | both |
| `config push <minutes>` | push interval (default 60) | both |
| `config window <start> <end>` | push window | both |
| `config clock auto\|report` | whether the ack's time corrects the clock or is only reported | both |
| `config listener <ip> <port>` | where to push | bench only |
| `config wifi <ssid> <pass>` | WiFi credentials, no reflash needed | bench only |
| `settime` | set the clock from the computer (backup to the ack time) | bench only |
| `push` | push now, ignoring the schedule | bench only |
| `heat` | one SHT45 heater pulse | both |
| `reboot` | restart; nothing lost | both |
| `format` | erase the SD card; needs a confirmation word | bench only |

Bench only: a wrong listener address or WiFi password sent remotely would cut the logger off, and erasing the card shouldn't be one queued line away.

**More information rather than less** (the user's preference). Each record carries, besides T/RH/pressure:
- supply voltage (2 × 100 kΩ divider from VIN to GPIO 34, about 25 µA; wired at stage 12a, read from 12d)
- warm-box temperature from the DS3231's built-in sensor (coarse, about ±3 °C) and from the BMP388

**Problem log:** an event log on the SD card, plus counters that survive restarts (not reset at boot), so e.g. "WiFi failed 140 times since Tuesday" is visible. Read with `log`. Plugging in USB may restart the board, but the log is on the card, so nothing is lost.

**Errors never stop logging:** WiFi setup and connect failures are logged and retried; the logger keeps sampling to the SD card and the backlog covers the gap. `wifi_connect` returns an error instead of restarting the board (done 2026-10-03). Remaining hard stops are only for things nothing can work without, and should fall back to defaults where possible.

**Silence alert at the collector:** the listener alerts when no push arrives from the logger for about two push intervals inside the push window. The usual response: plug in USB at the logger and read `log`. How the alert reaches the user (desktop notification on dev10, email, or a log line) is not yet chosen.

Each push also carries health information: last reset reason (power-on, crash, watchdog, brownout), WiFi connect time and push duration (the real radio-on cost, for solar sizing), error counts since boot (sensor reads, SD writes, push failures), clock offset at each ack and whether it was corrected, and free memory.

## Boiling-point calibration support

The ambient logger is the barometer for the thermistor boiling-point calibration (ads1115-dev `DESIGN.md`, Calibration). Decided 2026-10-02:

- **Order:** the boil comes after stage 20 at the earliest (BMP388 read on a timer with clock timestamps). Ice and Glauber's salt points don't need pressure and can run any time.
- **Where:** indoors, on a calm, settled-weather day, with range hood, bath fan, dryer and furnace blower off. The vessel stays open. The ambient logger sits on the counter in the same room on any USB charger, at roughly pot height (1 m of height ≈ 0.003 °C). No outdoor boil and no extra portability needed; indoor/outdoor and fan-effect checks were considered and skipped.
- **Sampling:** the ambient logger runs at a few seconds per reading during the boil (stage 20's settable interval), then goes back to its normal rate.
- **Matching readings:** thermistor readings are matched to pressure readings by time. Either the thermistor rig's readings carry timestamps from a clock set from the same source as the ambient logger's DS3231, or the two are run side by side and lined up by a shared start mark. Which one is decided when the thermistor reading stage is planned.

## Solar power (goal)

A goal since 2026-10-02: the user wants these projects on battery-backed solar power. dev10 itself runs on DC (grid AC → DC supply → the computer's DC supply), and is on about 06:00–18:00. The wall charger is the supply until the solar setup is built. Steps:

1. **Measure** current, awake (WiFi on) and asleep: the bare dev board at stage 12b, the whole logger at stage 22b. This replaces the estimates below.
2. **Size** the panel and battery from those numbers.
3. **Build** a small solar setup (panel, charge controller with low-voltage load disconnect, battery, 12 V → 5 V step-down converter such as a car USB charger) feeding the same USB cable. Run it beside the wall-charger data through cloudy stretches.

Design notes:
- Never feed 12 V to the AMS1117 or the ESP32's VIN; everything after the 5 V step-down stays as it is.
- Always-on WiFi is roughly 0.5–1 W (12–24 Wh/day): a large battery and panel for cloudy winter days. Sleeping between readings, with WiFi only for pushes inside the push window, cuts that by a large factor.
- The dev board's USB-to-serial chip, regulator and power LED keep drawing while the ESP32 sleeps; so does the AMS1117 module and its LED. Trimming LEDs, a low-drain regulator, or a bare module instead of a dev board may be needed.
- **Regulator (open):** the AMS1117 module's own drain (a few mA plus its LED) is a large share of a solar budget. A low-drain regulator (e.g. HT7333, a few µA; output limit ~250 mA, enough for the ~110 mA sensor rail) may replace it. Decide after stage 12b's numbers; the AMS1117 stays for bench work until then.
- Battery chemistry: LiFePO4 must not be charged below 0 °C without a heater or cold cutoff; sealed lead-acid charges in the cold but is heavier.
- The panel needs sun and the open box needs shade, so they mount apart.

## Collector change (pico-dev, stage 22)

- A new listener on dev10 receives pushes from the ambient logger: checks each packet, stores the records, replies with the acknowledgement carrying dev10's UTC time. It runs whenever dev10 is up, started by dev10's container setup; downtime loses nothing.
- collector.py keeps pulling the attic Pico as now.
- Ambient records go to their own table/CSV, since the record holds T, RH and pressure rather than one temperature per sensor.
- `OPERATIONS.md` gets a section on the ambient logger and the listener.

## Uncertainties (unverified; revisit as stages pass)

1. **30-pin pinout** comes from the standard DevKit-V1 order, not this board. Check the silkscreen before stage 2.
2. **The comparison has no absolute reference.** Agreement between boards isn't accuracy. Add a salt-jar check (NaCl ≈ 75.3% RH at 20 °C) as the known point; batch-mode medians of 4 are thin.
3. **Generic boards may be counterfeit.** Log each chip's serial number, and watch for odd command responses or outlier behaviour.
4. **The 1 m outdoor I2C cable** is the riskiest hardware choice. Stage 15 tests it; keep an alternative mounting approach in mind.
5. **The heater threshold** (RH > 95% sustained) is a guess, untested through a real winter.
6. **Sample interval:** the ambient logger should sample at the attic logger's rate, or a multiple of it, so readings line up. Not chosen yet.
7. **The dev10 collector/container side** can't be tested from this VM.
8. **Record layout.** The temp-sense protocol carries each reading as a 2-byte number with temperature in 1/16 °C steps (0.06 °C). That's too coarse if the SHT45 is checked against the calibrated thermistors (0.01 °C wanted). Pressure fits 2 bytes at 0.1 hPa steps. Likely needs a per-reading scale or a protocol version bump; decide at stage 21.
9. **Clock setting.** Each logger's DS3231 drifts ~1 min/year (±2 ppm rated), which is fine. The risk is the initial setting: the attic Pico's clock date was last recorded as unset. Both clocks must be set from the same source. The same applies to the thermistor rig if its boil readings are matched to the ambient logger's pressure by time.
10. **Spare parts on hand:** not confirmed that a DS3231 board and an SD card board are free for the ESP32, or what kind. Some SD boards want 5 V (own regulator + level shifter), some 3.3 V only; check before stage 15.
11. **FatFs on ESP-IDF:** ESP-IDF uses the same FatFs file library as the Pico code, but how directly temp-sense's `f_*` calls carry over (vs. going through ESP-IDF's file layer) is unchecked.
12. **BMP388 facts are from memory:** address 0x77 with SDO high (0x76 with SDO low), chip ID 0x50, CSB high selects I2C, ±0.5 hPa absolute. The generic board's pin labels, pull-ups and regulator are unknown until it's in hand. Check against the datasheet at stage 17.
13. **Heat and airflow:** the warm box's heat must not reach the open box; spacing and which one sits higher (warm air rises) are not yet decided. The open box's design (vent openings, sun and rain protection) is not yet described in this plan; it decides how fast the SHT45 follows outside air. Stage 23/25 should compare readings with the open box open vs. closed.
14. **AMS1117 input headroom:** it needs about 1.1 V above its output, so at least ~4.4 V in. Fed from VIN it gets the full USB 5 V (no diode on board 2) less cable and pass-through drop. A cheap charger, a sagging PC port, or a long thin USB cable could drop the sensor rail. Stage 12a measures it, including during WiFi bursts (a meter may not catch short dips).
15. **USB-to-VIN diode:** none found on board 2 (VIN = USB voltage; a zero-ohm-looking part near the socket). Assumed absent; boards 1 and 3 not checked. With power only through the USB socket this no longer matters.
16. **Breather vent lag:** how fast pressure inside the warm box follows outside through the vent is unchecked; expected seconds, fine for weather logging.
17. **Solar power figures are estimates:** 0.5–1 W always-on, the battery/panel sizes, and the dev board's sleep current (guessed at several mA or more) are unmeasured. Stages 12b and 22b replace them.
18. **Measuring sleep current:** a USB inline power meter (or a meter in series with VBUS on a cut cable) that resolves tens of µA to a few mA; what's on hand isn't known. Cheap USB meters often bottom out around 1 mA, which may be too coarse for sleep current. Measure from the charger, not a PC, so the serial chip's USB activity doesn't add to the reading.
19. **HT7333 figures are from memory**, not its datasheet.
20. **Listener on dev10:** port opening from its container and start-up with dev10 are untested from this VM.
21. **Why temp-sense left MQTT** (an earlier push-style design) hasn't been checked; if it was a reason that applies to push in general, it matters here.
22. **Push window in UTC or local time:** open. The clock runs in UTC; a local 06:00–18:00 window would shift an hour at each daylight-saving change unless the logger knows the time-zone rules. Suggested: set it in UTC, a little wider than dev10's hours.
23. **ESP32 analog input accuracy:** roughly ±1–2% after the chip's own calibration; calibrate against the meter if more is needed. The divider draws a constant small current (µA with large resistors).
24. **Serial console without resetting:** whether the terminal programs used here can open the port without toggling the reset lines is untested.
25. **Silence alert delivery:** how the listener reaches the user isn't chosen.
26. **USB-C pass-through:** not yet chosen; the ones seen don't pass CC (USB-A sources only). Data capability and fit must be checked per listing.

## Verification

End to end is stage 25's check (plus stage 26 when the shared library is extracted). Each earlier stage's pass check is its own verification, and `i2c-scan` stays in the repo as the first thing to run whenever wiring is in doubt.
