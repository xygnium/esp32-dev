# thermistor-cal, stage 1 plan: ADS1115 resolution test

## Where the code lives

| code | repo / path |
|---|---|
| ADS1115 driver, HAL interface (portable) | `ads1115-dev/ads1115_core/` |
| ESP-IDF glue for the HAL | `ads1115-dev/ports/ads1115_idf/` |
| this project: resolution test, later the calibration rig | `esp32-dev/thermistor-cal/` |

`thermistor-cal` pulls in `ads1115_core/` and `ports/ads1115_idf/` through `EXTRA_COMPONENT_DIRS`, pointing at the **sibling checkout** `~/dev/github/ads1115-dev` by relative path. No submodule for now. Stage 1j comes back to that question. No code is moved or copied from other repos. Only the three-line build/flash/monitor script pattern is copied from `blink/`.

## Goal

Prove each Lonely Binary board has a genuine ADS1115 (16-bit), not a relabelled ADS1015 (12-bit). This is arrival check 2 in `ads1115-dev/DESIGN.md`. Checks 3–5 (noise floor, pull-ups, address) come along with it.

Secondary goal: the library's first real code, exercised on hardware from the start.

## Hardware

- **ESP32:** the 30-pin ESP-WROOM-32 board, flashed over `/dev/ttyUSB0`.
- **I2C:** bus 0, **SDA = D21, SCL = D22**, the same pins as the SHT plan.
  - Check the silkscreen before wiring; the pin positions come from the standard DevKit-V1 30-pin order.
- **Power:** the ADS1115 board runs from the ESP32's **3V3** pin, never 5V/VIN.
- **Test input:** a potentiometer (10k–100k) across 3V3/GND, with the wiper to A0. Later, a jumper to short A0 to GND.
- **Multimeter:** for the pull-up and ADDR checks.

## Toolchain

ESP-IDF **v6.1** (`~/dev/esp32/esp-idf`, `release/v6.1`). Use the new `driver/i2c_master.h` API; the legacy `driver/i2c.h` isn't available.

## Layout

```
~/dev/github/
├── ads1115-dev/                 (library repo)
│   ├── ads1115_core/
│   │   ├── ads1115.h / .c       register access, single-shot conversion, config
│   │   └── ads_hal.h            interface a port provides: i2c_write, i2c_read, delay_ms
│   └── ports/
│       └── ads1115_idf/         ESP-IDF component implementing ads_hal.h on i2c_master
└── esp32-dev/
    └── thermistor-cal/
        ├── CMakeLists.txt       EXTRA_COMPONENT_DIRS → ../../ads1115-dev/ads1115_core, ../../ads1115-dev/ports/ads1115_idf
        ├── sdkconfig.defaults, main/
        └── build.sh, flash.sh, monitor.sh
```

`esp32-dev` builds only when `ads1115-dev` is checked out beside it. That's accepted for now.

GPIO control for excitation isn't in the HAL yet. It arrives with the thermistor stage.

## Stages

Code and wiring alternate, so a failure points at one or the other. Each stage stops for flashing or checking before the next one starts.

