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
