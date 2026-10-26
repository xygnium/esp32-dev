# Ambient firmware

Decisions and open items for the `ambient` firmware. Wiring is in [WIRING.md](WIRING.md). The firmware currently sends a status line over UDP to the listener (`udp_listener.py`). It has no ADS1115 code yet, and no WiFi power-save setting.

The reading scheme (ratiometric, pulsed excitation on GPIO25, A0 for excitation and A1 for the tap) is in [How the thermistor is read](WIRING.md#how-the-thermistor-is-read).

## Where the noise comes from

The ADS1115 is delta-sigma with a digital filter. At low data rates (8 to 128 SPS) it rejects energy far above its passband, so 2.4 GHz rarely corrupts the signal directly. The mechanisms that matter:

- **RF rectification** in the input ESD diodes or front end, which gives a DC offset.
- **Supply and ground disturbance** from WiFi transmit bursts (about 200 to 400 mA), which causes rail droop and ground bounce.
- **Long sensor leads** acting as antennas.

The first two are why the plan below times conversions around transmit bursts. The bench test in "Why the ADS1115 box is separate" in WIRING.md measures the effect.

## Sampling

**Table 1: Sampling decisions (proposed, not yet implemented)**

| Setting | Choice | Why |
|---|---|---|
| Data rate | 8 to 32 SPS | Strong filtering; about 125 ms per conversion at 8 SPS |
| Mode | Single-shot | The firmware chooses when each conversion runs |
| Averaging | Median of 3 to 5 readings | Removes outliers that coincide with a beacon or transmit burst |
| Order | Convert, read, publish, repeat | Conversions run between transmissions, so they avoid transmit bursts |
| Excitation | GPIO25 high only during a conversion | See the reading scheme above |

## WiFi power

**Table 2: WiFi power decisions (proposed)**

| Option | Effect | Decision |
|---|---|---|
| Power-save (`esp_wifi_set_ps(WIFI_PS_MIN_MODEM)`) | Stays associated and wakes at each DTIM beacon; the AP buffers packets; IP and DHCP lease survive | Use this |
| Radio off (`esp_wifi_stop()`) | Sockets die; reconnecting takes about 1 to 3 s with DHCP (a static IP is faster) | Only for sample intervals of minutes or more; not needed for EMI |

## Open items

- **Write the ADS1115 code** to Table 1, then run the noise test in WIRING.md (build step 3) with WiFi idle and with WiFi plus SD writes.
- **Set WiFi power-save** (Table 2). No call to `esp_wifi_set_ps` exists in the firmware yet.
- **Keepalive:** if the firmware ever moves from UDP status lines to MQTT, set the keepalive longer than any quiet period. The broker drops a session after 1.5 times the keepalive with no traffic.
