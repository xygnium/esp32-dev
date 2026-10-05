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
  - Ambient logger: 1× generic BMP388 board, CJMCU-388 (arrived 2026-10-04) (pressure), DS3231 clock board, SD card board, AMS1117 3.3 V module, breather vent (IP68), plus the SHT45-PTFE chosen by the comparison.
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
| Sidecar excitation (EXC) | 25 | D25 | Production only. Switches the thermistor divider on during readings (ads1115-dev pulsed excitation). Not a strapping pin |
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
  8  D25   ← sidecar EXC       8  D5
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
- **Thermistor sidecar (temporary, decided 2026-10-04):** a reusable module (ADS1115 board + precision resistor + thermistor on a CAT5 pair) that calibrates the SHT45 in place and then moves on to other projects. Its standard interface is five wires: 3.3 V (AMS1117), GND (single ground point; nothing else shares that wire), SDA (D21), SCL (D22), EXC (D25). On this logger: a plain 5-pin header on the warm-box board; jumper wires to the module; the cable leaves through the SHT45 cable's entry hole with a temporary seal (putty or tape). The module rides beside the warm box in a zip-lock bag or scrap box, out of the sun; the thermistor sits beside the SHT45 in the open box. Afterwards: unplug, reseal the hole. Not chosen: the module inside the warm box (box sized for a temporary part), or a GX12/M12 panel connector and sealed housing (too costly for a temporary arrangement). Bench-calibrating the SHT45 before install remains possible too. **Warm-box enclosure search:** choose a cable entry (gland or grommet) that can take the SHT45 cable plus a temporary second cable. Same five-wire interface on every host, including the Pico rework.
- Pins are bus 0: D21/D22, GND; SD card: D18/D19/D23/D4; sidecar EXC: D25. Board power from the AMS1117 module (fed from VIN), not the ESP32's 3V3 pin.

**Board wiring tables (production).** SD card and DS3231 pin names are from the common board types (check the silkscreen); BMP388 names as read off the CJMCU-388; ADS1115 names from `thermistor-cal/BOARDS.md`.

SD card board (SPI):

| SD board pin | Goes to | Notes |
|---|---|---|
| VCC (or 5V / 3V3) | VIN (5 V) if it has a regulator, AMS1117 3.3 V if not | wire as on the Pico logger (uncertainty 10) |
| GND | GND rail | |
| SCK / CLK | D18 | |
| MISO / DO | D19 | |
| MOSI / DI | D23 | |
| CS | D4 | |

DS3231 clock board (I2C, 0x68; memory chip 0x57):

| DS3231 pin | Goes to | Notes |
|---|---|---|
| VCC | AMS1117 3.3 V | |
| GND | GND rail | |
| SDA | D21 | |
| SCL | D22 | |
| SQW | not connected | spare: alarm output, could wake the ESP32 from sleep later |
| 32K | not connected | |

BMP388, CJMCU-388 (I2C, 0x77 expected):

| BMP388 pin | Goes to | Notes |
|---|---|---|
| VIN | AMS1117 3.3 V | board regulator gives the chip ~3.0 V |
| 3V0 | not connected | board regulator output; never tie to the 3.3 V rail |
| GND | GND rail | |
| SCK | D22 (I2C clock) | |
| SDI | D21 (I2C data) | |
| SDO | open if pulled up on the board (0x77), else 3.3 V | meter check pending |
| CS | open if pulled up (I2C mode), else 3.3 V | meter check pending |
| INT | not connected | |

Thermistor sidecar module, ADS1115 board (I2C, 0x48). Channel use is proposed, from ads1115-dev's design (one channel monitors the excitation; each thermistor is a ratio against it):

| ADS1115 pin | Goes to | Notes |
|---|---|---|
| VCC | header 3.3 V | |
| GND | header GND | the module's single ground point |
| SDA | header SDA (D21) | |
| SCL | header SCL (D22) | |
| ADDR | board default (pulled to GND → 0x48) | |
| ALRT | not connected | |
| A0 | excitation monitor: EXC via 1 kΩ, 100 nF to GND at the pin | proposed |
| A1 | thermistor tap via 1 kΩ, 100 nF to GND at the pin | proposed |
| A2, A3 | not connected or tied to GND | spare (up to 3 thermistors) |

