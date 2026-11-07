# Build, flash layout and memory

How the `ambient` firmware is built, where it sits in flash, and what uses the memory. Figures are from `idf.py size` and `idf.py size-components` at commit 8f0aace (2026-11-07). Firmware decisions are in [FIRMWARE.md](FIRMWARE.md).

## Memory use

**Table 1: Memory regions**

| Region | Used | Size | Free |
|---|---|---|---|
| App partition (flash) | 975 KB | 1024 KB | 72 KB |
| IRAM | 103 KB (78 %) | 128 KB | 28 KB |
| DRAM, static | 39 KB (22 %) | 177 KB | 141 KB, left for the heap |
| RTC slow | 64 B | 8 KB | 8 KB |

Runtime heap use (WiFi buffers, sockets, the FAT cache) isn't measured yet. A free-heap line in `status` would show it.

**Table 2: What the image is made of (68 components, KB)**

| Group | Libraries | KB |
|---|---|---|
| WiFi stack | net80211, wpa_supplicant, pp, phy | 370 |
| Crypto (WPA, TLS) | tfpsacrypto | 105 |
| TCP/IP | lwip | 93 |
| stdio (printf, console I/O) | esp_stdio | 80 |
| Core system | freertos, esp_hw_support, libc, spi_flash, nvs_flash, esp_system | 115 |
| FAT and SD | fatfs, sdmmc | 39 |
| **Our code** | main | **18** |
| 52 small components | see below | 166 |

The total is about 985 KB. That includes about 20 KB of `.bss`, which uses RAM but isn't stored in the image, so the image itself is 975 KB.

The 52 small components are each under 14 KB:
- Peripheral drivers (uart, spi, i2c, gpio, sdspi, adc): 52 KB
- System plumbing (ringbuf, libc, vfs, timer, log, pthread, common, stdc++): 25 KB
- HAL and low-level hardware (hal_*, soc, xtensa, mm, rom, efuse, clock, pmu, wdt): 22 KB
- Network glue (netif, event, esp_wifi, wifi, phy, coex): 20 KB
- Console: 13 KB
- Heap allocator: 13 KB
- Boot, partition and app-image support: 5 KB
- Odds and ends: about 16 KB

**What this means:**
- ESP-IDF is about 957 KB, 98 % of the image. Our code is 18 KB.
- A WiFi project with nothing else is about 800 KB (an estimate, not measured), which leaves about 200 KB of the default 1 MB partition. The extra IDF parts this firmware uses (console, FAT and SD, SPI and I2C drivers, more stdio) add about another 130 KB, so only 72 KB is left.
- New code of our own is cheap. Each new IDF component pulled in costs far more.

## Flash layout

The ESP32 here has 4 MB of flash (`CONFIG_ESPTOOLPY_FLASHSIZE_4MB` in `sdkconfig.defaults`). The firmware uses IDF's default single-app table (`partitions_singleapp.csv`).

**Table 3: Current flash layout**

| Offset | Contents | Size |
|---|---|---|
| 0x1000 | Second-stage bootloader | |
| 0x8000 | Partition table (binary) | 3 KB |
| 0x9000 | `nvs`, the settings | 24 KB |
| 0xF000 | `phy_init`, RF calibration | 4 KB |
| 0x10000 | `factory`, the app | 1 MB |
| 0x110000 to the end | unused | about 2.9 MB |

**Why the default is so small:** it was chosen to run on every chip. IDF assumes a 2 MB flash unless told otherwise, and 1 MB apps also fit the two-slot OTA table on 4 MB parts. The WiFi stack has grown with each IDF release, but the default hasn't. The expected path is a custom `partitions.csv`.

### Planned: custom table with a 3 MB app (separate project, not done)

1. Add `ambient/partitions.csv`, with the first two rows the same as the default so NVS stays at 0x9000:
   ```
   nvs,      data, nvs,     0x9000,  0x6000,
   phy_init, data, phy,     0xf000,  0x1000,
   factory,  app,  factory, 0x10000, 3M,
   ```
2. Add these lines to `sdkconfig.defaults`:
   ```
   CONFIG_PARTITION_TABLE_CUSTOM=y
   CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
   ```
   The existing `sdkconfig` doesn't pick up new defaults, so delete it (or set the options in `menuconfig`) and rebuild.
3. Run `./flash.sh`. It writes the bootloader, the new table and the app. NVS isn't touched unless someone runs `erase-flash`.

