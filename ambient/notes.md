# ADS1115 + WiFi SoC: EMI and Radio Timing Notes

Context: ADS1115 reading thermistor dividers on a WiFi SoC (Pico W / ESP32), publishing to MQTT.

## Where the noise actually comes from
The ADS1115 is delta-sigma with an internal digital filter; at low data rates (8–128 SPS) it rejects
energy far above its passband, so 2.4 GHz rarely corrupts the signal directly. Real mechanisms:
- **RF rectification** in input ESD diodes / front end → DC offset.
- **Supply/ground disturbance** from WiFi TX bursts (~200–400 mA pulses) → rail droop, ground bounce.
- **Long sensor leads** acting as antennas.

## Hardware mitigations
- RC filter at each input near the pins: ~1 kΩ series + 0.1 µF to ground; add 100 pF–10 nF
  differential cap across differential pairs.
- Prefer differential mode (AIN0–AIN1) to cancel common-mode pickup.
- Clean ADC/excitation supply: ferrite bead or LC filter, ideally a separate LDO;
  local 0.1 µF + 10 µF decoupling.
- Layout: ADC away from antenna, solid ground plane, keep radio return current out of analog ground.
  Short I²C runs.
- Remote sensors: twisted pair or shielded cable, shield grounded at ADC end.

## Ratiometric thermistor measurement
ADS1115 uses an internal reference, so excitation noise is NOT cancelled automatically.
Measure the excitation voltage on a second channel close in time and compute the ratio.

## Firmware / sampling
- Low data rate (8–32 SPS) plus averaging; median of 3–5 readings removes beacon-coincident outliers.
- Use single-shot mode and schedule conversions in radio-quiet windows:
  convert → read (~125 ms at 8 SPS) → publish → repeat.
  Since the firmware controls when TX happens, conversions naturally avoid TX bursts.

## Radio power modes vs. connection
- **Power-save / modem sleep keeps the connection** (stays associated, wakes each DTIM beacon,
  AP buffers packets; IP, DHCP lease, TCP/MQTT session survive).
  - Pico W: `cyw43_wifi_pm()` with `CYW43_PERFORMANCE_PM` or `CYW43_AGGRESSIVE_PM`.
  - ESP32: `esp_wifi_set_ps(WIFI_PS_MIN_MODEM)`.
- **Fully disabling WiFi drops the connection** (`esp_wifi_stop()`, cyw43 deinit). Sockets die,
  broker sees disconnect; reconnect ~1–3 s with DHCP (static IP is faster). Only worth it for
  sample intervals of minutes or more.
- **MQTT keepalive**: broker drops the session after 1.5× keepalive with no traffic.
  Keep keepalive comfortably longer than any quiet period.

Recommended default: stay associated, enable power-save, and time single-shot conversions
between publishes. Full radio-off is unnecessary for EMI purposes.
