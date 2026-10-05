# ESP32 dev environment: quick reference

For looking things up rather than remembering them. The background and install history are in `SETUP-PLAN.md`.

## Where things live

| what | path |
|---|---|
| ESP-IDF (SDK) | `~/dev/esp32/esp-idf`, branch `release/v6.1` (27506a49) |
| Toolchains + Python env | `~/dev/esp32/tools` (non-default; the default is `~/.espressif`) |
| Projects (git) | `~/dev/github/esp32-dev/<project>/` |
| Shared library | `~/dev/github/ads1115-dev` (sibling checkout, pulled in via `EXTRA_COMPONENT_DIRS`) |

## The environment step

Every script starts with:

```bash
export IDF_TOOLS_PATH=$HOME/dev/esp32/tools   # tools aren't in ~/.espressif
source $HOME/dev/esp32/esp-idf/export.sh      # IDF_PATH, PATH (idf.py, xtensa gcc, esptool), Python venv
```

Sourcing is always needed, wherever the tools live. It lasts only for that shell, so the login shell stays clean. To use `idf.py` by hand, run those two lines in the terminal first.

## Per-project scripts

Run these from the project directory, e.g. `thermistor-cal/`.

| script | does | Pico equivalent |
|---|---|---|
| `build.sh` | `idf.py build` (cmake + ninja into `build/`) | `wbuild.sh` / `xbuild.sh` (cmake + make) |
| `flash.sh` | `idf.py -p /dev/ttyUSB0 flash` (esptool over USB serial) | `load_*.sh` (OpenOCD over SWD, with sudo) |
| `flash-check.sh` | flash, keep the full log in `build/flash-check.log`, print a summary (crystal, writes, hash checks) | — |
| `monitor.sh` | `idf.py -p /dev/ttyUSB0 monitor`; **exit with Ctrl-]**. `ambient` and `blink` add `--no-reset` (the board keeps running). `thermistor-cal` resets the board at open and saves the console to `build/monitor.log` | `start_minicom.sh` |

## Project layout

```
<project>/
├── CMakeLists.txt            # includes $IDF_PATH/tools/cmake/project.cmake
├── main/CMakeLists.txt       # idf_component_register(...)
├── main/main.c               # app_main()
├── sdkconfig.defaults        # committed: target esp32, 4 MB flash
├── sdkconfig.defaults.esp32  # committed: 40 MHz crystal pinned
└── sdkconfig                 # generated from the defaults; delete it to regenerate
```

## Hardware and host

- This dev machine is a VirtualBox guest. Pass the board through to the VM before `/dev/ttyUSB0` appears.
- Board: ESP32 DEVKITV1 (30-pin), ESP32-D0WD-V3 rev v3.1, 40 MHz crystal, 4 MB flash. Details in `SETUP-PLAN.md`.
- USB-UART bridge: CP2102 (`10c4:ea60`), which appears as `/dev/ttyUSB0`, group `uucp`. The user is in `uucp`, so no sudo is needed.
- The console runs at 115200 baud. Garbage at the very start of boot output is the ROM talking at 74880. It's harmless.

## Known quirks

- **Flash fails at the stub flasher:** the serial link is flaky. Retry. `flash-check.sh` keeps the log.
- **"port is busy":** a monitor is still open. Exit it with Ctrl-], or check with `fuser -v /dev/ttyUSB0`.
- **Changed `sdkconfig.defaults*` but nothing changed:** the old `sdkconfig` wins. Delete it (or run `idf.py fullclean`) and rebuild.
- **Different chip (S3, C3...):** run `install.sh <target>` in `esp-idf` with the same `IDF_TOOLS_PATH`, then `idf.py set-target <target>` in the project.

## Handy by hand (after the environment step)

```bash
idf.py --version
idf.py menuconfig                                 # edits sdkconfig
idf.py size                                       # memory use
python -m esptool -p /dev/ttyUSB0 flash-id        # chip, crystal, flash ID, MAC
```
