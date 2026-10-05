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

## ADS1115 boards: stage 1e (pot on A0), 2026-10-01

Test input: a 10k linear-taper (B) pot, 210° of travel. Outer legs to 3V3 and GND, center (wiper) to A0. Meter: Klein MM400.

| measurement | value |
|---|---|
| pot unpowered, sweep | 8.6k to 2.8 Ω |
| center to GND, powered, A0 not yet connected | 0 to 3.28 V |
| verdict | pass |

8.6k is inside the usual ±20% tolerance for a 10k pot. The wiper never looks like more than about a quarter of that (2.2k) to A0.

## ADS1115 boards: stage 1f (resolution), 2026-10-01

Firmware: stage 1f test (`main/main.c`), ads1115-dev at `5bbca3c`. Single-shot conversions of A0 at 8 readings per second on the ±4.096 V range (1 step = 125 µV). I2C at 100 kHz.

**What it shows:** an ADS1015 has 16 times coarser steps, so on this scale it can only give multiples of 16: never an odd code, and the low four bits always 0. An ADS1115 gives every code.

| board | Config after a conversion | readings | range | odd codes | distinct codes | verdict |
|---|---|---|---|---|---|---|
| ADS-A | 0xc303 | 691 in 90 s, 0 errors | 0 to 26474 (0 to 3.309 V) | 330 | 455 | **16-bit, ADS1115** |
| ADS-B | — | — | — | — | — | pending (1i) |
| ADS-C | — | — | — | — | — | pending (1i) |

ADS-A, count of readings by low four bits, pot turned by hand end to end and back several times:

| 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | a | b | c | d | e | f |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 94 | 33 | 35 | 25 | 22 | 24 | 26 | 34 | 32 | 123 | 94 | 36 | 30 | 27 | 28 | 28 |

Notes:
- **The three tall bins are the end stops.** Bin 0 holds 62 readings of exactly 0 (pot parked at GND); bins 9 and a are the pot parked at full turn, codes 26473 and 26474. Away from the stops the bins are even.
- **Parked at full turn the reading flickers by one step** (26473/26474) and no more. That's a first look at noise on this range; 1h measures it properly on the ±0.256 V range.
- **Scale check:** full turn reads 3.309 V against 3.28 V on the Klein MM400, about 1% apart. The SC260 read this rail at 3.31 V in stage 1a.
- **The Config read-back 0xc303** is the value written with the "start" bit showing "idle": A0 against GND, ±4.096 V, single-shot, 8 per second, comparator off. Those field codes are recalled, not checked against the datasheet; the read-back and the scale check both bear them out.
- **Bottom end reads exactly 0.** The 2.8 Ω stop resistance predicts about 1 mV (8 steps). Not explained; some of the 2.8 Ω may have been meter leads.
- **Reading rate:** about 130 ms per reading (128 ms conversion, checked for "done" every 10 ms).
- **Difference from the plan:** each reading also prints hex and microvolts; summaries come every 40 readings.

## ADS1115 boards: stages 1g, 1h (noise and zero), 2026-10-01

Firmware: stage 1h noise test (`main/main.c` with `TEST_NOISE 1`), ±0.256 V range (1 step = 7.8 µV), 8 readings per second, blocks of 100. The runs against ground used ads1115-dev `5bbca3c`; the A0-minus-A1 runs used `814588c`. I2C at 100 kHz.

Wiring (1g): pot removed. The ADS board's GND, the ESP32's GND and the A0 jumper all land on one breadboard ground bus. With the 1f firmware still running, A0 on that bus read code −1 (−125 µV on the ±4.096 V range) on every reading.

ADS-A:

| input arrangement | Config | blocks | mean (steps) | mean (µV) | spread | std dev (steps) |
|---|---|---|---|---|---|---|
| A0 to the ground bus, read against ground | 0xcb03 | 4 | −10.81 to −10.89 | −85 | 1 step | 0.31–0.39 |
| same, jumper moved 3 cm closer to the board along the bus | 0xcb03 | 3 | −10.28 to −10.32 | −80.5 | 1 step | 0.45–0.47 |
| A0 to A1, pair floating, read as A0 minus A1 | 0x8b03 | 3 | 0.00 | 0.0 | 0 | 0.00 |
| A0 to A1, A1 to the ground bus, read as A0 minus A1 | 0x8b03 | 3 | 0.00 | 0.0 | 0 | 0.00 |

