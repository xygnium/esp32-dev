# Ambient wiring

Wiring for the ambient logger: the ESP32 board (the warm box), the ADS1115 box, and the SHT45 and thermistor at the sensor end. The `i2c-scan`, `rtc-probe` and `sd-probe` projects check the wiring stage by stage (see [Verifying the wiring](#verifying-the-wiring)). Firmware sampling and WiFi decisions are in [FIRMWARE.md](FIRMWARE.md).

Contents: [Overview](#overview) · [Power](#power) · [Warm box](#warm-box-esp32-perfboard) · [CAT5 cables](#cat5-cables) · [ADS1115 box](#ads1115-box) · [Sensor end](#sensor-end) · [Verifying the wiring](#verifying-the-wiring) · [Open items](#open-items) · [Conditions to monitor](#conditions-to-monitor) · [Design notes](#design-notes)

## Overview

The apparatus sits under an open-air shed roof, which stays dry in heavy rain. It measures the ambient air outside the shed, and nothing is ever in direct sun.

```
[warm box] --CAT5 #1, 2 ft-------- [ADS1115 box] --CAT5 #2, 6-7 ft-------- [sensor end]
 ESP32, DS3231, BMP388,             vented, beside the                       SHT45 + thermistor
 SD card, 3.3 V regulator           warm box, 2 RJ45 jacks                   in a hood, lowest,
 (above or level with                                                        no electronics
  the sensors)
```

- **Warm box:** the ESP32 perfboard. It is the largest heat source, roughly half a watt with WiFi.
- **ADS1115 box:** the ADS1115, the 10k divider resistor and the input filters, kept out of the warm box's heat and away from WiFi and SD current bursts.
- **Sensor end:** the SHT45 and the thermistor side by side at the same height, about 3 ft from the warm box, well below the roof and away from walls and the ground.
- **Warm air rises,** so both boxes go above or level with the sensors, never below them.

### ESP32 boards

**Table 1: ESP32 boards**

| Board | Used for |
|---|---|
| 1 dot | Breadboard rig |
| 2 dots | Perfboard (the wiring in this document) |

### ESP32 pins in use

**Table 2: ESP32 pins in use**

| GPIO | Use | Where |
|---|---|---|
| 21 | I2C bus 0 SDA | warm box, 3-pin header |
| 22 | I2C bus 0 SCL | warm box, 3-pin header |
| 25 | Thermistor energize | 3-pin header |
| 18 | SD CLK | SD board |
| 19 | SD MISO | SD board |
| 23 | SD MOSI | SD board |
| 4 | SD CS | SD board |
| 32, 33 | I2C bus 1 SDA, SCL | nothing connected, not on the headers |
| 34 | Supply voltage sense (`ambient`) | |

Bus 0 runs at 100 kHz. The breakout boards' own pull-ups carry the bus; the ESP32's internal pull-ups are also enabled (from `i2c-scan/main/main.c`), but they are too weak to run the cables on their own.

### Devices on bus 0

**Table 3: Devices on bus 0**

| Device | Address | Location | Status |
|---|---|---|---|
| SHT45 | 0x44 | sensor end | answers through CAT5 #1 and #2; serial number 0x16440c7f (SHT4x) |
| ADS1115 | 0x48 | ADS1115 box | answers through CAT5 #1 |
| DS3231 memory chip | 0x57 | warm box | verified |
| DS3231 | 0x68 | warm box | verified, including the backup battery |
| BMP388 | 0x76 | warm box | verified, chip id 0x50 |

No addresses conflict. The ADS1115 goes missing from a scan if CAT5 #1 is faulty, and the SHT45 if CAT5 #1 or #2 is faulty.

### Warm box wire colors

The board's own wires (not the CAT5 conductors):

**Table 4: Warm box wire colors**

| Signal | Color |
|---|---|
| SDA | orange |
| SCL | brown |
| 3.3 V | green |
| GND | black |
| Thermistor energize | yellow |
| SD CLK | blue |
| SD MOSI | purple |
| SD MISO | orange (same as SDA) |
| SD CS | yellow (same as energize) |

There are no more colors, so orange and yellow are reused. 30 AWG wire is too thin for tape marks, so rely on Tables 4 and 6-8 and visual inspection, and check continuity before powering up.

## Power

**Table 5: Power sources**

| Mode | Source |
|---|---|
| Development | USB |
| Production | 2.5 mm barrel jack |

- **5 V bus:** joins the ESP32's VIN pin, the barrel jack's 5 V and the 3.3 V regulator's input. On USB, the ESP32 board's USB 5 V reaches the bus through VIN. The board has no diode between USB 5 V and VIN (checked), so the bus gets the full USB voltage. In production the jack feeds the bus, and so VIN. The regulator runs in both modes, so the 3.3 V bus comes up and goes down with the ESP32.
- **3.3 V bus:** the regulator's output. It feeds every I2C device, the SD board and the 2-pin header. It is not connected to the ESP32's 3V3 pin. The regulator's ground is common with the ESP32's.
- **One source at a time.** The user is responsible for never connecting USB and the barrel jack together. With no diode, the jack would feed back into the USB port.

## Warm box (ESP32 perfboard)

### DS3231

**Table 6: DS3231 connections**

| Board pin | Goes to | Wire color |
|---|---|---|
| VCC | 3.3 V bus | green |
| GND | GND | black |
| SCL | SCL (D22) | brown |
| SDA | SDA (D21) | orange |
| SQW | not connected (unused) | |
| 32K | not connected (unused) | |

- The board's charging resistor has been removed, so a non-rechargeable backup battery (such as a CR2032) can be fitted safely.
- SQW is the square-wave or programmable interrupt output (1 Hz, or an alarm interrupt). 32K is the 32.768 kHz temperature-compensated oscillator output. Both are open-drain, so they need an external pull-up to be used. Left unconnected they do nothing and cause no harm.
- If SQW is ever used, for example as an alarm that wakes the ESP32, add a pull-up to 3.3 V and pick a GPIO that can wake the chip.

### BMP388 (I2C mode)

**Table 7: BMP388 connections**

| Board pin | Goes to | Wire color |
|---|---|---|
| VIN | 3.3 V bus | green |
| GND | GND | black |
| SCK | SCL (D22) | brown |
| SDI | SDA (D21) | orange |
| SDO | GND (gives address 0x76) | black |
| CS | 3.3 V bus (forces I2C mode; the board has no pull-up of its own) | green |
| INT | not connected | |
| 3V0 | not connected | |

### SD card (microSD board, SPI mode)

**Table 8: SD card connections**

| Board pin | Function | Goes to | Wire color |
|---|---|---|---|
| 3V3 | power | 3.3 V bus | green |
| GND | ground | GND | black |
| CLK | SCK | D18 (GPIO18) | blue |
| MISO | MISO | D19 (GPIO19) | orange |
| MOSI | MOSI | D23 (GPIO23) | purple |
| CS | CS | D4 (GPIO4) | yellow |

- The board has no 5 V to 3.3 V regulator. It runs from the 3.3 V bus only; never connect it to 5 V.
- These are the ESP32's standard VSPI pins, with CS moved off GPIO5 because GPIO5 is a boot-strapping pin.
- The board has its own pull-ups: four parts marked 103 (10 kΩ) and two brown capacitors of unknown value. It needs nothing added. The same regulator and SD board run together in two Pico projects with no problems so far.
- If the card fails to initialise, check the supply first, then the wiring against Table 8.

### Headers

Two headers, which are screw terminal blocks, carry bus 0, 3.3 V and the thermistor energize lead off the board to CAT5 #1 (Tables 9 and 10; the "Warm box end" column of Table 11 refers to them). Pins are marked with dots, counted from 1 on each header.

2-pin header (power), sitting directly on the rails with no wires:

**Table 9: 2-pin header (power)**

| Pin | Signal |
|---|---|
| 1 dot | 3.3 V (from the regulator) |
| 2 dots | GND |

3-pin header (signals):

**Table 10: 3-pin header (signals)**

| Pin | Signal | Goes to |
|---|---|---|
| 1 dot | SDA | D21 |
| 2 dots | SCL | D22 |
| 3 dots | Thermistor energize | D25 (GPIO25) |

GPIO25 was chosen because it is output-capable, is not a boot-strapping or flash pin, and no project in this repo uses it.

## CAT5 cables

### Common to CAT5 #1 and #2

The pin assignments in Tables 11 and 12 (CAT5 #1 and #2) are confirmed.

- **Cable:** bulk solid-core CAT5 (the cable on hand), cut to length. Solid core is stiff and can fatigue if flexed repeatedly. That risk is accepted.
- **Plugs:** both cables have a plug only at the ADS1115 box end. Crimp RJ45 plugs rated for solid core, since ordinary stranded-wire plugs grip solid wire poorly. Test all 8 conductors with a continuity tester or cable tester before connecting anything. Don't unplug and replug often.
- **Pairing:** each signal is twisted with a ground (or, for the thermistor, with its own return), so SDA, SCL and the analog lines are shielded from each other. The pair colors match the warm box wires (SCL brown, SDA orange, 3.3 V green).
- **Pin order:** deliberately not T568A/B, because these are not Ethernet cables. RJ45 pins 1-8 are green, green-striped, orange, orange-striped, brown, brown-striped, blue, blue-striped, so each signal wire sits next to its striped ground on adjacent pins (1-2 3.3 V, 3-4 SDA, 5-6 SCL, 7-8 thermistor). Use the same order at both ends of each cable and in both CAT5 #1 and #2. Keystone jacks carry T568A/B color labels: ignore them and wire by pin number.
- **Jacks and holes:** jacks face down. Seal each cable hole with a grommet or sealant, since the boxes are vented for air, not water. Label the jacks "NOT ETHERNET": they carry 3.3 V, and plugging one into a network port (especially PoE) can damage a part.
- **Drip loops:** where a cable leaves the bottom of a box, it hangs in a U at least 3-4 in deep, with its lowest point below the hole and a gentle radius, so water drips off before it reaches a jack or hole.
- **Capacitance:** about 50 pF per metre. CAT5 #1 (about 30 pF) and #2 (about 100 pF) total about 130 pF before the boards' own, well under the 400 pF I2C limit, so the 100 kHz clock stays.
- **Strain relief:** on both cables at every box.

### CAT5 #1: warm box to ADS1115 box (jack 1)

**Table 11: CAT5 #1 pin assignments**

| RJ45 pin | Wire | Signal | Warm box end (headers) | ADS1115 box end |
|---|---|---|---|---|
| 1 | green | 3.3 V | 2-pin header, 1 dot | 3.3 V wire: ADS1115 VDD, jack 2 pin 1 |
| 2 | green-striped | GND | 2-pin header, 2 dots | ground point (with ADS1115 GND) |
| 3 | orange | SDA | 3-pin header, 1 dot | SDA wire: ADS1115 SDA, jack 2 pin 3 |
| 4 | orange-striped | GND | 2-pin header, 2 dots | ground point (with ADS1115 GND) |
| 5 | brown | SCL | 3-pin header, 2 dots | SCL wire: ADS1115 SCL, jack 2 pin 5 |
| 6 | brown-striped | GND | 2-pin header, 2 dots | ground point (with ADS1115 GND) |
| 7 | blue | Thermistor energize | 3-pin header, 3 dots | energize node: 10k to the tap node, A0 divider to ADS1115 A0 |
| 8 | blue-striped | GND | 2-pin header, 2 dots | ground point (with ADS1115 GND) |

- **Length:** 2 ft (confirmed). It leaves the bottom of the warm box and plugs into jack 1 on the bottom of the ADS1115 box. The U takes about 11-14 in (4 in down, 4 in back up, 3-6 in across), plus the plug bends and the stiffness of solid core.
- **Warm box end:** no plug or jack. The solid-core conductors clamp straight into the 2-pin and 3-pin screw terminal blocks. The four striped GND wires are twisted and soldered together, with a single wire leaving the joint into the 2-dot (GND) terminal. The soldered joint is covered with heat shrink.

### CAT5 #2: ADS1115 box (jack 2) to sensor end

**Table 12: CAT5 #2 pin assignments**

| RJ45 pin | Wire | Signal | ADS1115 box end | Sensor end |
|---|---|---|---|---|
| 1 | green | 3.3 V | 3.3 V wire: ADS1115 VDD, jack 1 pin 1 | SHT45 VIN |
| 2 | green-striped | GND (SHT45) | ground point (with ADS1115 GND) | SHT45 GND |
| 3 | orange | SDA | SDA wire: ADS1115 SDA, jack 1 pin 3 | SHT45 SDA |
| 4 | orange-striped | GND (SHT45) | ground point (with ADS1115 GND) | SHT45 GND |
| 5 | brown | SCL | SCL wire: ADS1115 SCL, jack 1 pin 5 | SHT45 SCL |
| 6 | brown-striped | GND (SHT45) | ground point (with ADS1115 GND) | SHT45 GND |
| 7 | blue | Thermistor divider tap | tap node: 10k to the energize node, 1 kΩ to ADS1115 A1 | thermistor lead 1 |
| 8 | blue-striped | Thermistor ground return | ground point (with ADS1115 GND), directly | thermistor lead 2 |

- **Length:** 6-7 ft (confirmed). The run is a drip loop at the box (about 11-14 in), about 3 ft across, 1-2 ft of drop to the sensors, and about 6 in of slack inside the hood. Plug trimming and soldering use some length.
- **Ends:** a solid-rated plug at jack 2; soldered to the sensor-end perfboard, with no plug.
- **Thermistor pair:** a small analog signal on the longer run. It stays in its own twisted pair, with its filter at the ADS1115 end. Build step 3 checks the noise.

### Jack terminals (ADS1115 box)

The two keystone jacks come from different makers. Wire both by RJ45 pin number, using the pin order in Tables 11 and 12, whatever the terminal colors say.

Jack 1 (CAT5 #1) is numbered and color-coded A/B. Ignore the colors and use the terminal number (Table 13, from Table 11):

**Table 13: Jack 1 terminals (numbered, CAT5 #1)**

| Terminal | Cable wire | Signal | Goes to inside the box |
|---|---|---|---|
| 1 | green | 3.3 V | 3.3 V wire (ADS1115 VDD, jack 2 pin 1) |
| 2 | green-striped | GND | ground point |
| 3 | orange | SDA | SDA wire (ADS1115 SDA, jack 2 pin 3) |
| 4 | orange-striped | GND | ground point |
| 5 | brown | SCL | SCL wire (ADS1115 SCL, jack 2 pin 5) |
| 6 | brown-striped | GND | ground point |
| 7 | blue | Thermistor energize | energize node (10k, and the A0 divider) |
| 8 | blue-striped | GND | ground point |

Jack 2 (CAT5 #2) is color-coded only. Use the column that matches its colors, A or B (Table 14, from Table 12):

**Table 14: Jack 2 terminals (color-only, CAT5 #2)**

| Pin | Cable wire | Signal | Terminal if A-colored | Terminal if B-colored | Goes to inside the box |
|---|---|---|---|---|---|
| 1 | green | 3.3 V | white/green | white/orange | 3.3 V wire |
| 2 | green-striped | GND (SHT45) | green | orange | ground point |
| 3 | orange | SDA | white/orange | white/green | SDA wire |
| 4 | orange-striped | GND (SHT45) | blue | blue | ground point |
| 5 | brown | SCL | white/blue | white/blue | SCL wire |
| 6 | brown-striped | GND (SHT45) | orange | green | ground point |
| 7 | blue | Thermistor divider tap | white/brown | white/brown | tap node (10k, and 1 kΩ to A1) |
| 8 | blue-striped | Thermistor ground return | brown | brown | ground point, directly |

- **Colors don't match across sockets:** cable green lands on a white/green terminal on an A-colored jack and a white/orange one on a B-colored jack, and every striped GND wire lands on a different color. Check continuity from plug to terminal for all 8 pins.
- **Pins 4, 5, 7 and 8** are the same in A and B.

## ADS1115 box

A plastic switch box with a keystone cover plate, mounted open face down beside the warm box (proposed). The plate holds two RJ45 jacks facing down: jack 1 for CAT5 #1 and jack 2 for CAT5 #2. Vent at the top, behind fine screen against insects.

Everything is wired on a perfboard inside. The ADS1115 breakout plugs into female headers; the divider resistor, filters and both jacks are wired to the same perfboard. The SDA, SCL and 3.3 V wires run from jack 1 through the ADS1115 to jack 2.

### How the thermistor is read

From `ads1115-dev/DESIGN.md`:

- Ratiometric reading with pulsed excitation: GPIO25 drives the divider from about 20 ms before the first conversion until the last one ends (see FIRMWARE.md, Table 1).
- A0 reads half the excitation voltage and A1 reads the divider tap. The thermistor value comes from the ratio A1 ÷ (2 × A0), so ADC reference and supply errors cancel.
- A0 reads through a divider of two equal 10k resistors (0.1%, the same part as the divider's 10k) because GPIO25's high level is the ESP32 board's own 3.3 V, which moves with the ESP32's load and can sit above the ADS1115's supply (the separate 3.3 V bus). At half the excitation, A0 always stays well inside the ADS1115's input range.
- Inputs are single-ended, with the same gain setting on every input. One chip takes the excitation monitor plus up to 3 thermistors.

```
GPIO25 energize ──┬── 10k ───┬── A0   (100 nF from the A0 pin to the ground point)
 (CAT5 #1 blue)   │          │
                  │          └── 10k ── ground point
                  │
                  └── 10k ───┬── tap ── 1 kΩ ──┬── A1   (100 nF from the A1 pin to the ground point)
                             │
                             └── CAT5 #2 blue ── thermistor ── CAT5 #2 blue-striped ── ground point
```

### ADS1115 board pins

Pin names are typical for the Lonely Binary board and not verified against it. Read them off the board before soldering.

**Table 15: ADS1115 board pins**

| Board pin | Goes to | Wire color | Notes |
|---|---|---|---|
| VDD | 3.3 V wire | green | |
| GND | ground point | black | |
| SCL | SCL wire | brown | |
| SDA | SDA wire | orange | |
| ADDR | not connected | | pulled low on the board, which gives 0x48 |
| ALRT | not connected | | |
| A0 | midpoint of the A0 divider (10k from the energize node, 10k to the ground point), 100 nF to the ground point | none (component leads) | excitation monitor, reads half the excitation |
| A1 | 1 kΩ from the tap node, 100 nF to the ground point | none (component leads) | thermistor voltage |
| A2, A3 | not connected | | spare, for more thermistors |

### Divider and filter parts

**Table 16: Divider and filter parts**

| Part | From | To | Wire color |
|---|---|---|---|
| 10k resistor (0.1%, ≤15 ppm/°C) | energize node | tap node | |
| 10k resistor (0.1%, ≤15 ppm/°C), A0 divider top | energize node | A0 | |
| 10k resistor (0.1%, ≤15 ppm/°C), A0 divider bottom | A0 | ground point | |
| 1 kΩ (1% is fine) | tap node | A1 | |
| 100 nF | A0 | ground point | |
| 100 nF | A1 | ground point | |
| CAT5 #1 wire, pin 7 (jack 1 terminal 7) | jack 1 | energize node | blue |
| CAT5 #2 wire, pin 7 (jack 2 pin 7) | jack 2 | tap node | blue |
| CAT5 #2 wire, pin 8 (thermistor return) | jack 2 | ground point | blue-striped |
| ADS1115 GND wire | ADS1115 GND | ground point | black |

- The 10k's tolerance now counts as well as its stability, because the thermistor is not calibrated (0.1% is about 0.02 °C).
- The A0 divider's ratio enters the result directly, so its two resistors need the same precision. Two 0.1% parts keep the ratio within about 0.1%, which is about 0.05 °C at 25 °C, 0.08 °C at 0 °C and 0.18 °C at −20 °C. Matched parts, or measuring the ratio once on the bench, would do better.
- The divider draws about 0.17 mA from GPIO25 while the excitation is on.
- A0 sits at 5 kΩ, so the ADC input loading lowers it by about 0.1%. Firmware corrects it the same way as A1's loading (see Thermistor), using the same input-impedance figure.
- `DESIGN.md` assumes the board has no input filters of its own. Check yours, since onboard parts would add to these.

### Ground point

One node on the perfboard. All eight striped wires (four from each cable) and the ADS1115's GND pin join there. The thermistor's return (CAT5 #2 blue-striped) reaches it directly, not through the SHT45's ground wires, so SHT45 current never flows in the thermistor's return. This is the only place the two grounds are joined.

## Sensor end

A small perfboard on the end of CAT5 #2, inside the hood. The SHT45 breakout plugs into a 5-pin female header, and the thermistor's two wires solder to pads on the same perfboard. No electronics other than the sensors are there.

### SHT45 (Adafruit 6174, PTFE membrane)

Pin names: VIN, 3Vo, GND, SCL, SDA. The order along the board is from memory and not verified, so read it off the board before soldering.

**Table 17: SHT45 connections**

| Breakout pin | CAT5 #2 wire | Signal |
|---|---|---|
| VIN | green | 3.3 V |
| GND | orange-striped, green-striped and brown-striped, joined | GND |
| SCL | brown | SCL |
| SDA | orange | SDA |
| 3Vo | not connected | |

- The header is unkeyed. Mark pin 1 on the perfboard and the breakout, and check polarity with a meter before the first power-up: a reversed SHT45 puts 3.3 V on ground.
- The board's power LED was cut off with a knife, to remove its heat.

### Thermistor

**Table 18: Thermistor pads**

| Pad | CAT5 #2 wire | Signal |
|---|---|---|
| Lead 1 | blue | divider tap |
| Lead 2 | blue-striped | thermistor ground return, not joined to the SHT45 GND here |

Littelfuse 103JL1A, a glass-encapsulated DO-35 NTC (JL Series datasheet, Feb 2018). It is not calibrated: the Littelfuse curve is used as is.

- 10,000 Ω at 25 °C, curve J, beta 3892 K (0-50 °C), −4.4 %/°C at 25 °C.
- Accuracy ±0.5 °C over 0 to +100 °C (an interchangeable part). Nothing is specified below 0 °C.
- Dissipation constant 2 mW/°C. The thermal time constant is 5 s in still air. With one A0, A1, A0 set per reading (about 0.4 s of excitation at 8 SPS), self-heating is about 0.006 °C, where continuous excitation would give about 0.14 °C.
- Hermetically sealed glass body, about 0.16 x 0.075 in, rated −55 to +300 °C.
- Leads: 24 AWG (0.020 in) tinned copper-clad steel, at least 1.0 in long.

Conversion in firmware, resistance to temperature: Steinhart-Hart, `1/T = A + B·ln(R) + C·(ln R)³` (T in kelvin, R in ohms), with the generic 10k NTC coefficients A = 1.129148e-3, B = 2.34125e-4, C = 8.76741e-8. Against the R-T table in `/home/mike/sandbox/thermistor-103JL1A/10k-2.A.txt` (which agrees with the datasheet's R25, beta and temperature coefficient), this set is within about 0.06 °C from −30 to 100 °C, far inside the part's ±0.5 °C. Beta alone is not enough: it is off by 0.2 to 0.3 °C at 0 and 50 °C and about 2.4 °C at 100 °C. Correct the tap voltage for the ADC input loading before converting, with the gain held fixed. The loading acts like a leak of about 4-6 MΩ across the thermistor. The TI datasheet gives 6 MΩ common-mode and 15 MΩ differential input impedance at ±4.096 V; with A1 measured against ground the two may act in parallel, which gives about 4.3 MΩ (not checked against the datasheet's input model). At 4.3 MΩ the error is about 0.23% of resistance (0.05 °C) at 25 °C, 0.75% (0.15 °C) at 0 °C and 2.2% (0.39 °C) at −20 °C, so the correction depends on temperature.

Outdoors, the leads are copper-clad steel, so cut ends or damaged tinning can rust. Fog or condensation can also leave a thin water film that leaks current between conductors. The thermistor is one place of many: see [Moisture leakage](#moisture-leakage) for all of them. The conformal coating on the thermistor circuit seals the cut ends and the joints.

### Mounting

- Fix the perfboard rigidly (standoffs or a small plate), and tie or glue CAT5 #2 to it, so the solder joints never flex.
- The SHT45 and thermistor sit side by side at the same height in the lower part of the hood, a few cm above the rim and away from the walls. The SHT45 faces down or sideways, never up.
- Secure the breakout against loosening, for example with a dab of hot glue.

### Hood (proposed)

The sensors hang in free air under a small cover that keeps off drips from above (roof condensation, droppings), splashes and blown mist. The shed roof stays dry in heavy rain, so a single cover is enough.

- **Candidates:** a single inverted plastic bowl about 20-25 cm across (the wide opening gives the best airflow), or a wide-mouth plastic jar (ice cream or mayonnaise) hung mouth down. No glass.
- **Vents:** a jar has only one opening, so drill 4-6 holes (about 6 mm) near the top, behind fine screen if insects are a problem. Without them the air stagnates and the readings lag.
- **Cable entry:** CAT5 #2 passes through a hole in the closed end and is sealed with silicone. Many food containers are polypropylene, which adhesives grip poorly: roughen the plastic and test the silicone on a scrap first. Water can run down the jacket from above, so fit a collar or wraps of tape above the entry.
- **Hanger:** a separate eye bolt or cord to the beam, so CAT5 #2 never takes the load.
- **Inside:** remove paper labels and glue residue.
- **Insects:** a fine mesh ring over the opening is the fix if they move in, but it slows the air, so try without it first.
- **Test:** run the SHT45 under the hood and a bare sensor beside it for a day, and compare.

## Verifying the wiring

Each check is a small diagnostic project beside `ambient` in `esp32-dev`. In its directory, run `./build.sh`, `./flash.sh` (port `/dev/ttyUSB0`) and `./monitor.sh`. The monitor resets the board when it opens and logs to `build/monitor.log`.

**Table 19: Verification projects**

| Project | Checks | A pass looks like |
|---|---|---|
| `i2c-scan` | Both I2C buses: lines held low at idle, and every address that answers. Reads the BMP388 chip id and the SHT4x serial number. Repeats until reset. | Bus 0 idle high, with the addresses in Table 3 ([Devices on bus 0](#devices-on-bus-0)) that are wired so far. Bus 1 has nothing. |
| `sensor-probe` | The BMP388 and SHT45 deliver readings: chip id (a BMP390 is also accepted), SHT45 checksums, and temperature, pressure and humidity inside each part's operating range. Prints a `PASS` or `FAIL` per sensor and the BMP388 minus SHT45 temperature difference. Repeats until reset. | `PASS` for both, with plausible values. Accuracy needs a reference thermometer, barometer or hygrometer. |
| `rtc-probe` | The DS3231: valid time, oscillator-stop flag clear, seconds advancing, chip temperature. | `PASS`. Set the clock once first (`SET_FROM_BUILD` 1, flash, then back to 0 and reflash). The clock is kept in UTC. |
| `sd-probe` | The SD card: mounts it (never formats), writes, reads back and deletes a file, reports size and free space. | `PASS`. A `gpio: conflict found for GPIO[4]` warning after the unmount is harmless. |

Further wiring is suspended until `sensor-probe` confirms the BMP388 and SHT45 readings. First run (pass 3 to 6, CAT5 #1 and #2 in use): BMP388 24.95 C and 1021.4 hPa, SHT45 24.33 C and 44.3 %RH, both `PASS`, BMP388 about 0.6 C above the SHT45. That is expected: the BMP388's temperature is the warm box's temperature, not the air's, so it isn't meant to match the SHT45. The BMP388 is there for pressure. Not yet compared with a reference instrument.

Results so far: `i2c-scan` (pass 11) shows all five bus 0 addresses, with CAT5 #1 and #2 in use (0x44, 0x48, 0x57, 0x68, 0x76), with the SHT4x serial number 0x16440c7f and BMP388 chip id 0x50, idle levels high on both buses and nothing on bus 1 (earlier, with nothing connected, it showed only 0x57, 0x68 and 0x76); `rtc-probe` passed, including a power-off test on the backup battery; `sd-probe` passed with a 120 MB card at 4 MHz.

### Build order for the rest

Each step adds one unknown, and the scan checks it:

1. CAT5 #1 and the ADS1115: expect 0x48. Done (scan pass 11).
2. CAT5 #2 and the SHT45: expect 0x44 and a serial number. Done (scan pass 11).
3. A fixed precision resistor in place of the thermistor: check the noise. This is the baseline for later.
4. The thermistor, with its leads and joints sealed.

Each breakout adds its own pull-ups, so check the idle levels on the scan after each step.

### Reading the scan output

- `idle levels: SDA low` or `SCL low`: a short, a miswired pin, or a device holding the line. The scan skips that bus. Unplug devices one at a time to find the cause.
- A BMP388 address that answers but isn't a BMP388 (register 0 isn't 0x50) prints "not a BMP388".

## Open items

- **Mounting:** the ADS1115 box (switch box and keystone plate) is proposed, not built. Mounting heights are not decided.
- **Sensor hood:** bowl or jar, vents, the silicone seal and the hanger are proposed, not built.
- **ADS1115 board:** read the pin names off the board, and check whether it has its own series resistors or capacitors on A0 to A3.
- **SHT45 pin order:** read it off the Adafruit 6174 before soldering.
- **Firmware for GPIO25:** no code drives the energize lead yet.
- **Merging the boxes:** decided by the bench noise test in "Why the ADS1115 box is separate".

## Conditions to monitor

Concerns accepted for the prototype. Check these after the first weeks outdoors, and again each season.

- **Scan health:** run the scan periodically. A missing 0x44 or 0x48, or an idle line no longer high, points at a cable, plug, jack or header contact.
- **Contacts:** look for green or white corrosion on the SHT45 header and the jacks, and for intermittent readings. Remedy: see [Contact protection](#contact-protection).
- **Solid-core cable and solder joints:** look for cracks at the plug ends and the perfboard, or readings that jump when a cable is touched. Remedy: more strain relief, or stranded cable at the sensor end.
- **Hood:** check the silicone around the CAT5 #2 entry (especially on polypropylene) and look for water tracks below it. Look for water on the membrane, the perfboard or the jacks; remedy with a second bowl, a skirt or a collar above the entry.
- **Hood lag:** compare against a bare sensor for a day after any change. Humidity that stays high long after rain suggests too few vent holes.
- **Insects and nests:** in the hood, the vent screens and the ADS1115 box.
- **Thermistor protection:** check the bag or coating for pinholes, and the leads and joints for moisture or rust.
- **Moisture leakage:** after fog or dew, compare the thermistor temperature with the SHT45's. An offset that appears with high humidity and fades as it dries points at a leak (Table 20). I2C leaks show up in the scan instead.
- **Drift:** compare the SHT45 and the thermistor with a reference occasionally. A slow offset can mean a contaminated membrane or heater effects.
- **Noise:** compare noisy thermistor readings against the fixed-resistor baseline from build step 3.

## Design notes

Figures are estimates from typical datasheet values and general reasoning, not measurements.

### Heat and resolution

- The SHT45 resolves about 0.01 °C, and the uncalibrated thermistor is limited to its ±0.5 °C tolerance (an SHT45 comparison can improve it). Effects of thousandths of a degree can be ignored; only about 0.05 °C or more at the sensor matters.
- The SHT45 breakout's power LED (a few mW) was probably the biggest steady heat source near the sensors, so it was removed. Adafruit's documentation doesn't confirm the LED, so it was checked on the board itself. The ADS1115 board still has its LED.
- The ADS1115 (about 0.5 mW converting), the divider (about 0.5 mW, pulsed) and the SHT45 itself are small.

### Why the ADS1115 box is separate

A precaution, not a known problem. It keeps the 10k resistor (about 15 ppm/°C) out of the warm box's heat, and the analog inputs away from WiFi and SD current bursts. Venting helps the resistor's stability. If the fixed-resistor noise test (build step 3), logged in place, shows no wider spread than on the bench, the two boxes can be merged.

The boxes sit outdoors 6-12 in apart. Reasoning so far (estimates, not measurements):

- **Heat is a weak reason.** The warm box dissipates about 0.5 W in open air, and warm air rises rather than moving sideways. Even if the ADS1115 box ran 5 °C warm, the 10k would shift 75 ppm, against the thermistor's 44,000 ppm/°C (−4.4 %/°C), which is about 0.002 °C. That is far below the thermistor's ±0.5 °C tolerance.
- **Noise is the real question.** The concern is the ESP32's own transmit and SD current bursts, not ambient WiFi. CAT5 #1 carries 3.3 V and ground to the ADS1115 box, so supply ripple reaches it whatever the distance; separation only helps with radiated pickup.
- **Bench test, to retire the concern:** use the fixed precision resistor in place of the thermistor, with the same gain, sample count, CAT5 #1 length and shared supply as the real install. Compare the spread (standard deviation and min-to-max) with WiFi idle against heavy WiFi plus SD writes. If the spread does not change, the box can be merged and CAT5 #1 and jack 1 dropped.
- **If noise shows up:** first add bypass capacitors at the ADS1115 VDD (10 µF plus 100 nF). A cheap shielding test is foil wrapped around the box, grounded by one wire to the ground point only, with the same measurement off, on and off again. A permanent shield would need shielded CAT5 and metal-bodied jacks to be effective. Do not shield the ESP32, since that would block its WiFi.
- **If bypass caps are not enough:** feed the ADS1115 through a ferrite bead or an LC filter, or from its own LDO. Differential inputs would cancel common-mode pickup, but this design uses single-ended inputs and spends A0 on the excitation monitor, so they are not planned. Sampling and WiFi timing are in [FIRMWARE.md](FIRMWARE.md).

### Moisture leakage

A thin water film from fog or condensation can carry a small current between any two conductors it bridges, on bare leads, solder joints, screw terminals, jack contacts, header pins, perfboard pads and the damp surface of a cable end. The wires and terminals throughout the system are all exposed to it, but the effect differs a lot by node: a low-impedance node shrugs off a megohm leak, while a high-impedance one does not. Leakage films are usually megohms on clean surfaces and can fall to tens of kilohms with dirt or salt. The numbers below are first-order estimates, not measurements.

**Table 20: Where moisture leakage matters**

| Where | What a film bridges | Effect | How it shows |
|---|---|---|---|
| Thermistor leads, joints and pads (sensor end) | The two leads, across the thermistor | Parallel resistance lowers the NTC's resistance, so the reading is warmer. About R/R_leak: at 25 °C (10 kΩ, −4.4 %/°C) 1 MΩ gives 1% and +0.23 °C and 10 MΩ gives +0.02 °C; at 0 °C (about 33 kΩ) 1 MΩ gives 3% and about +0.6 °C; at −20 °C (about 97 kΩ) 1 MΩ gives 9% and about +1.6 °C. A dirty film of 100 kΩ gives about +2.2 °C even at 25 °C | Silent: the reading stays smooth and plausible |
| Divider tap (CAT5 #2 pin 7, jack 2, and in the ADS1115 box the 1 kΩ, the 100 nF and the A1 pin) | The tap and a neighbor: GND or the thermistor return (warmer); 3.3 V, SDA, SCL or the energize node (colder) | A leak to GND or the thermistor return is the same as a leak across the thermistor (row above). A leak to a neighbor at a high voltage shifts the reading by about −0.2 °C per 1 MΩ at any temperature from −20 to 40 °C, so in the cold it is smaller than a GND leak (−0.19 °C against +0.63 °C at 0 °C). At jack 2, pin 7's only neighbors are pins 6 and 8, both ground, so a leak there reads warmer | Silent |
| Energize node and lead (GPIO25, CAT5 #1 pin 7) | GND or a neighbor | A0 reads the energize node through its divider, so a steady leak cancels in the ratio, as long as the leak current stays within what the GPIO can drive. A leak at A0 itself is different: A0 sits at 5 kΩ, so a 1 MΩ leak to GND lowers it by 0.5% and the reading goes colder by about 0.2 °C at 25 °C, as sensitive as the tap | Silent at A0, otherwise mostly invisible |
| SDA and SCL (everywhere) | GND, 3.3 V or each other | The breakout boards' pull-ups hold the lines firmly, so only a strong leak disturbs the bus | Loud: missing 0x44 or 0x48 in the scan, `idle levels ... LOW`, or read errors in `sensor-probe` |
| 3.3 V to GND | The supply and ground wires | Only a few µA at 1 MΩ; harmless unless it drops to about 1 kΩ | Rarely seen |
| DS3231 backup battery contacts (warm box) | The cell to GND | A steady drain on a cell that supplies about 1 µA, so it shortens the battery's life | Clock resets, or the oscillator-stop flag is set, after a power-off |
| SHT45 and BMP388 breakout pins | Neighboring pins | Comm errors if severe. The SHT45 is humidity-rated, but its header is the wettest contact (see Contact protection) | `sensor-probe` fails or the values jump |

- **The silent rows are the real risk.** I2C and supply faults announce themselves; a thermistor leak just moves the temperature. A clean film at room temperature moves it by tenths of a degree; a dirty film, or the same film in cold weather, moves it by degrees. The thermistor circuit is the most sensitive node, from the thermistor pads at the sensor end to A0 and A1 in the ADS1115 box, so all of it needs the best protection.
- **The warm box is warmer than the air and vented, so its films dry sooner.** The ADS1115 box dissipates only about a milliwatt, so it sits at air temperature and its films dry no sooner than the sensor end's. Fog reaches the screw terminals, jack contacts and perfboard in both boxes through the vents and the cable holes.
- **Cable runs:** the conductors are inside an insulating jacket, so the cable itself is safe while the jacket is whole. Water can wick into the cut end and run down the jacket, so seal the ends and keep the drip loops.
- **Decided: conformal coating on the thermistor circuit only.** At the sensor end that is the thermistor's leads, joints and pads. In the ADS1115 box it is the divider and filter parts and the ADS1115's header pins; the ADS1115 board itself is not coated. Mask the connectors and jacks, and keep the coating off the SHT45 (see Contact protection). The I2C wiring is not coated: its faults show up in the scan. Spacing the tap and A0 pads away from 3.3 V, SDA, SCL and GND adds margin (a GND guard trace beside them is not recommended: a leak to it reads warmer, which is the larger error in the cold).
- **No test yet.** The scan sees only the loud faults. A leak on the thermistor can be seen only by comparing it with the SHT45 in fog.

### Contact protection

General guidance, untested here.

- Low-voltage signals fail through oxide films on contacts, so corrosion in damp air is the risk, not current.
- Avoid thick insulating grease (silicone dielectric grease) on unsealed header pins. A plug-in contact wipes little, so a thick film can make it intermittent.
- If protection is wanted, use a thin film of contact enhancer made for electronics (DeoxIT-type), or conformal coating after assembly with the sockets and mating pins masked.
- Never use conductive paste: it can bridge adjacent 2.54 mm pins.
- Never coat the SHT45's sensor opening or its PTFE membrane: it blocks the air and ruins the readings.
- The SHT45 header sees humidity and possible condensation under the hood. Soldering the breakout straight to the perfboard avoids that, at the cost of easy swapping. The ADS1115 box's contacts see less direct air.

### SHT45 heater

- It clears condensation that leaves humidity stuck near 100%. It stays off unless firmware sends the heater command, and none does yet.
- Pulses are 0.1 s or 1 s at 20, 110 or 200 mW. The SHT45's specifications don't hold during a pulse, so discard its readings, and the thermistor's beside it, until it has cooled.
- The datasheet caps heater-on time at 10% of the sensor's lifetime; a 1 s pulse once a minute is about 1.7%. Firmware should count heater time.
- The datasheet gives no operating life that I found. Check its drift figures if it matters.
