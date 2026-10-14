# i2c-scan wiring

Scans I2C bus 0 and bus 1, reports a line held low at idle, and identifies the BMP388 (chip id) and SHT4x (serial number). The ambient project shares this board and wiring.

## ESP32 boards

| Board | Used for |
|---|---|
| 1 dot | Breadboard rig |
| 2 dots | Perfboard (the wiring in this document) |

## ESP32 pins (from `main/main.c`)

| Bus | SDA | SCL |
|---|---|---|
| 0 | GPIO21 (D21) | GPIO22 (D22) |
| 1 | GPIO32 (D32) | GPIO33 (D33) |

The scan runs at 100 kHz with the ESP32's internal pull-ups enabled.

## Power

| Mode | Source |
|---|---|
| Development | USB |
| Production | Barrel jack |

A 5 V bus on the board joins three things:

- the ESP32's VIN pin;
- the barrel jack's 5 V;
- the 3.3 V regulator's input.

On USB, the ESP32 board's USB 5 V reaches the bus through the VIN pin. In production, the jack feeds the bus and so the ESP32's VIN. The regulator therefore runs in both modes, and the 3.3 V rail for the I2C devices comes up and goes down with the ESP32.

The regulator's output connects to a 3.3 V bus. The 3.3 V bus feeds the DS3231, the BMP388 and the 2-pin header's 1-dot pin. It is not connected to the ESP32's 3V3 pin.

Use one source at a time. The user is responsible for never connecting USB and the barrel jack together.

## On the board with the ESP32

The DS3231 and BMP388 are mounted on the same board as the ESP32, on bus 0:

| Device | Address | Bus 0 pins |
|---|---|---|
| DS3231 | 0x68 (its memory chip answers at 0x57) | SDA to D21, SCL to D22 |
| BMP388 | 0x76 (SDO tied to GND) | SDA to D21, SCL to D22 |

BMP388 board pins, in I2C mode:

| Board pin | Goes to | Wire color |
|---|---|---|
| VIN | 3.3 V bus | green |
| GND | GND | black |
| SCK | SCL (D22) | brown |
| SDI | SDA (D21) | orange |
| SDO | GND (address 0x76) | black |
| CS | 3.3 V bus (forces I2C mode; the board has no pull-up of its own) | green |
| INT | not connected | |
| 3V0 | not connected | |

Both are powered from 3.3 V, like every I2C device in this setup. That 3.3 V comes from a separate regulator, not the ESP32's 3V3 pin, and the regulator's ground is common with the ESP32's.

## Headers

Two headers extend bus 0 off the board and add the thermistor energize lead. Both headers are marked with dots, counted from 1 on each header:

2-pin header (power):

| Pin | Signal |
|---|---|
| 1 dot | 3.3 V (from the separate regulator) |
| 2 dots | GND |

3-pin header (signals):

| Pin | Signal | Goes to |
|---|---|---|
| 1 dot | SDA | D21 |
| 2 dots | SCL | D22 |
| 3 dots | Thermistor energize | GPIO25 (D25) |

