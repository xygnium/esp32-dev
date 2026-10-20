# Ambient wiring

Wiring for the ambient logger: the ESP32 board (the warm box), the ADS1115 box, and the SHT45 and thermistor at the sensor end. The `i2c-scan`, `rtc-probe` and `sd-probe` projects check the wiring stage by stage (see [Verifying the wiring](#verifying-the-wiring)).

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

| Board | Used for |
|---|---|
| 1 dot | Breadboard rig |
| 2 dots | Perfboard (the wiring in this document) |

### ESP32 pins in use

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

Bus 0 runs at 100 kHz with the ESP32's internal pull-ups enabled (from `i2c-scan/main/main.c`).

### Devices on bus 0

| Device | Address | Location | Status |
|---|---|---|---|
| SHT45 | 0x44 | sensor end | not yet wired on the perfboard |
| ADS1115 | 0x48 | ADS1115 box | not yet wired |
| DS3231 memory chip | 0x57 | warm box | verified |
| DS3231 | 0x68 | warm box | verified, including the backup battery |
| BMP388 | 0x76 | warm box | verified, chip id 0x50 |

No addresses conflict. The ADS1115 goes missing from a scan if CAT5 #1 is faulty, and the SHT45 if CAT5 #1 or #2 is faulty.

### Warm box wire colors

The board's own wires (not the CAT5 conductors):

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

There are no more colors, so orange and yellow are reused. 30 AWG wire is too thin for tape marks, so rely on these tables and visual inspection, and check continuity before powering up.

## Power

| Mode | Source |
|---|---|
| Development | USB |
| Production | 2.5 mm barrel jack |

- **5 V bus:** joins the ESP32's VIN pin, the barrel jack's 5 V and the 3.3 V regulator's input. On USB, the ESP32 board's USB 5 V reaches the bus through VIN. In production the jack feeds the bus, and so VIN. The regulator runs in both modes, so the 3.3 V bus comes up and goes down with the ESP32.
- **3.3 V bus:** the regulator's output. It feeds every I2C device, the SD board and the 2-pin header. It is not connected to the ESP32's 3V3 pin. The regulator's ground is common with the ESP32's.
- **One source at a time.** The user is responsible for never connecting USB and the barrel jack together.

## Warm box (ESP32 perfboard)

### DS3231

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
- If the card fails to initialise, check the supply first, then the wiring against the table.

### Headers

Two headers, which are screw terminal blocks, carry bus 0, 3.3 V and the thermistor energize lead off the board to CAT5 #1. Pins are marked with dots, counted from 1 on each header.

2-pin header (power), sitting directly on the rails with no wires:

| Pin | Signal |
|---|---|
| 1 dot | 3.3 V (from the regulator) |
| 2 dots | GND |

3-pin header (signals):

| Pin | Signal | Goes to |
|---|---|---|
| 1 dot | SDA | D21 |
| 2 dots | SCL | D22 |
| 3 dots | Thermistor energize | D25 (GPIO25) |

GPIO25 was chosen because it is output-capable, is not a boot-strapping or flash pin, and no project in this repo uses it.

## CAT5 cables

### Common to CAT5 #1 and #2

The pin assignments in the CAT5 #1 and #2 tables below are confirmed.

- **Cable:** bulk solid-core CAT5 (the cable on hand), cut to length. Solid core is stiff and can fatigue if flexed repeatedly. That risk is accepted.
- **Plugs:** both cables have a plug only at the ADS1115 box end. Crimp RJ45 plugs rated for solid core, since ordinary stranded-wire plugs grip solid wire poorly. Test all 8 conductors with a continuity tester or cable tester before connecting anything. Don't unplug and replug often.
- **Pairing:** each signal is twisted with a ground (or, for the thermistor, with its own return), so SDA, SCL and the analog lines are shielded from each other. The pair colors match the warm box wires (SCL brown, SDA orange, 3.3 V green).
- **Pin order:** RJ45 pins follow T568B, which is how the jacks are marked: 1 orange-striped, 2 orange, 3 green-striped, 4 blue, 5 blue-striped, 6 green, 7 brown-striped, 8 brown. Use the same order at both ends of each cable. If a jack is marked T568A, the pin numbers change but the colors don't.
- **Jacks and holes:** jacks face down. Seal each cable hole with a grommet or sealant, since the boxes are vented for air, not water. Label the jacks "NOT ETHERNET": they carry 3.3 V, and plugging one into a network port (especially PoE) can damage a part.
- **Drip loops:** where a cable leaves the bottom of a box, it hangs in a U at least 3-4 in deep, with its lowest point below the hole and a gentle radius, so water drips off before it reaches a jack or hole.
- **Capacitance:** about 50 pF per metre. CAT5 #1 (about 20 pF) and #2 (about 100 pF) total about 120 pF before the boards' own, well under the 400 pF I2C limit, so the 100 kHz clock stays.
- **Strain relief:** on both cables at every box.

### CAT5 #1: warm box to ADS1115 box (jack 1)

| RJ45 pin | Wire | Signal | Warm box end (headers) | ADS1115 box end |
|---|---|---|---|---|
| 1 | orange-striped | GND | 2-pin header, 2 dots | ground point |
| 2 | orange | SDA | 3-pin header, 1 dot | SDA trace |
| 3 | green-striped | GND | 2-pin header, 2 dots | ground point |
| 4 | blue | Thermistor energize | 3-pin header, 3 dots | energize node |
| 5 | blue-striped | GND | 2-pin header, 2 dots | ground point |
| 6 | green | 3.3 V | 2-pin header, 1 dot | 3.3 V trace |
| 7 | brown-striped | GND | 2-pin header, 2 dots | ground point |
| 8 | brown | SCL | 3-pin header, 2 dots | SCL trace |

- **Length:** 2 ft (confirmed). It leaves the bottom of the warm box and plugs into jack 1 on the bottom of the ADS1115 box. The U takes about 11-14 in (4 in down, 4 in back up, 3-6 in across), plus the plug bends and the stiffness of solid core.
- **Warm box end:** no plug or jack. The solid-core conductors clamp straight into the 2-pin and 3-pin screw terminal blocks. The four striped GND wires are twisted and soldered together, with a single wire leaving the joint into the 2-dot (GND) terminal. The soldered joint is covered with heat shrink.

### CAT5 #2: ADS1115 box (jack 2) to sensor end

| RJ45 pin | Wire | Signal | ADS1115 box end | Sensor end |
|---|---|---|---|---|
| 1 | orange-striped | GND (SHT45) | ground point | SHT45 GND |
| 2 | orange | SDA | SDA trace | SHT45 SDA |
| 3 | green-striped | GND (SHT45) | ground point | SHT45 GND |
| 4 | blue | Thermistor divider tap | tap node | thermistor lead 1 |
| 5 | blue-striped | Thermistor ground return | ground point, directly | thermistor lead 2 |
| 6 | green | 3.3 V | 3.3 V trace | SHT45 VIN |
| 7 | brown-striped | GND (SHT45) | ground point | SHT45 GND |
| 8 | brown | SCL | SCL trace | SHT45 SCL |

- **Length:** 6-7 ft (confirmed). The run is a drip loop at the box (about 11-14 in), about 3 ft across, 1-2 ft of drop to the sensors, and about 6 in of slack inside the hood. Plug trimming and soldering use some length.
- **Ends:** a solid-rated plug at jack 2; soldered to the sensor-end perfboard, with no plug.
- **Thermistor pair:** a small analog signal on the longer run. It stays in its own twisted pair, with its filter at the ADS1115 end. Build step 3 checks the noise.

## ADS1115 box

A plastic switch box with a keystone cover plate, mounted open face down beside the warm box (proposed). The plate holds two RJ45 jacks facing down: jack 1 for CAT5 #1 and jack 2 for CAT5 #2. Vent at the top, behind fine screen against insects.

Everything is wired on a perfboard inside. The ADS1115 breakout plugs into female headers; the divider resistor, filters and both jacks are wired to the same perfboard. The bus traces run from jack 1 through the ADS1115 to jack 2.

### How the thermistor is read

From `ads1115-dev/DESIGN.md`:

- Ratiometric reading with pulsed excitation: GPIO25 drives the divider only while a conversion runs.
- A0 reads the excitation voltage and A1 reads the divider tap. The thermistor value comes from the ratio A1 ÷ A0, so ADC reference and supply errors cancel.
- Inputs are single-ended, with the same gain setting on every input. One chip takes the excitation monitor plus up to 3 thermistors.

```
GPIO25 energize ──┬── 1 kΩ ──┬── A0   (100 nF from the A0 pin to the ground point)
 (CAT5 #1 blue)   │
                  └── 10k ───┬── tap ── 1 kΩ ──┬── A1   (100 nF from the A1 pin to the ground point)
                             │
                             └── CAT5 #2 blue ── thermistor ── CAT5 #2 blue-striped ── ground point
```

### ADS1115 board pins

Pin names are typical for the Lonely Binary board and not verified against it. Read them off the board before soldering.

| Board pin | Goes to | Notes |
|---|---|---|
| VDD | 3.3 V trace | |
| GND | ground point | |
| SCL | SCL trace | |
| SDA | SDA trace | |
| ADDR | not connected | pulled low on the board, which gives 0x48 |
| ALRT | not connected | |
| A0 | 1 kΩ from the energize node, 100 nF to the ground point | excitation monitor |
| A1 | 1 kΩ from the tap node, 100 nF to the ground point | thermistor voltage |
| A2, A3 | not connected | spare, for more thermistors |

### Divider and filter parts

| Part | From | To |
|---|---|---|
| 10k resistor (0.1%, ≤15 ppm/°C) | energize node | tap node |
| 1 kΩ (1% is fine) | energize node | A0 |
| 1 kΩ (1% is fine) | tap node | A1 |
| 100 nF | A0 | ground point |
| 100 nF | A1 | ground point |

- The 10k's stability matters more than its tolerance: calibration absorbs its value, not its drift.
- `DESIGN.md` assumes the board has no input filters of its own. Check yours, since onboard parts would add to these.

### Ground point

One node on the perfboard. All eight striped wires (four from each cable) and the ADS1115's GND pin join there. The thermistor's return (CAT5 #2 blue-striped) reaches it directly, not through the SHT45's ground wires, so SHT45 current never flows in the thermistor's return. This is the only place the two grounds are joined.

## Sensor end

A small perfboard on the end of CAT5 #2, inside the hood. The SHT45 breakout plugs into a 5-pin female header, and the thermistor's two wires solder to pads on the same perfboard. No electronics other than the sensors are there.

### SHT45 (Adafruit 6174, PTFE membrane)

Pin names: VIN, 3Vo, GND, SCL, SDA. The order along the board is from memory and not verified, so read it off the board before soldering.

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

| Pad | CAT5 #2 wire | Signal |
|---|---|---|
| Lead 1 | blue | divider tap |
| Lead 2 | blue-striped | thermistor ground return, not joined to the SHT45 GND here |

The thermistor is a glass-bead NTC; the glass is sealed but the leads and solder joints are bare. They stay dry in a cooking bag for steam calibration (oven bags are rated about 200 °C, and a salt-water leak test at room temperature passed). Outdoors, moisture films on bare leads could leak, so they need a bag or a coating. Not decided.

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

| Project | Checks | A pass looks like |
|---|---|---|
| `i2c-scan` | Both I2C buses: lines held low at idle, and every address that answers. Reads the BMP388 chip id and the SHT4x serial number. Repeats until reset. | Bus 0 idle high, with the addresses under [Devices on bus 0](#devices-on-bus-0) that are wired so far. Bus 1 has nothing. |
| `rtc-probe` | The DS3231: valid time, oscillator-stop flag clear, seconds advancing, chip temperature. | `PASS`. Set the clock once first (`SET_FROM_BUILD` 1, flash, then back to 0 and reflash). The clock is kept in UTC. |
| `sd-probe` | The SD card: mounts it (never formats), writes, reads back and deletes a file, reports size and free space. | `PASS`. A `gpio: conflict found for GPIO[4]` warning after the unmount is harmless. |

Results so far: `i2c-scan` shows 0x57, 0x68 and 0x76 with the headers installed and nothing connected; `rtc-probe` passed, including a power-off test on the backup battery; `sd-probe` passed with a 120 MB card at 4 MHz.

### Build order for the rest

Each step adds one unknown, and the scan checks it:

1. CAT5 #1 and the ADS1115: expect 0x48.
2. CAT5 #2 and the SHT45: expect 0x44 and a serial number.
3. A fixed precision resistor in place of the thermistor: check the noise. This is the baseline for later.
4. The thermistor, bagged or coated.

Each breakout adds its own pull-ups, so check the idle levels on the scan after each step.

### Reading the scan output

- `idle levels: SDA low` or `SCL low`: a short, a miswired pin, or a device holding the line. The scan skips that bus. Unplug devices one at a time to find the cause.
- A BMP388 address that answers but isn't a BMP388 (register 0 isn't 0x50) prints "not a BMP388".

## Open items

- **Thermistor protection outdoors:** a bag or a coating for the bare leads and joints.
- **Mounting:** the ADS1115 box (switch box and keystone plate) is proposed, not built. Mounting heights are not decided.
- **Sensor hood:** bowl or jar, vents, the silicone seal and the hanger are proposed, not built.
- **ADS1115 board:** read the pin names off the board, and check whether it has its own series resistors or capacitors on A0 to A3.
- **SHT45 pin order:** read it off the Adafruit 6174 before soldering.
- **Firmware for GPIO25:** no code drives the energize lead yet.

## Conditions to monitor

Concerns accepted for the prototype. Check these after the first weeks outdoors, and again each season.

- **Scan health:** run the scan periodically. A missing 0x44 or 0x48, or an idle line no longer high, points at a cable, plug, jack or header contact.
- **Contacts:** look for green or white corrosion on the SHT45 header and the jacks, and for intermittent readings. Remedy: see [Contact protection](#contact-protection).
- **Solid-core cable and solder joints:** look for cracks at the plug ends and the perfboard, or readings that jump when a cable is touched. Remedy: more strain relief, or stranded cable at the sensor end.
- **Hood:** check the silicone around the CAT5 #2 entry (especially on polypropylene) and look for water tracks below it. Look for water on the membrane, the perfboard or the jacks; remedy with a second bowl, a skirt or a collar above the entry.
- **Hood lag:** compare against a bare sensor for a day after any change. Humidity that stays high long after rain suggests too few vent holes.
- **Insects and nests:** in the hood, the vent screens and the ADS1115 box.
- **Thermistor protection:** repeat the bag leak check after the first steam calibration. Outdoors, check the bag or coating for pinholes and the leads for moisture.
- **Drift:** compare the SHT45 and the thermistor with a reference occasionally. A slow offset can mean a contaminated membrane or heater effects.
- **Noise:** compare noisy thermistor readings against the fixed-resistor baseline from build step 3.

## Design notes

Figures are estimates from typical datasheet values and general reasoning, not measurements.

### Heat and resolution

- The SHT45 resolves about 0.01 °C, and the calibrated thermistor chain is budgeted at ±0.05 to 0.1 °C. Effects of thousandths of a degree can be ignored; only about 0.05 °C or more at the sensor matters.
- The SHT45 breakout's power LED (a few mW) was probably the biggest steady heat source near the sensors, so it was removed. Adafruit's documentation doesn't confirm the LED, so it was checked on the board itself. The ADS1115 board still has its LED.
- The ADS1115 (about 0.5 mW converting), the divider (about 0.5 mW, pulsed) and the SHT45 itself are small.

### Why the ADS1115 box is separate

A precaution, not a known problem. It keeps the 10k resistor (about 15 ppm/°C) out of the warm box's heat, and the analog inputs away from WiFi and SD current bursts. Venting helps the resistor's stability. If the fixed-resistor noise test (build step 3), logged in place, shows no wider spread than on the bench, the two boxes can be merged.

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
