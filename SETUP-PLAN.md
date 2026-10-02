# Set up an ESP32 C dev environment parallel to the Pico setup

## Context

You have a working Pico dev setup split into two trees:
- `~/dev/pico/` — shared SDK/toolchain area: separately-cloned upstream repos (`pico-sdk`, `pico-examples`, `openocd` built from source, `debugprobe`), not itself a git repo.
- `~/dev/github/pico-dev/` — a git-tracked workspace of independent per-board CMake projects (`gmcount`, `rtc`, `sdsc`, `wifi`, `wifi2`, `temp-sense`), each with its own `wbuild.sh`/`load_*.sh`/`start_minicom.sh` and a `common/` folder for shared code (wifi, mqtt).

You now want an equivalent, separate environment for ESP32 C development (target board: an Elegoo ESP-WROOM-32 dev board, USB-C, WiFi + BT 4.2 — this is the original ESP32, Xtensa LX6, no built-in USB-JTAG, flashed over its USB-UART bridge). Goal: mirror the Pico split (shared toolchain tree + git project workspace) using Espressif's official ESP-IDF, Espressif's standard C SDK and CMake-based build system — the direct analogue of `pico-sdk`.

Key differences from the Pico setup to account for:
- ESP-IDF projects have a fixed layout (`CMakeLists.txt` + `main/CMakeLists.txt` + `main/*.c`), not a single flat `CMakeLists.txt` like Pico.
- ESP-IDF ships its own build driver (`idf.py`) and bundles `esptool.py` for flashing — no separate OpenOCD/debug-probe step is needed for ordinary flashing (unlike Pico's SWD-based flash-via-debug-probe). JTAG hardware debugging is possible later but needs an external adapter (this board has no built-in USB-JTAG) — out of scope for now.
- Toolchain isn't installed via pacman (no ESP32 xtensa cross-compiler exists in Arch's repos); it's fetched by ESP-IDF's own `install.sh` into a tools directory of our choosing.
- Board enumerates as a USB-serial device (e.g. `/dev/ttyUSB0`), not a debug-probe CDC-ACM port — need `uucp`/`dialout`(Arch: `uucp`) group membership for the device, no udev/VBox passthrough script needed unless this session turns out to run inside a VM (flag if so).

### The board, identified (2026-09-26)

The Amazon listing ("Elegoo ESP-WROOM-32, USB-C") names the seller and the module, not the board design. From the outside in:

| layer | what | source |
|---|---|---|
| seller | Elegoo | Amazon listing |
| board | **ESP32 DEVKITV1**, 30-pin | silkscreen, bottom of board |
| module | can marked only "ESP-32 / wifi + bt - SoC inside / ISM2.4G 802.11/b/g/n": no module name, no logo, no FCC ID. Probably a third-party module around an Espressif chip. The listing calls it ESP-WROOM-32. | can marking (read by the user) |
| SoC | **ESP32-D0WD-V3**, revision v3.1, dual core, 240 MHz max | `esptool flash-id` |
| crystal | 40 MHz | `esptool flash-id`; pinned in each project's `sdkconfig.defaults.esp32` |
| flash | 4 MB, JEDEC manufacturer `0x5e`, device `0x4016`; 3.3 V (set by a strapping pin) | `esptool flash-id` |
| USB-UART bridge | Silicon Labs **CP2102** (`10c4:ea60`, driver `cp210x`) | `lsusb`, `udevadm info` |
| MAC | `8c:94:df:4d:05:50` | `esptool flash-id` |

Notes:
- "DEVKITV1" is a common clone layout (after the DOIT DevKit V1), not an Espressif board, so pin positions can vary between makers. Check the silkscreen before wiring.
- Chip revision vs. the module name: as I recall, the original ESP-WROOM-32 used the D0WDQ6 chip, the -32D the D0WD (revision 1), and the -32E the D0WD-V3 (revision 3). This board's revision 3 chip matches Espressif's -32E, but the can's generic marking points to a third-party module. The chip itself is probably genuine: esptool reads its model and revision from the chip's eFuses. Not checked against Espressif's module datasheets.
- Unverified: which vendor JEDEC ID `0x5e` belongs to (possibly Zbit), and whether the MAC's OUI (`8c:94:df`) is registered to Espressif. Either would say more about where the flash and module came from.
- To reread any of this: `python -m esptool -p /dev/ttyUSB0 flash-id` (with the ESP-IDF environment exported), then `lsusb`.