No conversion errors in any block.

| board | noise floor (±0.256 V, 8 per second) | converter zero error | verdict |
|---|---|---|---|
| ADS-A | spread of 1 step (7.8 µV) or less | under 1 step | pass |
| ADS-B | — | — | pending (1i) |
| ADS-C | — | — | pending (1i) |

Notes:
- **The noise is smaller than one step.** Against ground the readings split between two neighbouring codes (−11 and −10); as a difference they were all 0. So the standard deviation mostly reflects where the mean sits between two codes, not the true noise. It rose after the 3 cm move only because the mean moved nearer the middle.
- **The −80 µV is in the ground path, not the converter.** Read as a difference between two inputs, the zero is exact.
- **Likely cause: the ADS board's own ground lead.** The board's supply current (power LED plus chip, probably a little over 1 mA, not measured) returns through its ground wire to the bus. That lifts the chip's ground above the bus, so an input tied to the bus reads negative. 80 µV at about 1.2 mA is roughly 0.07 Ω: a jumper wire and two breadboard contacts. The 3 cm of bus accounted for about 4 µV. The ground trace on the ADS board itself may also contribute; these tests don't separate it from the wire.
- **Slow drift isn't ruled out** as part of the 4 µV change: the two runs were a few minutes apart.
- **For the thermistor rig:** take ground references at the ADS board, not at a shared bus, or read as a difference between two inputs. This offset depends on wiring, so repeat this test after the move to perf board; the chip checks (1b, 1d, 1f) don't need repeating.
- **Not yet shown:** that the A0-minus-A1 setting responds to a real voltage. All-zero readings fit a quiet chip with a shorted input, but would also fit a dead channel.
- **Differences from the plan:** 1h reports standard deviation as well as mean, min/max and spread, and repeats every block. The A0-minus-A1 runs were added to find the source of the offset. `main.c` keeps the 1f test too, chosen with `TEST_NOISE`; `NOISE_MUX` picks the noise test's input.
- **Field codes** for the ±0.256 V range and A0-minus-A1 are recalled, not checked against the datasheet. The Config read-backs match what was written, and the −85 µV here agrees with the −1 step seen on the ±4.096 V range.

## ADS1115 boards: stage 1h2 (all four inputs), 2026-10-04

Why: an Amazon review of these boards reported A2 and A3 dead and suspected an ADS1114, which has only the A0/A1 pair. Every earlier stage used only A0 and A1.

Method: `TEST_INPUTS`. A0, A1, A2, A3 each read against ground (±4.096 V, 8 per second), four voltages printed about once a second. Pot across 3V3/GND; wiper moved to each input in turn with the program running; the other three inputs unconnected. Library at `ads1115-dev` e3065ef. Readings as reported from the console by eye, not captured to a file.

| wiper on | A0 column | A1 column | A2 column | A3 column |
|---|---|---|---|---|
| A0 | follows the pot (odd and even values) | 582–583 mV | 582–583 mV | 582–583 mV |
| A1 | 582.2–582.4 mV | follows the pot | 582.1–582.2 mV | 582.1–582.2 mV |
| A2 | not noted | not noted | follows the pot | not noted |
| A3 | not noted | not noted | not noted | follows the pot |

| board | A0 | A1 | A2 | A3 | verdict |
|---|---|---|---|---|---|
| ADS-A | follows | follows | follows | follows | pass: four independent inputs, not an ADS1114 |
| ADS-B | — | — | — | — | pending |
| ADS-C | — | — | — | — | pending |

Notes:
- **Unconnected inputs sit at about 582 mV**, steady to a few tenths of a mV, and don't copy the driven input.
- **Not recorded:** the four Config read-back lines printed at start (`A0:` to `A3:`), and the pot's end-to-end voltages on each input.