| # | type | what | pass check |
|---|---|---|---|
| 1a | **code** | Scaffold: library skeleton in `ads1115-dev` (HAL header, ESP-IDF port with I2C init), `thermistor-cal` project. The app only scans I2C bus 0 and prints the addresses found. | Builds, flashes, and reports an **empty bus** with nothing wired. SDA/SCL read high at idle. |
| 1b | **meter** | ADS1115 board unpowered: measure SDA→VDD, SCL→VDD (pull-ups) and ADDR→GND (pull-down). Record the values. Chip marking: BOGI ✓ (already seen). | Pull-ups present (expect ~10k). ADDR pulled to GND. |
| 1c | **wiring** | Wire one board: VDD→3V3, GND→GND, SDA→D21, SCL→D22, ADDR at default. | Scan finds **0x48**. If not: check 3.3 V at VDD and SDA/SCL idle high, with the meter, before touching code. |
| 1d | **code** | Driver reads the Config register (0x01) and prints it. Then a **timing sweep**: for each DR code 000–111, start a single-shot conversion (OS=1), wait for OS to go 1→0 (so there's no race right after the write), poll until OS=1, and time it with `esp_timer_get_time()`. 500 ms timeout per conversion. Print the 8 times. | Power-on default **0x8583**. This proves the read path, but not the chip type: ADS1015 has the same default. Timing identifies the chip: DR=000 takes **110–140 ms** on an ADS1115 (8 SPS) and about 7.8 ms on an ADS1015 (128 SPS). A timeout, or OS never dropping to 0, is a finding in its own right. |
| 1e | **wiring** | Pot across 3V3/GND, wiper → A0. | ~0–3.3 V on A0 with the meter as the pot turns. |
| 1f | **code** | Single-shot A0 conversions at **8 SPS, PGA ±4.096 V**. Print raw codes. Summarize every N readings: min/max, count of **odd codes**, histogram of `code & 0xF`. | Turning the pot slowly produces **odd codes** and a varied low nibble → genuine ADS1115. All codes multiples of 16 → ADS1015. |
| 1g | **wiring** | Replace the wiper with a jumper A0→GND. | — |
| 1h | **code** | Noise mode: PGA **±0.256 V**, 100 readings at 8 SPS. Print mean, min/max and spread. | Spread of a few LSB (1 LSB = 7.8 µV). Record it as this board's noise floor. |
| 1h2 | **code + wiring** | Inputs test (`TEST_INPUTS`): read A0, A1, A2, A3 against GND at PGA ±4.096 V, print the four voltages about once a second. Pot wiper moved to each input in turn. Added 2026-10-04 after an Amazon review of these boards reported A2/A3 dead and suspected an ADS1114 (which has only the A0/A1 pair). Every stage before this used only A0 and A1. | With the wiper on an input, that column follows the pot (~0–3.3 V) and the others don't. A2 or A3 not following its own pin → the board can't carry 3 thermistors + excitation monitor; set it aside. |
| 1i | **repeat** | Each remaining board: 1b, 1c, 1d, 1f, 1h, 1h2. 1h2 also on ADS-A. Label each (ADS-A, ADS-B, ADS-C) with its results. | All pass, or the fake is set aside. |
| 1j | **review** | Revisit a git submodule vs the sibling path, now that the library has real code and history. Weigh: does `esp32-dev` need to build without the sibling? Has a library change broken, or nearly broken, a build? Is the pointer-bump overhead worth pinning? Decide, and switch only if it pays. | A decision, recorded in `ads1115-dev/DESIGN.md`. |

Out of scope for stage 1: address strapping (0x49–0x4B) and multiple boards on one bus.

## Output

For each board, record: label, chip marking, pull-up values, ADDR default, timing fingerprint (conversion time for each DR code), resolution verdict (odd-code count, low-nibble histogram), and noise floor at ±0.256 V. Keep the results in `thermistor-cal/BOARDS.md`.

## Uncertainties

1. **The ADS1115 power-on Config value 0x8583** is recalled, not checked against the datasheet.
2. **The noise expectation (a few LSB at ±0.256 V, 8 SPS)** hasn't been checked against the datasheet noise table.
3. **Board pull-ups and ADDR pull-down** are assumed. Stage 1b measures them.
4. **The ESP32 internal pull-ups** are weak. D21 measured at **about 77k** (Thévenin estimate; see `BOARDS.md`), not the ~45k recalled. If 1b finds no board pull-ups, add 4.7k–10k before 1c.
5. **`ads1115_core/` and `ports/ads1115_idf/` (renamed from `core/`, `ports/esp-idf/` on 2026-09-30) as separate component directories:** the ESP-IDF side is settled in 1a. The Pico side (pico-dev `temp-sense`) isn't tested until later.
6. **Pin positions on the 30-pin board** come from the standard layout. Check the silkscreen.
7. **Timing-check figures (1d)** are recalled and haven't been checked against the datasheets: the ADS1015 data-rate table (DR=000 → 128 SPS), the ADS1115 oscillator tolerance (about ±10%, which sets the 110–140 ms window), and whether OS reads 0 immediately after the start write.
8. **Sibling path:** there's no record of which `ads1115-dev` commit an `esp32-dev` build used. If that matters before 1j, note the library commit hash in `BOARDS.md` alongside results.