### The three boards (2026-10-02)

Labels are paint-pen dots on each board: 1, 2 or 3 dots.

| dots | MAC | SoC | crystal | flash | USB-UART | blink | use |
|---|---|---|---|---|---|---|---|
| 1 | `8c:94:df:4d:05:50` | ESP32-D0WD-V3 v3.1 | 40 MHz | `0x5e`/`0x4016`, 4 MB, 3.3 V | CP210x | pass (2026-09-26) | bench: thermistor-cal (ADS-A) |
| 2 | `30:76:f5:92:5d:84` | ESP32-D0WD-V3 v3.1 | 40 MHz | `0x5e`/`0x4016`, 4 MB, 3.3 V | CP210x | pass | perf-board ambient logger |
| 3 | `8c:94:df:46:43:b4` | ESP32-D0WD-V3 v3.1 | 40 MHz | `0x5e`/`0x4016`, 4 MB, 3.3 V | CP210x | pass | spare |

Notes:
- Board 2's MAC starts with a different maker prefix (`30:76:f5`) from boards 1 and 3 (`8c:94:df`). The chip, crystal and flash are the same, so the difference may only be a different production batch. Not checked.
- Boards 2 and 3: seller and bottom silkscreen not yet recorded.
- Board 3's first `flash-id` misread the crystal (15.55 MHz) and the stub flasher didn't start. Two retries were clean. Board 1 had a similar one-off failure at its first flash. Put down to the serial link through the VM.
- Serial captures in this VM can replay one old chunk of output many times when the port opens (identical lines, same timestamp). On board 2 this replayed the boot banner and looked like a reset loop; it wasn't.

## Approach

### 1. Shared toolchain tree — `~/dev/esp32/`