Divider on the module: EXC (D25) → precision 10 kΩ → tap (A1) → thermistor over its CAT5 pair → module GND. While fitted the module adds a 5th device and pull-up pair to the bus (combined pull-up ~2 kΩ or a little less) and its power LED draws a mA or two.

5-pin sidecar header on the warm-box board:

| Header pin | Goes to |
|---|---|
| 1 | 3.3 V (AMS1117) |
| 2 | GND |
| 3 | SDA (D21) |
| 4 | SCL (D22) |
| 5 | EXC (D25) |

Connections per point on the warm-box board:

| Point | Connections |
|---|---|
| 3.3 V (AMS1117 out) | DS3231, BMP388, SHT45 cable, sidecar header (+ SD if 3.3 V) |
| GND | SD, DS3231, BMP388, SHT45 cable, sidecar header, AMS1117, 220 µF, divider |
| D21 (SDA) | DS3231, BMP388, SHT45 cable, sidecar header |
| D22 (SCL) | DS3231, BMP388, SHT45 cable, sidecar header |
| D18, D19, D23, D4 | SD card only |
| D34 | supply-voltage divider |
| D25 | sidecar header |
| VIN (5 V) | AMS1117, 220 µF, divider (+ SD if 5 V type) |

**Supply-voltage divider (stage 12a)**

```
ESP32 VIN (left pin 15) ──────────┐
                                  │
                                 [R1] 100 kΩ 1%
                                  │
                       junction ──┼──────────────── 30 AWG ──── ESP32 D34 (left pin 4)
                                  │        │
                                 [R2]     [C1] 100 nF ("104")
                            100 kΩ 1%      │
                                  │        │
GND rail ─────────────────────────┴────────┘
```

| From | Part | To |
|---|---|---|
| ESP32 VIN (left pin 15), or the VIN wire to the AMS1117 | R1, 100 kΩ 1% | junction |
| junction | R2, 100 kΩ 1% | GND rail |
| junction | C1, 100 nF | GND rail |
| junction | 30 AWG wire | ESP32 D34 (left pin 4) |

