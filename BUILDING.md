# Building the Sharingan firmware

How firmware images are generated for the two boards in this repo. This is the
practical, copy-pasteable reference; the short version also lives in
[README.md](README.md) under "Build & flash".

One ESP-IDF project builds **one board at a time**. The board is a single
setting (`board.txt`); everything board-specific (display backend, battery
driver, Kconfig defaults, partition table, generated `sdkconfig.<board>`) is
keyed off it, so the two boards never clobber each other.

- `amoled_2_06` - Waveshare ESP32-S3-Touch-AMOLED-2.06 (410x502, CO5300 QSPI, LVGL)
- `lcd_1_28` - Waveshare ESP32-S3-Touch-LCD-1.28 (240x240, GC9A01 SPI, raw esp_lcd)

---

## 1. Toolchain

- **ESP-IDF v5.5.1**, installed at `C:\Users\nisch\esp\v5.5.1\esp-idf`.
- Target: **esp32s3** (both boards). Pinned in the top
  [CMakeLists.txt](CMakeLists.txt) so a fresh configure never defaults to plain
  `esp32`.
- Tools (compiler, ninja, python venv) under `C:\Users\nisch\.espressif`.

### Environment activation (important gotcha)

The stock `export.ps1` derives the Python venv name from whatever `python` is
first on `PATH`. On this machine that resolves to Python 3.13, but IDF actually
installed its venv as **`idf5.5_py3.11_env`**, so a plain
`. export.ps1` fails with *"virtual environment ... py3.13_env not found"*.

Activate by pointing `IDF_PYTHON_ENV_PATH` at the real venv first:

```powershell
$env:IDF_PYTHON_ENV_PATH = "$HOME\.espressif\python_env\idf5.5_py3.11_env"
. "$HOME\esp\v5.5.1\esp-idf\export.ps1"
```

After this, `idf.py` is on `PATH` for the rest of the shell session. The
Espressif-IDE (Eclipse) and the VS Code ESP-IDF extension handle this
automatically; the note above is only for a raw PowerShell terminal.

---

## 2. Pick the board

Selection precedence (first that is set wins), from the top
[CMakeLists.txt](CMakeLists.txt):

1. `-DSHARINGAN_BOARD=<name>` on the `idf.py` line (one-off override)
2. `$env:SHARINGAN_BOARD` (environment variable)
3. [board.txt](board.txt) - the easy way; first non-comment line
4. fallback: `amoled_2_06`

The easy way is to edit [board.txt](board.txt) to the single word `amoled_2_06`
or `lcd_1_28`. CMake reconfigures automatically on the next build and prints:

```
-- Sharingan board: amoled_2_06  (edit board.txt to switch)
```

Always confirm that line matches the board you intend to flash.

---

## 3. Build, flash, monitor

```powershell
cd G:\ElectronicsProj\sharingan
idf.py set-target esp32s3      # first time in a fresh build dir only
idf.py build
idf.py -p COM5 flash monitor   # use your board's COM port
```

- First build pulls the Waveshare BSP + LVGL into `managed_components/` (needs
  internet). `eye_core` is found via `EXTRA_COMPONENT_DIRS`, no registry publish.
- Output image: `build/sharingan_eye.bin`. The build footer prints the app-
  partition usage.
- Serial console (commands + logs) is on **USB-Serial-JTAG** (the USB-C port) on
  the AMOLED, UART on the LCD.

---

## 4. Where the config comes from (and a gotcha)

Each board gets its own generated config file and defaults:

- `sdkconfig.<board>` - the **active, generated** config the build reads
  (e.g. `sdkconfig.amoled_2_06`). Set by `SDKCONFIG` in the top CMakeLists.
- `boards/<board>/sdkconfig.defaults` - the seed values (PSRAM mode, 240 MHz,
  LVGL options, console channel, partition-table path, enabled fonts, ...).

**Gotcha:** `sdkconfig.defaults` is only applied when `sdkconfig.<board>` does
**not already** contain that key (i.e. on first configure). Editing a default
that already has a materialised value in `sdkconfig.<board>` will *not* take
effect on a normal rebuild. When you change `sdkconfig.defaults`, either:

- edit the same key directly in `sdkconfig.<board>` (fast, targeted), or
- `idf.py reconfigure` after deleting the stale value, or
- delete `sdkconfig.<board>` (or the whole `build/`) and rebuild to reseed.

(This is exactly why enabling `CONFIG_LV_FONT_MONTSERRAT_28` needed the value
set in `sdkconfig.amoled_2_06`, not just in the defaults.)

Partition tables also differ per board: `boards/<board>/partitions.csv`,
selected via the defaults.

---

## 5. Switching boards

Editing [board.txt](board.txt) and rebuilding in the same `build/` dir triggers
a near-full recompile (different components + PSRAM settings) - expected. To keep
two warm builds, give each its own build dir; the `-D` flag overrides
`board.txt` as a one-off:

```powershell
idf.py -B build_amoled build flash                    # AMOLED (uses board.txt only if no -D)
idf.py -B build_1_28 -DSHARINGAN_BOARD=lcd_1_28 build flash
```

---

## 6. Clips (not part of the app image)

- **AMOLED**: clips are read from the SD card folder `/sdcard/eye` at runtime -
  no reflash. Copy `*.eyv` files onto the card (build them with
  `tools/eye_animator.py` or `tools/make_anim.py`).
- **LCD 1.28**: clips live in the flash `anim` data partition (FATFS); see
  [README.md](README.md) for the image/upload flow.

---

## 7. Quick troubleshooting

- **`idf.py` not recognised** - environment not activated; see section 1.
- **`virtual environment ... py3.13_env not found`** - set `IDF_PYTHON_ENV_PATH`
  to the `idf5.5_py3.11_env` venv before sourcing `export.ps1`.
- **A `sdkconfig.defaults` change didn't take** - see the gotcha in section 4.
- **Wrong board flashed** - check the `-- Sharingan board: <name>` CMake line and
  `board.txt`.
- **Font / symbol `undeclared` at compile** - the matching `CONFIG_LV_FONT_*`
  isn't set in the active `sdkconfig.<board>`.