- `git clone --recursive https://github.com/espressif/esp-idf.git ~/dev/esp32/esp-idf` (checkout a stable release branch/tag, e.g. latest `release/v5.x`, matching what's current).
- Run the installer targeting this board's chip only, keeping tools self-contained under our own tree (parallel to how Pico keeps everything under `~/dev/pico/`) rather than the default `~/.espressif`:
  ```bash
  cd ~/dev/esp32/esp-idf
  IDF_TOOLS_PATH=~/dev/esp32/tools ./install.sh esp32
  ```
- Verify prerequisites first: `python3`, `cmake`, `git` are already present; confirm `ninja` is installed (`pacman -Qs ninja`) and install it via pacman if missing — it's ESP-IDF's preferred generator.
- No persistent env var — same pattern as `PICO_SDK_PATH`: each project's build script sources ESP-IDF's `export.sh` (which sets `IDF_PATH`, `PATH`, and activates the Python venv) inline, pointing at `IDF_TOOLS_PATH=$HOME/dev/esp32/tools`.

### 2. Project workspace — `~/dev/github/esp32-dev/`

`git init` a new repo (left local; creating/pushing a GitHub remote is left to you as a manual follow-up, same as `pico-dev`'s existing `origin`). Structure:

```
esp32-dev/
├── .gitignore                  # build/, sdkconfig (generated), wifi_secrets.h — mirrors pico-dev's .gitignore
├── common/                     # shared ESP-IDF components (wifi, mqtt, etc.), added per-project via EXTRA_COMPONENT_DIRS
└── <first-project>/            # e.g. "blink" as a smoke test, then real projects
    ├── CMakeLists.txt          # top-level: include($ENV{IDF_PATH}/tools/cmake/project.cmake); project(name)
    ├── sdkconfig.defaults      # board defaults (flash size, etc.) — committed, unlike generated sdkconfig
    ├── main/
    │   ├── CMakeLists.txt      # idf_component_register(SRCS "main.c" INCLUDE_DIRS ".")
    │   └── main.c
    ├── build.sh                # source $HOME/dev/esp32/esp-idf/export.sh (IDF_TOOLS_PATH=$HOME/dev/esp32/tools); idf.py build
    ├── flash.sh                # idf.py -p /dev/ttyUSB0 flash
    └── monitor.sh              # idf.py -p /dev/ttyUSB0 monitor
```

This mirrors `pico-dev`'s pattern of a `wbuild.sh`/`load_*.sh`/`start_minicom.sh` trio per project, adapted to ESP-IDF's own tooling (`idf.py build` / `idf.py flash` / `idf.py monitor` — these can also be combined as `idf.py flash monitor` in one script if you'd rather have fewer scripts).

Reuse the existing `.gitignore` from `/home/mike/dev/github/pico-dev/.gitignore` as a starting point (it already excludes `build/`, `wifi_secrets.h`, and personal `.claude/settings.json` — all applicable here too), adding `sdkconfig` and `managed_components/` (ESP-IDF's fetched-component cache, analogous to a lockfile-managed dependency dir).

### 3. First project as a smoke test

Create a minimal `blink`-style project (or `hello`, using `idf.py create-project` scaffolding) to prove the toolchain end-to-end: build → flash → monitor over `/dev/ttyUSB0`.

### 4. Hardware access (VM guest)

Dev and build run inside a VM (same as the Pico setup), so the Elegoo board's USB-UART device needs to be passed through to the guest before `/dev/ttyUSB0` will appear there at all:

- Attach the board's USB-C connection to the guest the same way Pico's debug probe is attached — either manually via the VM software's Devices → USB menu, or by reviving/adapting the archived `pico-dev/vbox-usb-attach/vbox-usb-attach.py` helper (it currently targets the debug probe + a USB-serial adapter's vendor/product IDs; it would need the Elegoo board's own USB-UART bridge VID:PID added, found via `lsusb` on the host once plugged in).
- Once passed through and visible in the guest (`lsusb` / `ls /dev/ttyUSB*` inside the VM), add your user to the serial-port group Arch uses for USB-UART devices (`uucp`) if not already a member, so `/dev/ttyUSB0` is accessible without sudo: `sudo usermod -aG uucp $USER` (requires logout/login, or a fresh VM console session, to take effect).

## Adding other ESP32 models later

The toolchain install and per-project chip selection are separate steps, so supporting a new ESP32 variant later doesn't touch the existing setup:

- **Toolchains are additive.** Re-run the installer with a comma-separated target list (or `all`):
  ```bash
  cd ~/dev/esp32/esp-idf
  IDF_TOOLS_PATH=~/dev/esp32/tools ./install.sh esp32,esp32s3,esp32c3
  ```
  Each target's cross-compiler (Xtensa for esp32/esp32s3, RISC-V for esp32c3/c6) is installed side-by-side under `~/dev/esp32/tools` — the original `esp32` toolchain isn't replaced.
- **Per-project chip selection** happens inside each project via `idf.py set-target esp32s3` (etc.), which regenerates that project's `sdkconfig` for the new chip — analogous to how a Pico project's `wbuild.sh` passes `-DPICO_BOARD=pico_w` vs. `wifi2`'s RP2350 target.
- A board revision using the *same* chip needs no toolchain change at all — only a genuinely different chip (Xtensa vs. RISC-V variant) requires an `install.sh` re-run.

## Verification

1. `idf.py --version` succeeds after sourcing `export.sh`, confirming the toolchain installed correctly.
2. Build the smoke-test project: `idf.py build` completes without error and produces a `.bin` in `build/`.
3. Flash it to the Elegoo board over USB-C: `idf.py -p /dev/ttyUSB0 flash`.
4. `idf.py -p /dev/ttyUSB0 monitor` shows expected serial output (e.g. blink log lines or boot banner); confirms WiFi/BT-capable chip is recognized correctly in the build (`idf.py menuconfig` target defaults to `esp32`).
5. `git status` in `esp32-dev/` shows a clean tree with only intended files tracked (build artifacts excluded per `.gitignore`).