Expected: unpowered, D34 to GND somewhere between ~50 and ~100 kΩ (the meter sees R2 directly and R1 through VIN and the ESP32 board's circuits; may climb briefly while C1 charges); the real check is "not near 0 Ω". Powered, D34 ≈ half of VIN (~2.5 V). Pin numbers from the standard DevKit V1 layout; check the board's silkscreen.


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
| 12a | **wiring:** AMS1117 module input from ESP32 VIN, ground shared, nothing on the module's output yet; 220 µF 25 V capacitor across VIN–GND; 2 × 100 kΩ divider from VIN to GPIO 34 plus 100 nF across the lower resistor (divider and capacitor away from the ESP32, 30 AWG lead to GPIO 34). Powered through the USB socket only. (Done first: VIN on USB alone, board 2 read 5.06–5.09 V → no diode.) | meter only; stage-12 code | ESP32 boots and its pushes reach the listener; VIN ≥ 4.75 V while it sends over WiFi; 3.3 V (±2%) at the module output, also during WiFi; GPIO 34 reads about half of VIN; same on the USB-A charger and on the laptop. The pass-through and USB inline meter are added when the box is built. **Passed 2026-10-04 on board 2:** unpowered VIN–GND and module out–GND open, capacitor stripe on GND; charger: VIN 5.01 V, D34 2.49 V (49.7%); desktop USB (stood in for the laptop): VIN 5.06–5.09 V, D34 2.534 V (49.8%), module out 3.298 / 3.31 V; pushes steady throughout. Not measured: module output on the charger. Divider alone 199.5 kΩ, R1 99.7 kΩ |
| 12c | **code:** settings store (NVS) + serial console with the bench commands; settings apply immediately and survive a reboot | stage-12a node | each command in the list works on the serial console; `config` changes survive `reboot`; `config push` changes the status-push rate live; opening the console with the terminal set not to toggle reset lines leaves the logger running. **Passed 2026-10-04 on board 2** (commands: help, status, info, config, push, quiet, reboot): live push-rate change; settings survive `reboot`; bad input refused with nothing changed; a bad saved value falls back to its default at boot. Bug found and fixed: the listener-address check accepted shorthand like "1.2.3" (lwIP's parser); now exactly four numbers 0–255. **Not yet met:** opening the console without restarting the board (pyserial restarts it; try `idf.py monitor --no-reset`, uncertainty 24). **Untested:** `config wifi` (needs the real password to restore; user to try a wrong password, then the right one) |
| 12d | **code:** supply-voltage reading in `status` (fresh reading each time) and in each push (`vin_mv=`), ESP-IDF ADC with eFuse-reference calibration, 16-sample average; low-supply watch with `config lowv` (warning on going low and on recovering, 50 mV margin; count in `status`) | stage-12c node | reading within ±2% of the meter at VIN, on the USB-A wall charger and on the laptop; a low-threshold crossing (threshold temporarily set just above the normal reading) writes a log entry. The listener's low-supply alert comes with stage 22 **Passed 2026-10-04 on board 2:** desktop USB meter 5.09 V / logger 5.11 V (+0.4%); USB-A charger meter 5.00 V / logger 5.018 V (+0.4%); steady across readings; `config lowv 5.2` gave one "supply low" warning and count 1, no repeat while low; back to 4.6 V gave "supply recovered"; out-of-range and non-numeric thresholds refused. Being near the top of the ADC range (uncertainty 27) didn't hurt at 5 V. |
| 13 | **wiring:** DS3231 board on bus 0, powered from the AMS1117 module | i2c-scan | finds 0x68 (and 0x57 if the board has the memory chip) |
| 14 | **code:** `common/ds3231` + a `time` / `settime` command in `ambient` | stage-13 wiring | time set from the PC reads back; still correct after unplugging the ESP32 for a few minutes |
| 15 | **wiring:** SD card board on SPI (D18/D19/D23/D4) | meter only | 3.3 V at the card's supply pin (or 5 V at the board's input if it has its own regulator); no shorts between the four signal lines |
| 16 | **code:** `sd-probe` | stage-15 wiring | with the 32 GB SDHC card: mounts; file written, read back identical; survives a reboot (and, if wanted, the same with a 128 MB SDSC card) |
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
| 26 | *(separate, later)* extract the shared storage/transfer code into its own repo; temp-sense and ambient both switch to it. The Pico's card layer will also need swapping from the SDSC-only library to one for SDHC when the 128 MB cards run out | both loggers working | both rebuilt, flashed, and pulled by the collector with no change in output |

## Commands and reporting (decided 2026-10-03)

The logger never listens, so commands reach it two ways, sharing one set of command handlers:
- **Bench:** a serial console over USB (ESP-IDF's `console` component). Opening the port normally resets the ESP32; set the terminal program not to toggle the reset lines when the logger must keep running.
- **Remote:** commands queued at the listener ride in the next ack; the logger applies them and reports the result in its following push. They wait for the next push (up to the push interval). The listener keeps a command log (queued → sent with an ack → done, with result and times) and a view of pending and finished commands.

Settings are saved in NVS (non-volatile storage: the ESP32's small key-value settings area in flash; decided at stage 12c) and applied immediately, without a reboot. Flashing new firmware doesn't erase them. The WiFi password is stored in plain text in flash (readable with esptool by anyone holding the board): accepted for this application (2026-10-04). A router that separates IoT, guest and admin networks is a possible later upgrade; it would also allow address reservations by MAC, but then the router must let UDP 8080 through from the IoT network to dev10 (push needs only that one outbound path).

| Command | What it does | Where |
|---|---|---|
| `status` | running time; clock time and whether set; last clock offset; WiFi (signal, access point, address); last push result; backlog; supply voltage | both |
| `read` | latest temperature, RH, pressure with timestamp | both |
| `sd` | storage ring: capacity, records, sequence range, last confirmed, backlog | both |
| `info` | firmware version and build date, SHT45 serial number, BMP388 chip ID, board MAC | both |
| `config get` | every setting and its value | both |
| `log [n]` | the last n entries of the problem log (WiFi connect failures with the router's reason code, push failures, restarts with cause, sensor/SD errors), plus counters since the log was cleared | both |
| `config sample <n>[s\|m]` | sampling interval; bare number = seconds; 1 s–1 h (default 60 s; seconds-scale for calibration runs) | both |
| `config push <n>[s\|m\|h]` | push interval; bare number = minutes; 10 s–24 h (default 1 h) | both |
| `config window <start> <end>` | push window | both |
| `config clock auto\|report` | whether the ack's time corrects the clock or is only reported | both |
| `config listener <ip> <port>` | where to push | bench only |
| `config wifi <ssid> <pass>` | WiFi credentials, no reflash needed | bench only |
| `config lowv <volts>` | supply-low threshold, 3.0–5.5 V (default 4.6 V, provisional) | both |
| `settime` | set the clock from the computer (backup to the ack time) | bench only |
| `push` | push now, ignoring the schedule | bench only |
| `heat` | one SHT45 heater pulse | both |
| `quiet [on\|off]` | hide (on) or show (off) successful pushes on the serial console, so log lines don't land in the middle of typing; problems always shown; on at every boot, not saved | bench only |
| `reboot` | restart; nothing lost | both |
| `format` | erase the SD card; needs a confirmation word | bench only |
| `flush` | write the waiting batch to the SD card now | both |
| `shutdown` | flush, stop sampling, report "ready for power off" | both (remote is the useful one) |
| `resume` | start sampling again after `shutdown` | both |

Bench only: a wrong listener address or WiFi password sent remotely would cut the logger off, and erasing the card shouldn't be one queued line away.

**More information rather than less** (the user's preference). Each record carries, besides T/RH/pressure:
- supply voltage (2 × 100 kΩ 1% metal-film divider from VIN to GPIO 34, about 25 µA, any wattage; a 100 nF ceramic capacitor ("104") across the lower resistor steadies the high-resistance input during sampling. Layout for board 2's perf board: the divider and capacitor sit together away from the ESP32 (space), with a 30 AWG lead from their junction to GPIO 34; the capacitor feeds the lead from a low resistance, so pickup should stay small. If stage 12d's readings are jumpy, first fix: move the capacitor to the GPIO 34 end of the lead. Keep the lead short and away from the antenna end; wired at stage 12a, read from 12d)
- warm-box temperature from the DS3231's built-in sensor (coarse, about ±3 °C) and from the BMP388

**Problem log:** an event log on the SD card, plus counters that survive restarts (not reset at boot), so e.g. "WiFi failed 140 times since Tuesday" is visible. Read with `log`. Plugging in USB may restart the board, but the log is on the card, so nothing is lost.

**Errors never stop logging:** WiFi setup and connect failures are logged and retried; the logger keeps sampling to the SD card and the backlog covers the gap. `wifi_connect` returns an error instead of restarting the board (done 2026-10-03). Remaining hard stops are only for things nothing can work without, and should fall back to defaults where possible.

**Silence alert at the collector:** the listener alerts when no push arrives from the logger for about two push intervals inside the push window. The usual response: plug in USB at the logger and read `log`. An "all clear" follows when pushes resume. No alerts while dev10 is off (no pushes are expected then); after the listener starts it waits a full period before it can alert.

**Alert delivery (decided 2026-10-04): email**, to an address that is configurable and kept in the listener's local settings file (git-ignored, like `wifi_secrets.h`), not in the plan or code. The listener also writes every alert to its own log. Sending needs a mail account to send from; with Gmail that's an app password (requires two-step verification). Other channels (desktop notification on dev10, a phone push service such as ntfy) can be added later.

**Low-supply handling (wall power):** the supply voltage is in every record and push, so trends show at the listener (a weakening charger, a connection going bad). Crossing a low threshold writes a problem-log entry on the logger, so `log` after a crash or brownout restart shows whether the supply sagged first. The listener alerts when pushes report the supply below a threshold, like the silence alert. Thresholds (perhaps ~4.6 V) are set after normal values have been seen, not guessed now.

Each push also carries health information: last reset reason (power-on, crash, watchdog, brownout), WiFi connect time and push duration (the real radio-on cost, for solar sizing), error counts since boot (sensor reads, SD writes, push failures), clock offset at each ack and whether it was corrected, and free memory.

## SD card contents (reviewed 2026-10-04)

**Card:** the 32 GB SDHC card, formatted FAT32. ESP-IDF's SD driver handles both standard-capacity (SDSC, ≤2 GB, like the 128 MB cards) and high-capacity (SDHC) cards, so the ESP32 isn't tied to the 128 MB cards, which are no longer sold; those stay with the Pico loggers. To confirm at stage 16 (`sd-probe`) with the 32 GB card.

| File | Holds | Written | Size |
|---|---|---|---|
| `ring.dat` | readings, fixed 32-byte records at slot seq % capacity (temp-sense scheme) | every sample | fixed, created once (64 MB ≈ 4 years at one sample a minute) |
| `ring_state.dat` | next seq, confirmed watermark, counters that survive restarts | at each acknowledged push | one 512-byte block, rewritten |
| `events.dat` | problem log: a second ring of fixed-size entries (time, type, code, value) | when something happens | fixed, tens of thousands of entries |

Settings stay in flash (NVS, stage 12c), which is written only on `config` commands. Anything saved often (counters, problem log, clock history) goes on the SD card, not in flash, to avoid wearing out the flash; the SD card manages its own wear. Until stage 21 brings the card, counters reset at each restart. No `config.dat`/`labels.dat`: settings are in NVS and the sensors are fixed.

**Reading record (32 bytes, 16 per 512-byte block):** seq (4), UTC seconds from the DS3231 (4), SHT45 temperature in 0.01 °C (2), SHT45 RH in 0.01 % (2), BMP388 pressure in 1 Pa (4), supply voltage in mV (2), DS3231 temperature (2), BMP388 temperature (2), valid flags, one bit per sensor (1), format version (1), spare zeroed (4), CRC-32 (4). The DS3231 (0.25 °C steps, about ±3 °C, updated every 64 s) and BMP388 (about ±0.5 °C) report their own chip temperatures: the warm box, not outdoor air. Figures from memory; confirm at stages 14 and 18.

**Problem-log entry types:** WiFi connect failed (with reason code); push failed or no reply; restart (with cause); sensor read error; SD error; supply low / recovered; clock offset at an ack and whether corrected; remote command applied (with result). Per-push health details (connect time, push duration, free memory) are sent, not stored; anything abnormal becomes a log entry.

**Batched writes (decided 2026-10-04).** Records collect in the ESP32's RTC memory (8 KB; survives a restart, crash or sleep, but not a power loss) and are written to the card as whole blocks. The batch is written out ("flushed"):
- before every push (so pushes send from the card, and pushed data is on the card);
- before `reboot`, `format` and sleep;
- when the supply falls below the low threshold (then writes stop; see Solar power);
- on a `flush` command;
- when the oldest held record reaches the time limit (e.g. 10 minutes), which caps what a power cut can lose;
- after any restart, for whatever was waiting in RTC memory.

**Planned power-down:** two ways, no button needed. (1) A remote `shutdown` command queued at the listener: at the next push the logger flushes, stops sampling and sends an extra push saying "ready for power off"; `resume` (or the next restart) starts sampling again. It waits for the next push, so set the push rate faster before a planned visit. (2) Unplug within one sample interval after a push lands at the listener: every push flushes first, so nothing is waiting. Both need the listener's log in view; at the box with only a laptop, rely on (2) without confirmation. Unplanned power loss still loses what's waiting, at most the time limit's worth. The swap-cables procedure for flashing counts as a power cut, so use (1) or (2) first.

**Clock offset (decided 2026-10-04):** every push sends the offset to the listener, so the full series lives there (lining up the loggers, clock health, catching a wrong dev10 clock). The logger's problem log records only corrections (with size), offsets above a threshold (~2 s) and big jumps (a clock reset, e.g. a dead coin cell). Realigning: `clock auto` sets the DS3231 from the ack's time when the offset exceeds ~2 s (ack time is whole seconds; alignment within about a second, plenty for 1–2 minute samples), and logs it; `settime` on the console or queued remotely sets it by hand. dev10's own clock is kept right by an internet time service (NTP), so `clock auto` is the default (decided 2026-10-04). The attic Pico will be reworked to match the ESP32's operation (push, clock check, auto correction) in the near future, as a separate pico-dev job.

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
- **Low-supply protection on solar**, in order as the voltage falls: (1) skip WiFi pushes but keep sampling, the backlog holds the data; (2) stop SD writes before the voltage can corrupt the card's file system (a brownout mid-write can damage it; the most important step); (3) sleep until the voltage recovers, checking at each wake. Thresholds come from measuring what the AMS1117 and SD card actually tolerate.
- Before running on solar, check the supply-voltage reading across a range (e.g. 4.5–5.5 V fed through a USB cable from a bench supply), set aside from stage 12d.
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
6. **Sample interval:** the attic logger samples every 2 min (user's recollection, 2026-10-04). The user keeps the two loggers' sample rates the same by setting both by hand (`config sample` on each).
7. **The dev10 collector/container side** can't be tested from this VM.
8. **Record layout.** The temp-sense protocol carries each reading as a 2-byte number with temperature in 1/16 °C steps (0.06 °C), too coarse for checking against the calibrated thermistors. Settled for the SD record (see SD card contents: 0.01 °C, 0.01 % RH, 1 Pa); the push packet format follows at stage 22.
9. **Clock setting.** Each logger's DS3231 drifts ~1 min/year (±2 ppm rated), which is fine. The risk is the initial setting: the attic Pico's clock date was last recorded as unset. Both clocks must be set from the same source. The same applies to the thermistor rig if its boil readings are matched to the ambient logger's pressure by time.
10. **Spare parts on hand:** answered 2026-10-04: a DS3231 board and an SD card board are on hand, the same types used on the Pico logger (proven parts). Still to note: which supply voltage the SD board gets on the Pico (3.3 V or 5 V), to wire it the same way at stage 15.
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
22. **Push window: UTC (decided 2026-10-04).** No time-zone setting on the logger. dev10's ~06:00–18:00 local hours slide by an hour against a UTC window at each daylight-saving change, so set the window about an hour wider than dev10's hours; pushes at the edges fail harmlessly and back off. (A local-time window via a time-zone string was the alternative.) Units also decided: no change (bare `config push` = minutes, bare `config sample` = seconds).
23. **ESP32 analog input accuracy:** roughly ±1–2% after the chip's own calibration; calibrate against the meter if more is needed. The divider draws a constant small current (µA with large resistors).
24. **Serial console without resetting:** whether the terminal programs used here can open the port without toggling the reset lines is untested.
25. **Alert email from dev10:** whether dev10's container setup allows outgoing mail connections is unchecked; setting up the sending account's app password is a step on dev10.
26. **USB-C pass-through:** not yet chosen; the ones seen don't pass CC (USB-A sources only). Data capability and fit must be checked per listing.
27. **Supply reading near the top of the ADC's accurate range:** the divider puts ~2.5 V on GPIO 34. The ESP32's calibrated range at 12 dB attenuation is good to about 2.45 V (from memory) and less accurate above. If stage 12d misses ±2% at 5 V, change the lower resistor (e.g. 68 kΩ puts ~2.0 V on the pin) and the divider ratio in `supply.c`.

## Verification

End to end is stage 25's check (plus stage 26 when the shared library is extracted). Each earlier stage's pass check is its own verification, and `i2c-scan` stays in the repo as the first thing to run whenever wiring is in doubt.
