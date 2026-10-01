# thermistor-cal: board records

Stage 1 results, one section per board. See `PLAN-stage1-resolution-test.md`, Output.

## ESP32 baseline (DEVKITV1, see `../SETUP-PLAN.md`)

Stage 1a, 2026-09-26. Firmware: scan-only, I2C bus 0 with internal pull-ups on, nothing wired to D21/D22.

| measurement | value | meter |
|---|---|---|
| 3V3 pin | 3.29 V / 3.31 V | first meter / SC260 |
| D21 (SDA) idle | 3.1 V / 3.25 V | first meter / SC260 |
| D22 (SCL) idle | 3.1 V / 3.24 V | first meter / SC260 |
| D21 with 1 MΩ to GND | 3.02 V | SC260 |

**D21 internal pull-up, Thévenin estimate** (SC260 alone, 10 MΩ input on the 4 V range, then SC260 ∥ 1 MΩ = 909k):
- R_th ≈ **77k** (73k–80k for ±5 mV reading error); V_th ≈ **3.27 V**.
- V_th is about 35 mV below the 3.31 V rail: roughly 0.45 µA of leakage, or a small difference between the pad supply and the 3V3 pin. Negligible.
- D22 hasn't been loaded with the resistor. Its idle reading matches D21's to within 10 mV.

**Meters:**
- **Fieldpiece SC260:** 10 MΩ input on the 4 V DC range, 9.1 MΩ on 40–600 V.
- **The first meter:** model and input impedance unknown. It loads high-impedance nodes noticeably, so don't use it for pull-up voltages.

## ADS1115 boards: stage 1b (unpowered), 2026-09-26

Labels: white paint dots on each board. 1 dot = **ADS-A**, 2 dots = **ADS-B**, 3 dots = **ADS-C**. The board's supply pin is marked VCC (the chip's VDD). Meter: Fieldpiece SC260, resistance range with red on the first-named pin, unless noted.

| measurement | ADS-A | ADS-B | ADS-C |
|---|---|---|---|
| chip marking | BOGI | BOGI | BOGI |
| VCC → SDA | 10.0k | 9.9k | 9.9k |
| VCC → SCL | 9.9k | 9.9k | 9.9k |
| ADDR → GND | 8.99k | 8.56k | 8.67k |
| VCC → GND, ohms | 4M, climbing; later 3k–8k | 3.8M climbing, slowing near 4.5M; later 3k–8k | later 3k–8k |
| VCC → GND, diode mode | LED lights, 0L | LED lights, 0L | LED lights, 0L |
| verdict | pass | pass | pass |

Notes:
- **The boards have a power LED across VCC and GND.** It makes VCC → GND nonlinear. On the high resistance ranges the meter reads MΩ (the climb is the decoupling cap charging). On the low ranges its test voltage turns the LED on, and it reads 3k–8k, varying from try to try. In diode mode the LED lights and the display shows 0L (over range). A short would read about 0 V. **No short on any board.**
- **ADDR → GND reads 8.6k–9.0k,** a 5% spread against 1% on the pull-ups. The likely cause is a parallel path: ADDR → ESD diode → VCC → LED → GND. It hasn't been confirmed; swapping the leads or reading the SMD code would settle it. ADDR is pulled low on every board either way, so the address is 0x48.
- **The LED draws current whenever the board is powered** (not yet measured; probably about 1 mA) and makes a little heat near the ADC. It's worth remembering for battery nodes and for thermal work.
- **Not measured:** VCC → ALRT (only needed if ALERT/RDY is used), and ADDR → GND with the leads swapped.
- Chip markings: the user confirmed BOGI on all three boards (2026-09-26).

## ADS1115 boards: stage 1c (wired, scan), 2026-09-30

Wiring: VCC → 3V3, GND → GND, SDA → D21, SCL → D22, ADDR left at the board default. Firmware: the stage 1a scan build, unchanged (app version `b9c7d3a-dirty`; it had not been rebuilt since 1a). The ESP32's internal pull-ups are in parallel with the board's 10k.

| board | power LED | scan result | verdict |
|---|---|---|---|
| ADS-A | lit | `0x48 (found 1, timeouts 0)`, same on repeated scans | pass |
| ADS-B | — | — | pending (1i) |
| ADS-C | — | — | pending (1i) |

## ADS1115 boards: stage 1d (Config read + timing), 2026-09-30

Firmware: stage 1d test (`main/main.c`), ads1115-dev at `d114198`. I2C at 100 kHz. Same wiring as 1c; A0 not connected (timing doesn't depend on the input).

**What it shows:** the real ADS1115 (about 65,000 steps) and its cheaper sister, the ADS1015 (about 4,000 steps), differ in speed. At the slowest setting the ADS1115 takes about 1/8 s per reading; the ADS1015 about 1/128 s. Timing is the quick check; the step size itself is shown in 1f.

| board | Config at boot | Config after general-call reset | DR 000 time | verdict |
|---|---|---|---|---|
| ADS-A | 0x8583 | 0x8583 | 128.3 ms | **ADS1115** |
| ADS-B | — | — | — | pending (1i) |
| ADS-C | — | — | — | pending (1i) |

ADS-A, all eight speed settings (three passes, identical poll counts each time):

| DR | measured | ADS1115 nominal | ADS1015 nominal |
|---|---|---|---|
| 000 | 128.3 ms | 125 ms | 7.8 ms |
| 001 | 64.4 ms | 62.5 ms | 4.0 ms |
| 010 | 32.6 ms | 31.3 ms | 2.0 ms |
| 011 | 16.7 ms | 15.6 ms | 1.1 ms |
| 100 | 8.7 ms | 7.8 ms | 0.63 ms |
| 101 | 4.8 ms | 4.0 ms | 0.42 ms |
| 110 | 2.8 ms | 2.1 ms | 0.30 ms |
| 111 | 1.6 ms | 1.2 ms | 0.30 ms |

Notes:
- **The chip's own clock runs about 2.1% slow** (±0.3%): measured ≈ 1.0215 × nominal + 0.63 ms across all eight codes.
- **The 0.63 ms extra is the stopwatch, not the chip.** Each "done yet?" check takes about 0.4 ms, so every time is a whole number of checks. That's why the fast settings look proportionally long, and why the passes repeat exactly.
- **0x8583 at boot is the true power-on value:** the board had only run the scan firmware, which never writes Config.
- **Differences from the plan:** the sweep repeats every 5 s instead of once, and a general-call reset runs before the second Config read (the ADS keeps its settings across an ESP32 reset).
- **Still unchecked against datasheets:** the ADS1015 rate table and the ADS1115 clock tolerance. The ADS1115 rate table is borne out: all eight codes fit one line.
