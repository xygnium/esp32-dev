# Plan: sensor sampling (project 3)

Temporary. When this is implemented, its decisions move into [FIRMWARE.md](FIRMWARE.md) and this file is deleted. Builds on project 1 (SD and hard faults) and project 2 (clocks). The readings aren't stored or pushed yet: SD logging is the next project, then pushes that carry every sample.

## Decided

- **Code source:** port the probe code that ran on this hardware, as `ds3231.c` came from `rtc-probe`. The thermistor comes from `ads1115-probe`, not the shared `ads1115_core` library. The SHT45 and BMP388 come from `sensor-probe`. The probes stay as diagnostics.
- **A failed sensor is a soft failure:** the other sensors keep sampling. The failed one is retried at every sample and resumes when it answers. Hard faults (SD, RTC) are unchanged.
- **Alerting** (email, phone) is a separate project. Until then a failure shows only on the console, and later in logserv's output.
- **Thermistor ratio:** A1/A0 is kept as a data type of its own beside temp_c, so a later calibration can be applied to past data.
- **Time stamps:** each reading gets its own UTC stamp with milliseconds, taken when its measurement finishes. The BMP388's temperature and pressure share one stamp.

## Firmware (ambient)

1. **Boot:** drive GPIO25 high (excitation off) before anything else that could take time.
2. **`thermistor.c`,** from `ads1115-probe`: ADS1115 at 0x48, PGA ±4.096 V, 8 SPS, single-shot. Per reading: excitation on, wait 30 ms, convert A0, A1, A0, excitation off. It is always off again on return, even after an error. The ratio is A1 over the mean of the two A0 readings. The resistance is corrected for the ADC input (4.3 MΩ), then Steinhart-Hart with the generic coefficients (WIRING.md).
   - Checks from the probe: A0 is 3.1 to 3.5 V when on and moves less than 10 mV across the reading, and the ratio is between 0.02 and 0.98 (outside that is an open or a short).
   - The probe's "A0 near 0 V when off" check is dropped. It costs an extra 0.13 s conversion each sample and belongs in the diagnostic.
3. **`sht45.c`,** from `sensor-probe`: the high-precision measurement (0xFD), both CRCs, humidity clamped to 0 to 100 %. A value more than 5 % outside that range is a failure.
4. **`bmp388.c`,** from `sensor-probe`: soft reset and calibration read at init, then a forced measurement each sample. If it fails at boot, init is retried at each sample. Chip id 0x50 or 0x60. A value outside -40 to 85 °C or 300 to 1250 hPa is a failure.
5. **`sensors.c`:** at each sample interval, in this order: thermistor (about 0.42 s), the SHT45 straight after (so the two temperatures are as close in time as possible), then the BMP388. All three share the bus from `i2c_bus.c` and keep their device handles.
   - Each result holds the data types, the stamp, OK or failed, and an error reason (for example `nack`, `crc`, `range`, `open`, `short`).
   - No sampling while the clock is unset, or during a hard fault.
6. **Data types:**

   | Sensor | data_type |
   |---|---|
   | thermistor | temp_c, ratio |
   | sht45 | temp_c, rh_pct |
   | bmp388 | temp_c, pressure_pa |

7. **Console:**
   - `status` gains a line per sensor with its last values and their stamp, or its last error and how many samples it has failed in a row.
   - `read` takes one sample now and prints it. Useful for tests.
8. No fault injection is needed: every sensor can be disconnected for real (the BMP388 is socketed).
9. Build.

## Server (logserv)

No change in this project.

## Docs

1. `FIRMWARE.md`: a "Sensors" section with the decisions above. Table 1 changes from "proposed" to implemented. Update Open items.
2. `main.c` header comment.

## Test (on the board)

Before each test, check the port and `status` first.

1. All three sensors read OK. The values agree with `ads1115-probe` and `sensor-probe` run just before, and the thermistor and SHT45 agree within the thermistor's ±0.5 °C.
2. The stamps are in UTC with ms, and each reading has its own.
3. Unplug CAT5 #2: the SHT45 and the thermistor fail (the thermistor as an open), and the BMP388 keeps reading. Plug it back in: both resume at the next sample.
4. Unplug CAT5 #1: the ADS1115 fails as well. Both resume when it's plugged back in.
5. Pull the BMP388 from its socket with the power off, then boot: it fails at init and at each sample while the others keep reading. Put it back and boot: it reads OK.
6. Run for several hours at the 120 s default: no failures, and readings stay steady.

Then commit, and the user pushes.