Wire colors on the warm box board (the board's own wires, not the CAT5 conductors):

| Signal | Color |
|---|---|
| SDA | orange |
| SCL | brown |
| 3.3 V | green |
| GND | black |
| Thermistor energize | blue |

A CAT5 cable connects to the headers. Its far end carries:

- an **SHT45** (0x44) on SDA/SCL, powered from 3.3 V and GND;
- an **ADS1115** (0x48) on SDA/SCL, powered from 3.3 V and GND;
- the **thermistor voltage divider**, which the ADS1115 reads. The energize lead from the ESP32 powers the divider only while a reading is taken.

All of these share bus 0 with the DS3231 and BMP388, so the addresses on the bus are 0x44, 0x48, 0x57, 0x68 and 0x76, with no conflicts. The scan should list all of them, and the SHT45 and ADS1115 go missing if the cable is faulty.

CAT5 has 8 conductors in 4 twisted pairs, and the two headers carry 5 signals. Proposed assignment (for review), matching the board wires (SCL brown, SDA orange), with each signal twisted with a ground so SDA, SCL and the energize lead are each shielded from the others:

| Pair (colors) | Solid-color wire | White-striped wire |
|---|---|---|
| Brown | SCL | GND |
| Orange | SDA | GND |
| Green | 3.3 V | GND |
| Blue | Thermistor energize | GND |

All four GND wires join at the 2-pin header's 2-dot pin and at the far end's ground.

Bus 1 (D32/D33) is not on the headers.

GPIO25 was chosen because it is output-capable, is not a boot-strapping or flash pin, and no project in this repo uses it. Pins already taken in the code: 21 and 22 (bus 0), 32 and 33 (bus 1), 34 (supply voltage sense, ambient).

## ADS1115 inputs

From `ads1115-dev/DESIGN.md`:

- Ratiometric reading with pulsed excitation: the energize lead (GPIO25) drives the divider only while a conversion runs.
- The divider is: fixed resistor (10k, 0.1%, ≤15 ppm/°C), then the tap, then the thermistor, then a single-point ground.
- One input measures the excitation voltage. Each thermistor input reads its divider tap and is computed as a ratio against the excitation input. Up to 3 thermistors plus the excitation monitor per chip.
- Inputs are single-ended (against GND), with the same gain setting on every input.
- Each used input gets a 1 kΩ series resistor and a 100 nF capacitor to ground at the pin. `DESIGN.md` assumes the board has none; check yours.
- ADDR is pulled low on the board, which gives 0x48.

Input assignment (proposed; `ads1115-dev` doesn't number the inputs):

| Input | Connects to |
|---|---|
| A0 | Excitation monitor: the energize lead, through 1 kΩ with 100 nF to GND |
| A1 | Divider tap for the thermistor, through 1 kΩ with 100 nF to GND |
| A2, A3 | Spare, for more thermistors |

`DESIGN.md` suggests the ADS1115 and resistors sit away from the thermistor, with only the thermistor on its own CAT5 pair. This design puts the ADS1115 and divider at the far end of the cable with the SHT45, so the thermistor wires connect directly to the divider.

## Open items

- **Where the ADS1115 and divider sit.** Chosen: the ADS1115 and divider go in their own vented box, away from the SHT45 (`ads1115-dev/DESIGN.md` also wants them away from the thermistor). One CAT5 runs ESP32 box to SHT45, then on to the ADS1115 box, each leg 6 to 12 in. Still to settle: whether the SHT45 is a tap on one continuous cable or a cable ends there and a second starts, and where the thermistor sits (its own pair from the ADS1115 box).
- **ADS1115 input assignment.** A0 as the excitation monitor and A1 as the thermistor tap is proposed, not confirmed.
- **CAT5 conductor pairing.** The table above is proposed, not confirmed.
- **Header pin mapping.** Pins 1 to 5 as 3.3 V, GND, SDA, SCL, energize is assumed, not confirmed.
- **Input filter.** Check whether the ADS1115 board has its own series resistors or capacitors on A0 to A3.
- **Firmware for GPIO25.** No code drives the energize lead yet.

## Design notes (for later consideration)

Figures are estimates from typical datasheet values and general reasoning, not measurements.

- **Heat sources near the SHT45:**
  - The SHT45's breakout power LED (a few mW) was probably the biggest steady source. It has been removed from the Adafruit 6174 (SHT45 with PTFE) by cutting it with a knife. Other SHT45 boards, and the ADS1115 board, still have theirs unless stated. Whether the Adafruit board has an LED is unconfirmed in Adafruit's documentation, so the removal was done on the board itself.
  - The ADS1115 (about 0.5 mW converting) and the divider (about 0.5 mW, pulsed) are small.
  - The SHT45's own dissipation is negligible.
- **Resolution limits:** the SHT45 resolves about 0.01 °C, and the calibrated thermistor chain is budgeted at ±0.05 to 0.1 °C. Effects of thousandths of a degree can be ignored. Only sources of about 0.05 °C or more at the sensor matter.
- **Placement:** the SHT45 and the thermistor sit side by side at the same height, close together, so they see the same air. The ADS1115 box sits above or beside them, with its vent at the top so warm air leaves upward. Nothing is in direct sun.
- **Vented box:** venting helps the resistor's stability more than it helps the SHT45. Keep the box out of rain and damp.
- **Cable run:** ESP32 box to SHT45 to ADS1115 box, each leg 6 to 12 in, so cable capacitance and the stub at the SHT45 are not concerns.
- **Far-end connections (planned):** the SHT45 and ADS1115 breakouts plug into female pcb headers soldered to perfboards, with the CAT5 conductors soldered to the perfboard traces.
  - The SHT45 is mid-chain: the incoming and outgoing CAT5 conductors go to the same traces, with the SHT45's socket on them, so signals pass through with no tap.
  - The ADS1115 perfboard also holds the 10k divider resistor, the 1 kΩ and 100 nF input filters, and the thermistor pair's pads or header.
  - The breakouts' pin orders differ, so wire each socket to its own board's pin names.
  - Sockets are unkeyed. Mark pin 1 on the perfboard and on the breakout, and check polarity with a meter before the first power-up, since a reversed SHT45 puts 3.3 V on ground.
  - The breakout stands about 8 mm above the perfboard. Put the SHT45 at the perfboard's free edge with its sensor end pointing away from the perfboard and the ADS1115 socket.
  - Add strain relief on the cable and secure the breakouts against loosening, for example with a dab of hot glue.
  - Each breakout adds its own pull-ups to the bus. Check the idle levels on the scan after each addition.
  - Contact protection: see the note below. Both far-end boxes are vented, so their contacts see outdoor humidity.
- **Contact protection for header pins (general guidance, untested here):**
  - Low-voltage signals can fail through a film of oxide on the contact, so corrosion in damp air is the risk, not current.
  - Avoid thick insulating grease (silicone dielectric grease) on unsealed header pins. A plug-in contact wipes little, so a thick film can make the contact intermittent.
  - If protection is wanted, use a thin film of contact enhancer made for electronics (DeoxIT-type), or conformal coating after assembly, with the sockets and mating pins masked.
  - Never use conductive paste: it can bridge adjacent 2.54 mm pins.
  - The SHT45 needs outside air, so its box can't be sealed, and its socket sees humidity and possible condensation. Soldering the SHT45 to the perfboard (or to short wires) instead of using a socket avoids the contact problem, at the cost of easy swapping.
  - Never coat the SHT45's sensor opening or its PTFE membrane, since coating blocks the air and ruins the readings. Coat only the perfboard and the pins, and mask the sensor.
  - The ADS1115 box is vented too, but its contacts see less direct air, so a socket there is less of a worry.
- **SHT45 heater (outdoor use):**
  - It clears condensation that leaves humidity stuck near 100%. It stays off unless firmware sends the heater command, and none does yet.
  - Pulses are 0.1 s or 1 s at 20, 110 or 200 mW. The SHT45's specifications don't hold during a pulse, so discard its readings until it has cooled.
  - A pulse may also warm the thermistor beside it, so discard its readings for the same period.
  - The datasheet caps heater-on time at 10% of the sensor's lifetime. A 1 s pulse once a minute is about 1.7%. Firmware should count heater time.
  - The datasheet gives no operating life that I found. Check its drift figures if it matters.

## SD card (microSD board, SPI mode)

Wired and verified with `sd-probe` (a 120 MB SDSC card mounted, wrote, read back and deleted a file at 4 MHz; PASS). The pins are the ESP32's standard VSPI pins, with CS moved off GPIO5 because GPIO5 is a boot-strapping pin.

The board has pull-ups on its lines and no 5 V to 3.3 V regulator. It runs from the 3.3 V bus only; never connect it to 5 V.

| Board pin | Function | Goes to | Wire color |
|---|---|---|---|
| 3V3 | power | 3.3 V bus | green |
| GND | ground | GND | black |
| CLK | SCK | ESP32 D18 (GPIO18) | blue |
| MISO | MISO | ESP32 D19 (GPIO19) | orange |
| MOSI | MOSI | ESP32 D23 (GPIO23) | purple |
| CS | CS | ESP32 D4 (GPIO4) | yellow |

- The board already works on a Pico, so it needs no added pull-ups or capacitors. It has six small parts: four marked 103 (10 kΩ, probably pull-ups) and two brown capacitors of unknown value.
- The same regulator and SD board run together in two other projects on a Pico, with no problems so far, so build it and see.
- Wire colors were assigned in the order the board's pins were listed (CS, MOSI, CLK, MISO). Blue is also the thermistor energize color and orange is also SDA on the warm box board. The reuse is accepted because there are no more colors; 30 AWG wire is too thin for tape marks, so rely on visual inspection and this wiring table, and check continuity before powering up.
- If the card fails to initialise, check the supply first, then the wiring against the table.

## Reading the output

- `idle levels: SDA low` or `SCL low`: a short, a miswired pin, or a device holding the line. The scan skips that bus. Unplug devices one at a time to find the cause.
- A BMP388 address that answers but isn't a BMP388 (register 0 isn't 0x50) prints "not a BMP388".
