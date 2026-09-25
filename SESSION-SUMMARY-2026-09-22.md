# Attic sensors & radiant barrier: session summary (2026-09-22)

Follows on from `SESSION-SUMMARY.md` (2026-09-18). That session built the report
tool and produced provisional fake-vs-Dallas findings from a 4-hour window. This
session re-ran the comparison over ~56 h of data, which **changes several of those
conclusions**, and then reframed the whole question around the actual objective:
measuring radiant barrier effectiveness.

## 1. Fixed: `run.sh`

Trailing whitespace after the line-continuation backslashes. `\` followed by a
space escapes the space, not the newline, so line 1 ended there (passing a stray
`" "` argument, hence `unrecognized arguments:` with nothing visible after it)
and lines 2–3 ran as separate shell commands. Stripped the trailing whitespace;
the script now runs clean.

## 2. Sensor characterisation (supersedes the 2026-09-18 provisional read)

Method: window 2026-09-19 07:00 onward (~1400–1670 fully-populated sample times,
47–56 h, attic 20.6–48.0 °C). Reference = mean of the two genuine Dallas parts.
Offsets fit on the first half of the window, p95 measured on the held-out second
half. Analysis scripts were ad-hoc (see Open items).

### The fleet splits 6 good / 4 bad, and the split is the same on every metric

| sensor | noise floor | drift @30–36 | drift @40–46 | held-out p95 | class |
|---|---|---|---|---|---|
| center-floor-street *(Dallas)* | 0.125 | 0.00 | 0.03 | 0.27 | good |
| raf-rr-hot-street *(Dallas)* | 0.125 | 0.00 | 0.03 | 0.27 | good |
| rafter-rear-center | **0.062** | 0.04 | 0.05 | **0.24** | good |
| rafter-center-center | 0.188 | 0.06 | 0.02 | 0.42 (0.29 w/gain) | good |
| raf-fr-hot-street | 0.125 | 0.08 | 0.04 | 0.40 | good |
| rafter-front-patio | 0.125 | 0.10 | 0.17 | 0.61 | marginal |
| rafter-rear-patio | 0.438 | 0.20 | 0.37 | 1.49 | bad |
| rafter-center-patio | 0.438 | 0.22 | 0.39 | 1.54 | bad |
| raf-fr-cool-street | 0.438 | 0.23 | 0.46 | 1.52 | bad |
| rafter-front-center | 0.438 | 0.23 | 0.37 | 1.12 | bad |

All figures °C. Noise floor = p95 deviation from the sensor's own local 5-reading
median. Drift = day-to-day change in offset **at matched reference temperature**,
which separates true drift from gain error appearing at different temperatures.

- `rafter-rear-center` is the **quietest device in the fleet**, beating both
  genuine Dallas parts (0.062 vs 0.125).
- The four bad sensors are all drifting **monotonically toward zero error**
  (e.g. raf-fr-cool-street 1.91 → 1.45 in the 40–46 °C band). That looks more
  like burn-in settling than runaway drift, but two days at matched temperature
  can't confirm it. **Leave them logging another week before writing them off.**

### The three-sensor ensemble is the headline result

`rafter-rear-center` + `rafter-center-center` + `raf-fr-hot-street`, averaged,
with a single **−0.30 °C offset**:

- held-out p95 **0.14 °C** — passes the 0.25 tolerance with margin, no gain term
- each genuine Dallas part tracks the Dallas *pair mean* at only 0.27 p95

**Three fakes and one constant beat either real sensor.** Adding
`rafter-front-patio` degrades it to 0.21 (still passing); it's the weak member.

### The correlated triplet: not three sensors' worth of information

`rafter-center-patio`, `rafter-rear-patio` and `rafter-front-center` have
residual correlations of **0.94–0.99** with each other, and rear-patio /
center-patio report **identical values 51.3%** of the time. Averaging all four
bad sensors gives p95 1.36 — no better than one of them. This quantifies and
extends the 2026-09-18 duplicate-value oddity. `raf-fr-cool-street` is
*uncorrelated* with the triplet (0.08–0.10) and independently bad.

### The reference itself is soft

The two genuine, co-located Dallas parts disagree by **p95 0.57 °C, worst case
1.19 °C**. This is *not* thermal lag: during calm periods (|dT/dt| < 0.02 °C/min,
n = 516) the spread is still 0.57, and the worst moment occurred at
dT/dt = +0.008. It is static calibration spread, squarely within the DS18B20's
±0.5 °C datasheet spec.

**Consequence: the 0.25 °C tolerance is below the yardstick's own resolution.**
Adding the 3 Dallas spares tightens the reference mean (0.27 → 0.22 → 0.19 → 0.17
for N = 2…5) but does nothing for any individual deployed sensor.

### Things tested and ruled out

- **Rolling/centred averaging of previous N readings.** Tested as a correction
  stage, not just a plot. N=5/9 barely moves the bad sensors (1.52 → 1.44) and
  *degrades* the good ones (rafter-rear-center 0.24 → 0.34). Their error is not
  white noise. Not worth adding to the tool.
- **A solar beam through the gable vents** as the cause of the bad sensors'
  behaviour. The four bad sensors carry a strong repeatable diurnal residual
  (peak-to-trough 1.0–1.5 °C vs 0.29–0.51 for the good six) peaking at noon,
  which has the right shape. But across three days the midday residual scales
  with the diurnal swing (+1.09 / +0.67 / +0.40 for swings of 27.4 / 24.9 /
  19.4 °C) rather than switching on and off. Looks like sensor response, not
  sunlight. Not conclusive — no genuinely overcast day in the window.

## 3. The decision: is full co-location worth the effort?

**No.** Best case — position error removed entirely — each sensor bottoms out at
its own noise floor, which is a property of the die and travels with it:

| non-co-located sensor | now | floor | outcome |
|---|---|---|---|
| rafter-rear-center | 0.24 | 0.062 | already usable |
| rafter-front-patio | 0.62 | 0.125 | **promoted** |
| rafter-center-center | 0.41 | 0.188 | **promoted** |
| rafter-center-patio | 1.54 | 0.438 | still fails |
| rafter-rear-patio | 1.49 | 0.438 | still fails |
| rafter-front-center | 1.12 | 0.438 | still fails |

A full early-morning co-location campaign buys **two sensors**, added to an
ensemble that already achieves 0.14 without them. And a sensor offset is a
property of the part, so it transfers on redeployment — meaning those two
offsets can be had by bringing just those two sensors next to a Dallas part for
**one night**, not by building a full rig.

| option | verdict |
|---|---|
| Co-locate everything | **No.** Optimises absolute accuracy, which the objective never uses. |
| Buy more genuine Dallas parts | **No** for accuracy. Install the 3 spares for a firmer reference. |
| Switch sensor type (SHT4x etc.) | **Only if** better than ±0.5 °C *absolute* is genuinely needed. |

## 4. Reframe: it's a radiant barrier study, so absolute accuracy is irrelevant

A before/after intervention is a **differential** measurement — same sensor, same
spot. A constant offset cancels exactly. What matters is **drift** (≤0.10 °C
day-to-day for six of ten sensors) and **gain** (0.003–0.029 /°C for the good
ones, contributing <0.15 °C of error on a 5 °C change). Noise is a non-issue at
~720 samples/day.

**Six of ten sensors, including four fakes, are already good enough — with zero
attic work.**

### The real enemy is weather, not sensors

Peak attic temperature in the logged window: 47.6 °C (09-20), 46.6 (09-21),
25.4 (09-22, partial day). A naive before/after across that kind of swing
"measures" cloud cover.

### Therefore: split-attic control, not before/after

The labels already form a grid — *street / center / patio* × *front / center /
rear*. Barrier one side, keep the other as a simultaneous control, and the metric
becomes the change in the street-minus-patio difference, which cancels weather.

Baseline already in the data:

- street − patio at peak hour: −1.80, −1.66, +0.25 across the three days
- day-to-day wobble of the daily mean: **0.51 °C** — well under the expected
  several-°C effect

### The one piece of physical work worth doing

Drifters are concentrated on the intended control side:

| side | good / total |
|---|---|
| street | 3 / 4 |
| center | 2 / 3 |
| **patio** | **1 / 3** (only `rafter-front-patio`) |

Move good parts to the patio side, using the 3 Dallas + 7 fake spares. Targeted
swap, not a co-location rig.

## 5. Outdoor ambient — the missing piece

No ambient reference currently exists; all 10 sensors are in the attic. This is
where absolute accuracy *does* matter, since ambient is the denominator of every
normalised metric. Planned ESP32 + **SHT4x** project covers it. Use **SHT45**
(±0.1 °C) over SHT40 (±0.2) for this role.

- **The radiation shield matters more than the sensor.** Unshielded in sun reads
  5–10 °C high — 20–50× the part's spec error. Stacked-plate screen, north-facing,
  clear of walls and roof. Passive is fine outdoors (there's wind).
- **Keep it off the ESP32 board** — regulator and WiFi radio warm a board-mounted
  sensor by a few tenths, varying with transmit duty, so not even a constant offset.
- **Bench-co-locate it against a Dallas part before deployment** to get the
  inter-system offset. This is the one co-location worth doing, and it's desk work.
- Consider a **second SHT4x inside the attic**: tighter absolute anchor than the
  Dallas pair, plus attic RH (condensation on the cold side of a radiant barrier
  is a real failure mode). Rated to +125 °C, so 50 °C peaks are fine.

### Pipeline changes needed before that node exists

Schema is `readings(timestamp_epoch, label, timestamp_utc, temp_c, valid)`,
PK `(timestamp_epoch, label)`; `sensors(id, romcode TEXT UNIQUE, label)`.
No device column, so a second logger can write in with unique labels — but:

- **`fit_corrections` (temp_report.py:189) and the group comparisons join on
  exact `timestamp_epoch`.** Current cadence is rigid (120 s, at :46 past the
  minute). A second ESP32 will share essentially zero timestamps and be
  **silently dropped from every comparison** while still appearing in the summary
  table and charts (which bucket). Fix: tolerance/bucket join. Outdoor temp moves
  slowly; a 5-minute bucket is plenty.
- **No humidity column.** `ALTER TABLE readings ADD COLUMN rh_pct REAL` is
  non-breaking. Do **not** store RH as a pseudo-sensor label — it would enter the
  tool as a 0–100 "temperature" and wreck the shared axis, heatmap scale and every
  min/avg/max.
- SHT4x has a unique serial (I²C command `0x89`) that fits `sensors.romcode`,
  preserving the romcode-over-label discipline.

## 6. Instrumented location (hot/cool side of the barrier)

### Framing

An unshielded sensor in an attic does not measure air temperature — it settles
where convection balances IR from a ~70 °C roof deck. **The barrier changes the
radiant environment, so it changes that error**, in the same direction as the
effect being measured. A bare sensor inflates the result.

Also: a foil barrier stapled under rafters has no meaningful ΔT *across* it. The
two "sides" are really two air spaces. In the narrow gap radiation dominates, so
measure **surfaces** there and **shielded air** below.

### Probe roles — only two need to be matched

| probe | role | accuracy needed |
|---|---|---|
| globe, below barrier | **metric** | matched to the air probe |
| shielded air, below barrier | **metric** | matched to the globe |
| barrier cool face | supporting | ±0.5 |
| roof deck underside | covariate | ±0.5 |
| barrier hot face | diagnostic | ±1 |

The headline number is `globe − shielded air`, a difference between two probes
centimetres apart, so shared absolute error cancels. **Two-point bench
calibration gives ±0.05–0.1 °C relative**, which is all that quantity depends on.

**Minimum viable rig: one globe + one shielded air sensor as a matched pair.**
The other three explain *why* the number moved.

### Parts

| part | spec | ~cost |
|---|---|---|
| 10k NTC thermistors, **glass-encapsulated** | not epoxy — epoxy absorbs moisture and drifts | $3–6 ea |
| ADS1115 16-bit I²C ADC | 4 channels; 4 addresses per bus (0x48–0x4B) | $15 |
| reference resistors, 10k | 0.1%, **≤10 ppm/°C** — electronics sit in the 50 °C attic | $1 ea |
| 40 mm fan, **ball bearing, ≥70 °C**, 2-wire | sleeve bearings derate badly when hot | $5 |
| globe | 40 mm table-tennis ball, matte black high-temp paint | $2 |
| aluminium foil tape | 3M 425 or HVAC foil — not duct tape | — |
| BH1750 / TSL2591 light sensor | logged covariate at the globe | $3–8 |
| SHT45 + stacked-plate shield + enclosure | outdoor node | $55–75 |

Rough totals: ~$50 matched pair, ~$90 all five probes, ~$150 with the outdoor node.

### Design decisions settled

- **Ratiometric excitation.** ADS1115 is not ratiometric (internal reference).
  Spend one channel measuring the excitation voltage and compute each thermistor
  as a ratio against it; supply drift then cancels. Budget **3 thermistors + 1
  excitation monitor per chip**.
- **Pulsed excitation from a GPIO.** Self-heating is 0.1–0.3 °C at steady DC —
  and it differs between the still-air globe and the fan-cooled air probe, landing
  straight in `globe − air`. Enable the divider, convert (few ms), read, disable.
  Bead thermal time constant is seconds, so heating is negligible. The GPIO's
  imprecise output voltage doesn't matter because the measurement is ratiometric.
- **Bond the leads, not just the sensor** — tape the sensor *and* the first ~10 cm
  of lead flat to the surface, or it conducts heat and reads the air.
- **Calibrate in an ice bath + warm bath** (sous-vide circulator is ideal).
  Bundle all probes for both points. Calibrating end-to-end also removes mux and
  source-impedance errors.

### Cable runs (24–32 ft)

| signal | OK at that distance? |
|---|---|
| analog thermistor | **yes** — standard practice in building automation |
| 1-Wire (DS18B20) | **yes** |
| I²C | **no** — 400 pF spec budget ≈ 6–8 m of cable alone |
| SPI (MAX31865) | **no** |

Thermistor lead resistance is negligible (60 ft of 24 AWG ≈ 1.6 Ω against 10 kΩ =
0.016%). If I²C must travel: P82B715 buffer pair, or PCA9615 differential over
CAT5. **The attic SHT4x is I²C-only, so it must live within a metre or two of
its own microcontroller.**

### Cable: use CAT5, not shielded twisted pair

CAT5 is the better choice here, not merely the cheaper one. Twisting is what
rejects magnetically-coupled noise, and CAT5's twist rate is tight and controlled
in a way generic two-conductor shielded cable usually isn't. 24 AWG solid copper,
stiff enough for a fixed run, and four pairs = **four probes per cable** — the
whole instrumented cluster on one run.

Rules:

- **One twisted pair per probe.** Sense conductor and its ground must be the
  *same* pair. Splitting a probe across conductors from two different pairs
  discards the entire benefit — the most common way this is got wrong.
- **No shared ground conductor between probes.** Each probe gets its own pair
  with its own ground leg, all joining at the single-point ground on the board.
  A shared return creates shared-impedance coupling between channels.
- **Keep fan power out of the same cable.** Brushless commutation noise is
  exactly what you don't want on the sense lines. Separate cheap zip cord.
- **Ground the unused pairs** at the ADC end. Not a real shield, but free.

**What's given up without a foil screen** is rejection of capacitively-coupled
60 Hz from mains wiring running through the attic. Handle that at the ADC rather
than in the cable:

- set the ADS1115 to a **low data rate (8 or 16 SPS)**, where its delta-sigma
  filter gives inherent 50/60 Hz rejection; or
- take ~15 samples at 860 SPS — one full 60 Hz cycle — and average.

Either is more effective than a shield would have been, and costs nothing.

Note the RC low-pass at each ADC input (1 kΩ series + 100 nF to ground) sits at
about **1.6 kHz**: it kills RF and cable ringing but does **nothing** for 60 Hz.
Don't rely on it for mains. For hardware attenuation as well, raise the cap to
10 µF (~16 Hz corner) and extend the excitation settle delay to ~200 ms before
converting.

F/UTP CAT5e (foil-screened) costs only slightly more than plain UTP and keeps all
the twist-rate advantages if a shield is wanted anyway — but it isn't worth it
for a DC measurement sampled every two minutes.

### FARS (fan-aspirated radiation shield)

Reference: Thomas & Smoot 2013, *"An Effective, Economic, Aspirated Radiation
Shield…"*, JTECH 30(3), 526–537, doi:10.1175/JTECH-D-12-00044.1. Double-walled
concentric PVC, fan-ventilated, ~0.2 °C radiation error. Paywalled — could not
read the full text, so the internal flow path is unverified.

**Verdict: right performance class, but don't copy the materials.** It's tuned
for *shortwave* — white, double-walled, because sun heats the outer wall. An
attic has no beam solar; the threat is longwave IR from the deck, and white paint
is ε ≈ 0.9 in the IR, same as black. **A foil / polished-aluminium outer skin is
the lever**, not the second wall:

| inner wall above air | error at 3 m/s |
|---|---|
| 1 °C | 0.06 °C |
| 5 °C | 0.31 °C |

Build spec:

- **1–1.5″ inner tube** (literature uses 1.5–2″ because it houses 12 mm probes;
  a 2.5 mm bead doesn't need it). Smaller = less flow, less attic disturbance,
  shorter development length.
- **L/D ≥ 5.** Aspect ratio governs, not diameter — the sensor must not see out
  the ends. Concentric intake so there is no straight optical path.
- **Down-facing intake at the bottom.** The outdoor convention transfers, for a
  different reason: the dominant radiant source (roof deck) is also overhead.
- **Fan pulling, downstream of the sensor.** Pushing puts motor heat (~0.3 °C)
  upstream, and parks the bead in the hub wake and swirl.
- **Side discharge through an elbow** — removes the vertical sight line through
  the fan, and aims exhaust away from the globe.
- **Target 3 m/s**, where the error curve flattens (h ∝ V^0.47):

| tube ID | CFM at 3 m/s |
|---|---|
| 1″ | 3.2 |
| 1.5″ | 8.4 |
| 2″ | 13.5 |

  Fan ratings are free-air; derate ~2×. **No PWM regulation** — run at 100%; a
  constant flow gives a constant, largely-cancelling error, and RPM wouldn't fix
  CFM anyway (a fouling intake makes an unregulated fan speed *up* while
  delivering less). Do the commissioning sweep once on the bench by varying
  supply voltage (12/9/7/5 V) and find where temperature stops responding.
- **Coarse, removable intake screen**; clean when up there.

### Globe siting

- ~0.5 m from the FARS — close enough to sample the same air, far enough to avoid
  the exhaust. Prefer the **intake** side (sink flow, ~1/r², no directed jet).
- **Not aspirated and not shielded** — it is the opposite instrument by design.
- **Gable vent light: a non-issue for diffuse light.** Solid angle of a 0.07–0.3 m²
  vent at 3–8 m is 0.001–0.03 sr → ~0.05–1.6 W/m² at the globe → **<0.03 °C**.
  A *direct beam* would be catastrophic (~5–8 °C), so the light sensor stays as
  insurance: gable vents are vertical and therefore most exposed to **low sun**
  (early/late, and winter), and before/after periods may straddle that change.
- **Flow loss mimics success.** If the FARS intake fouls, the air probe drifts up
  toward the globe and `globe − air` shrinks — the same signature as a working
  barrier. Tell-tale: track shielded-probe minus nearest unshielded DS18B20. A
  working barrier pulls the *globe down*; flow loss pushes the *air probe up*.

## 7. Open items

- **Analysis scripts are not preserved.** Everything in section 2 came from
  ad-hoc scripts in a session scratchpad, not this directory. Worth saving the
  drift-at-matched-temperature and residual-correlation checks into the repo, or
  folding them into `temp_report.py`, so they can be re-run as data accumulates.
- **Bucket/tolerance join** in `fit_corrections` and `compare_groups`, before the
  second logger exists (see section 5).
- **`--ensemble NAME=pat,...`** — a virtual averaged sensor run through the
  existing fit/verdict machinery. This is what found the 0.14 result.
- **Residual-correlation matrix** in the report — the check that exposed the
  triplet and would flag any future non-independent sensors.
- **Watch the four drifters another week** to see whether they settle (burn-in)
  or keep going (junk).
- **Decide the split** — which side gets the barrier — and move good sensors to
  the patio side beforehand.
- Still open from 2026-09-18: swap-tolerant reference, correction table keyed by
  ROM code, permanent `D1`–`D5` / `F01`–`F15` labels, `git init`.
