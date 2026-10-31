# AO3401 switch test

Bench test of the thermistor excitation switch, an AO3401 P-channel MOSFET on a SOT-23-to-DIP adapter, before wiring it into the ADS1115 box. The circuit it goes into is in [../ambient/WIRING.md](../ambient/WIRING.md).

Tested 2026-10-31 on the breadboard. All tests passed. One check is still open: see [Open items](#open-items).

## Pinout

**Table 1: AO3401 pins**

| Pin | Function |
|---|---|
| 1 | Gate |
| 2 | Source |
| 3 | Drain |

Confirmed by tests 1-7. SOT-23 numbering is from the top (the marked face): pin 1 bottom left, pin 2 bottom right, pin 3 the single pin at the top. Seen from underneath, pins 1 and 2 are mirrored.

## Unpowered tests

The meter is in diode mode, with nothing else connected to the part. The gate acts as a small capacitor and holds its charge, which can leave the channel on and give false readings. So short pins 1, 2 and 3 together before each reading.

- **Tests 1-2:** the body diode conducts from drain to source only.
- **Tests 4-7:** the gate is insulated from the source and the drain. Any voltage reading or beep means a damaged gate.

**Table 2: Unpowered results**

| Test | Red | Black | Expected | Measured |
|---|---|---|---|---|
| 1 | D (3) | S (2) | about 0.5-0.7 V | 0.545 V, drifting to 0.538 V |
| 2 | S (2) | D (3) | OL | OL |
| 4 | G (1) | S (2) | OL | OL |
| 5 | S (2) | G (1) | OL | OL |
| 6 | G (1) | D (3) | OL | OL |
| 7 | D (3) | G (1) | OL | OL |

There is no test 3: the numbering skips it, and no test is missing.

## Powered tests with a jumper

Measure pin 3 to GND in DC volts. The 10k gives the drain a load: an open drain gives a meaningless reading. The jumper stands in for the GPIO pin.

```
3.3 V
  │
  ├──── 100k ────────┐
  │                  │
  │ S (pin 2)        │
  │                  │
AO3401 ── G (pin 1) ─┴── ○ jumper ○ ── GND
  │
  │ D (pin 3)
  │
  ├───── meter + (DC volts)
  │
 10k
  │
 GND ─── meter −
```

**Table 3: Jumper results**

| Test | Gate | Expected, pin 3 to GND | Result |
|---|---|---|---|
| 8 | on the pull-up only | about 0 V | pass (value not recorded) |
| 9 | jumpered to GND | about 3.3 V | pass (value not recorded) |
| 10 | jumper removed | about 0 V | pass (value not recorded) |

## Powered tests with a GPIO

Same circuit, with GPIO17 in place of the jumper. If the 3.3 V doesn't come from the ESP32's own 3V3 pin, connect the two grounds.

```
3.3 V
  │
  ├──── 100k ────────┐
  │                  │
  │ S (pin 2)        │
  │                  │
AO3401 ── G (pin 1) ─┴── GPIO17 (TX2)
  │
  │ D (pin 3)
  │
  ├───── meter + (DC volts)
  │
 10k
  │
 GND ─── meter −
```

The design uses GPIO25, but the breadboard has no free socket on that side of the ESP32. Any output pin behaves the same for this test, so `PIN_SWITCH` in `main/main.c` is set to GPIO17.

In this directory, run `./build.sh`, `./flash.sh` (port `/dev/ttyUSB0`) and `./monitor.sh`. The monitor resets the board when it opens. The firmware alternates high and low every 10 s and prints the state and the expected reading. It has no PASS or FAIL, because the ESP32 can't see the drain: the meter decides.

**Table 4: GPIO results**

| Test | GPIO17 | Measure | Expected | Measured |
|---|---|---|---|---|
| 11 | high | pin 3 to GND | about 0 V | 0.0001 V |
| 12 | high | pin 1 to pin 2 | under 0.4 V | 0.0014 V |
| 13 | low | pin 3 to GND | about 3.3 V | 3.3 V |

## Open items

- **Perfboard rails:** on the perfboard, GPIO25's high level comes from the ESP32's 3.3 V, while the AO3401's source is on the regulator's 3.3 V bus. If the two differ by more than about 0.4 V, the switch doesn't turn fully off. Test 12 reads only 1.4 mV, so the gate and source appear to share a rail here, which says nothing about the perfboard. Measure between the ESP32's 3V3 pin and the 3.3 V bus on the perfboard. No firmware is needed.
- **GPIO25:** set `PIN_SWITCH` back to `GPIO_NUM_25` to run this probe on the perfboard.
- **Boot resets:** one scripted capture of the console logged 25 power-on resets before the app started, and another read got no output. A capture with the serial reset lines released showed one normal boot. The capture method is the likely cause, but this wasn't proved.
