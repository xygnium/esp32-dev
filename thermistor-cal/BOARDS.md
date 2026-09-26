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
- The chip markings are from an earlier check; the memory note says "Chips read BOGI". Confirm that all three boards were checked.
