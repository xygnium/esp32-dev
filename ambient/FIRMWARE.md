# Ambient firmware

Decisions and open items for the `ambient` firmware. Wiring is in [WIRING.md](WIRING.md). The firmware currently sends a status line over UDP to the listener (`udp_listener.py`). It has no ADS1115 code yet. It doesn't call `esp_wifi_set_ps`, so it runs with ESP-IDF's default power-save mode, `WIFI_PS_MIN_MODEM` (`esp_wifi.h`).

The reading scheme (ratiometric, pulsed excitation on GPIO25, A0 for the excitation and A1 for the tap) is in [How the thermistor is read](WIRING.md#how-the-thermistor-is-read).

## Where the noise comes from

The ADS1115 is delta-sigma with a digital filter. At low data rates (8 to 128 SPS) it rejects energy far above its passband, so 2.4 GHz rarely corrupts the signal directly. The mechanisms that matter:

- **RF rectification** in the input ESD diodes or front end, which gives a DC offset.
- **Supply and ground disturbance** from WiFi transmit bursts (about 200 to 400 mA), which causes rail droop and ground bounce.
- **Long sensor leads** acting as antennas.

The firmware can't time conversions around WiFi activity: beacon wakes and delayed transmits happen on their own. The effect is expected to be far below the thermistor's ±0.5 °C tolerance. The bench test in "Why the ADS1115 box is separate" in WIRING.md measures the effect.

## Sampling

**Table 1: Sampling decisions (proposed, not yet implemented)**

| Setting | Choice | Why |
|---|---|---|
| Data rate | 8 to 32 SPS | Strong filtering; about 125 ms per conversion at 8 SPS |
| Mode | Single-shot | The firmware chooses when each conversion runs |
| Readings | One set per reading: A0, A1, A0 | The excitation is on for about 0.4 s at 8 SPS, which heats the thermistor about 0.006 °C. Sets back to back keep heating it (about 0.03 °C for five), so if more readings are ever wanted, space the sets about 15 s apart |
| Order | Energize, wait, convert A0, A1, A0, de-energize, publish | |
| Excitation | GPIO25 low (switch on) from about 20 ms before the first conversion until the last conversion ends | The inputs climb to their final voltage through the filters: A1 takes about 7 ms at 25 °C and 11 ms at −20 °C to settle within one ADC step. A0 is driven directly by the switch and settles in microseconds. A conversion started at switch-on averages the climb in, so if A1 were converted first the error would be about +0.2 °C at 25 °C and +1.4 °C at −20 °C. With the A0, A1, A0 order A1 starts long after it has settled, so the wait is a margin that costs nothing. The 20 ms figure is from `ads1115-dev/DESIGN.md`, still to be confirmed on the bench |

## WiFi power

**Table 2: WiFi power decisions (proposed)**

| Option | Effect | Decision |
|---|---|---|
| Power-save (`WIFI_PS_MIN_MODEM`) | Stays associated and wakes at each DTIM beacon; the AP buffers packets; IP and DHCP lease survive | Already in use: it is the ESP-IDF default, so no call is needed |
| Radio off (`esp_wifi_stop()`) | Sockets die; reconnecting takes about 1 to 3 s with DHCP (a static IP is faster) | Only for sample intervals of minutes or more; not needed for EMI |

## Open items

- **Write the ADS1115 code** to Table 1. Read A0, A1, A0 and average the two A0 readings, so the ratio holds even if the excitation moves between readings. Then run the noise test in WIRING.md (build step 3) with WiFi idle and with WiFi plus SD writes.
- **Keepalive:** if the firmware ever moves from UDP status lines to MQTT, set the keepalive longer than any quiet period. The broker drops a session after 1.5 times the keepalive with no traffic.
