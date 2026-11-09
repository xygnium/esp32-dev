# The 64-byte USB junk

The console sometimes shows a run of � in place of the start of a line. It spoils only what you see on the serial console. Logging, the SD card and the messages to logserv are not affected.

## Symptom

```
���������������������������������������������������������������� 2026-11-09T00:30:44.407Z,thermistor,temp_c,24.03,ok
```

- Exactly 64 bytes, all 0xFF (captured with `serial-probe/raw_capture.py`).
- They take the place of the start of a line: the real bytes there are lost, not just pushed along.
- It hits the first output after a pause: the first line of a sample (30 s apart), or with `quiet on` the hourly `resynced from the DS3231` line. Most first lines still come through clean.
- 64 bytes is one full USB packet from the CP2102.

## Tests (2026-11-08)

| Setup | Result |
|---|---|
| ambient, monitor in the VM, Genesys hub (USB 3) | Junk, several times a day; twice in a minute with `quiet off` |
| ambient, miniterm on dev10 (no VirtualBox), Genesys hub | Junk within minutes |
| serial-probe (no WiFi, I2C or SD), miniterm on dev10, Genesys hub | Junk at pass 8 |
| serial-probe, board direct in a dev10 USB 2.0 port | 40 passes clean |
| serial-probe, board direct in dev10 USB 3 ports | Junk within 5 passes |
| serial-probe, Genesys hub in a USB 2.0 port | 51 passes clean |
| ambient with `quiet off`, Genesys hub in a dev10 USB 2.0 port (2026-11-09) | Overnight, clean |

Power cycling the hub port cleared it for a while (it came back within hours).

## Ruled out

- **The ambient firmware, WiFi, the sensors, the SD card:** serial-probe has none of them and shows the junk.
- **VirtualBox:** it shows on dev10 directly.
- **USB autosuspend:** the CP2102 is set to never suspend on dev10 and in the VM.
- **Cables:** a cable fault gives random errors or drops, not exactly 64 × 0xFF at the same point.

## Suspect

USB 3 ports, and the USB 3 hub. Why is not known. A guess, not checked: dev10's USB 3 ports are on its xHCI controller and the USB 2.0 ports on EHCI, so the controller or its driver may matter more than the port speed. `lsusb -t` with the board in each port would show which controller it's on.

## Reports elsewhere

- [Silicon Labs community](https://community.silabs.com/s/question/0D5Vm000001KaWyKAK/esp32-cp2102-works-fine-in-external-usb-hub-but-not-without?language=en_US): an ESP32's CP2102 isn't recognised at all when plugged straight into a laptop (Windows 11, AMD Ryzen 5 5600H, AMD USB 3.1 xHCI controller), but works through an external hub and on a desktop. A different symptom from ours, with the same pattern: trouble plugged straight into USB 3 (xHCI) ports. The Silicon Labs answer: "use a USB hub... This problem originates from the ESP32, not the CP2102", with no reason given.

## Workaround

Restrict the board to USB 2.0 ports, or to a hub plugged into one. A hub alone isn't always enough: on dev10 the Genesys hub on a USB 3 port still gave junk.

- **Newer computers** may have only USB 3 (xHCI) controllers and no USB 2.0-only port, so they may show this problem with no workaround on that machine.
- **In use, the logger doesn't need USB:** it runs from any 5 V supply, and the console is only for setup and fault finding.

## Tools

- `serial-probe`: prints a fixed block every 30 s with nothing else running, so any junk comes from the serial link. `raw_capture.py` records the raw bytes and hex-dumps anything that isn't text.
- `usb-echo`: echoes lines typed or sent by `echo_test.py`, with a quiet gap between lines (`--gap 30` repeats the pauses that come before the junk).
- `python3 -m serial.tools.miniterm /dev/ttyUSB0 115200` watches the port on dev10 without ESP-IDF. Opening it didn't reset the board.