The app goes from 1 MB to 3 MB, and about 960 KB at the end stays unused. For OTA later, the 3 MB app would be replaced by `otadata` and two app slots of about 1.5 MB each. That means repartitioning again, but NVS stays put.

Keeping NVS in place means the settings survive: WiFi SSID and password, the listener, the intervals, `clock auto/report` and the test flags. These are set only on the console now. If NVS moved, they would have to be typed in again. It's a small saving, but it costs nothing.

## NVS and wear

**NVS (Non-Volatile Storage)** is IDF's key-value store in its own flash partition. It holds named ints, strings and blobs through reboots and app reflashes. The `config` settings live there, and so does some WiFi driver state.

**Wear:** a flash sector (4 KB) survives roughly 100,000 erase cycles. Each `./flash.sh` erases the app's sectors once, so even 1,000 flashes in development use about 1 % of their life. Wear matters only for data that the running firmware writes again and again.

**How NVS levels wear.** It's a log that is scanned at boot, with no pointer table in flash:
- Each 4 KB sector is a page with a header: its state (empty, active, full, freeing) and a sequence number.
- A page holds 126 entries of 32 bytes (namespace, key, type, value, CRC). Long strings and blobs span several entries.
- An erase sets bits to 1, and a write can only turn bits to 0. Each page has a bitmap with 2 bits per entry (empty, written, erased), so marking an entry stale is a write, not an erase.
- A changed value is appended to the active page, and the old entry is marked erased.
- One page is always kept spare. When space runs out, the live entries of the oldest full page are copied to the spare page, and the old page is erased and becomes the new spare.
- At boot NVS reads every page in sequence order and builds a hash table in RAM from key to page and entry. Keeping the map in RAM rather than in flash matters: a map in flash would rewrite its own sector at every change and wear that sector out first.

Keeping NVS at 0x9000 is only about keeping the settings. It has nothing to do with wear.

The sensor logs go to the SD card, not internal flash. The card's controller does its own wear levelling, and the card can be replaced.

## The build

`./build.sh` runs `idf.py build`, which runs CMake, which then runs **make**. On this VM CMake generates Unix Makefiles (see `CMAKE_GENERATOR` in `build/CMakeCache.txt`). IDF prefers Ninja, which does the same job and is faster on small rebuilds, but it isn't installed. Installing it is deferred. If it's installed later, delete `build/` and `build-test/`, because the generator is fixed when a build directory is created.

**Table 4: Build steps**

| Step | Tool | Output |
|---|---|---|
| Compile | `xtensa-esp32-elf-gcc` | `.o` per source file |
| Archive | `xtensa-esp32-elf-ar` | `.a` per component (`libmain.a`, `liblwip.a`, …) |
| Link | `ld` (via gcc) with IDF's linker scripts | `ambient.elf` |
| Convert | `esptool elf2image` | `ambient.bin`, the flashable image |
| Partition table | `gen_esp32part.py` reads the CSV | `partition-table.bin` |
| Size check | `check_sizes.py` reads the CSV | fails the build if the app is bigger than its partition |
| Flash arguments | CMake reads the CSV for offsets | `flasher_args.json` |
| Bootloader | the same steps, as a separate subproject | `bootloader.bin` |

**Who uses the partition CSV:** only the three build helpers above. The compiler and linker never see it, and nothing reads the CSV after the build. On the chip, the bootloader and the firmware read the binary copy at 0x8000.

**Addresses:** the compiler emits relocatable code with no addresses. The linker assigns them from IDF's linker scripts:
- Flash code and data are linked to fixed virtual addresses (code at 0x400D…, rodata at 0x3F40…). Physical flash offsets never reach the toolchain.
- IRAM and DRAM are real internal RAM addresses. RAM isn't remapped, but these addresses are fixed by the chip, not by the partition table.

**Flashing:** esptool writes each `.bin` at the offset given in `flasher_args.json`. The partition table isn't instructions to esptool; it's just one of the files it writes.

## Boot

There is no OS loader.

1. The ROM bootloader in the chip loads IDF's second-stage bootloader from 0x1000.
2. The second-stage bootloader reads the partition table at 0x8000 and finds the app.
3. It programs the flash MMU so the app partition's sectors appear at the app's virtual addresses, copies the IRAM and DRAM parts into RAM, and jumps to the app.
4. The running firmware reads the table to find `nvs` and `phy_init`.

Because of the MMU mapping, the same image runs from any app partition. That's how OTA can boot either slot.
