# Sharingan Eye — Build & Flash Reference

## Project layout

One IDF project, two boards. The active board is set in `board.txt` (one word,
no other changes needed). Each board keeps its own `sdkconfig.<board>` and a
dedicated build directory so switching boards never contaminates the other.

```
board.txt          ← change this to switch target (amoled_2_06 | lcd_1_28)
boards/
  amoled_2_06/     ← Waveshare AMOLED 2.06  (COM4 in current setup)
    partitions.csv
    sdkconfig.defaults
    main/
  lcd_1_28/        ← Waveshare LCD 1.28     (COM5 in current setup)
    partitions.csv
    sdkconfig.defaults
    main/
build_amoled/      ← AMOLED build artifacts (gitignored)
build_1_28/        ← LCD 1.28 build artifacts (gitignored)
```

---

## One-time IDF environment setup (PowerShell)

Run this once per PowerShell session before any `idf.py` commands:

```powershell
$env:IDF_PYTHON_ENV_PATH = "$HOME\.espressif\python_env\idf5.5_py3.11_env"
. "$HOME\esp\v5.5.1\esp-idf\export.ps1" *> $null
Set-Location "G:\ElectronicsProj\sharingan"
```

---

## Build

### AMOLED 2.06

```powershell
# Switch board (if not already set)
(Get-Content board.txt) -replace '^lcd_1_28', '#lcd_1_28' |
  ForEach-Object { $_ -replace '^#?amoled_2_06', 'amoled_2_06' } |
  Set-Content board.txt

idf.py -B build_amoled build
```

### LCD 1.28

```powershell
# Switch board (if not already set)
(Get-Content board.txt) -replace '^amoled_2_06', '#amoled_2_06' |
  ForEach-Object { $_ -replace '^#?lcd_1_28', 'lcd_1_28' } |
  Set-Content board.txt

idf.py -B build_1_28 build
```

Both commands: CMake re-runs automatically when `board.txt` changes; only
changed source files recompile.

---

## Flash (first time / partition table change)

The first flash after a partition table change must write bootloader +
partition table + otadata + app together. `idf.py flash` does this:

### AMOLED on COM4

```powershell
idf.py -B build_amoled -p COM4 flash
```

Writes (in order):
| Address    | File                                        |
|------------|---------------------------------------------|
| `0x0`      | `build_amoled/bootloader/bootloader.bin`    |
| `0x8000`   | `build_amoled/partition_table/partition-table.bin` |
| `0x10000`  | `build_amoled/ota_data_initial.bin`         |
| `0x20000`  | `build_amoled/sharingan_eye.bin`            |

### LCD 1.28 on COM5

```powershell
idf.py -B build_1_28 -p COM5 flash
```

Writes (in order):
| Address    | File                                         |
|------------|----------------------------------------------|
| `0x0`      | `build_1_28/bootloader/bootloader.bin`        |
| `0x8000`   | `build_1_28/partition_table/partition-table.bin` |
| `0x10000`  | `build_1_28/ota_data_initial.bin`             |
| `0x20000`  | `build_1_28/sharingan_eye.bin`                |

> **COM port busy?** Close the VS Code serial monitor (the plug icon in the
> ESP-IDF toolbar) or any other terminal holding the port before flashing.

---

## Flash (OTA — after first flash)

Once the board is running OTA-capable firmware you can update it wirelessly:

1. Build the new firmware: `idf.py -B build_amoled build`
2. Open the board's web UI (connect to `SharinganEye-XXXX` AP or via home
   WiFi at `http://sharingan-eye.local`)
3. Go to **System → Firmware update (OTA)**
4. Select `build_amoled/sharingan_eye.bin`
5. The board flashes and reboots automatically

> **Note:** The `.bin` file must be built for the correct board. Do not flash
> an AMOLED binary onto the LCD board or vice versa.

---

## Monitor (serial log)

```powershell
# AMOLED
idf.py -B build_amoled -p COM4 monitor

# LCD
idf.py -B build_1_28  -p COM5 monitor

# Exit monitor: Ctrl+]
```

## Build + flash + monitor in one go

```powershell
idf.py -B build_amoled -p COM4 flash monitor
idf.py -B build_1_28  -p COM5 flash monitor
```

---

## Full erase (factory reset NVS / WiFi credentials)

```powershell
idf.py -B build_amoled -p COM4 erase-flash
idf.py -B build_1_28  -p COM5 erase-flash
# Then reflash normally afterwards
```

---

## VS Code (ESP-IDF extension)

The extension reads `board.txt` via CMake on every build, so the workflow is:

1. Edit `board.txt` to the target board name
2. **ESP-IDF: Build** (Ctrl+E B)
3. Select the COM port in the status bar
4. **ESP-IDF: Flash** (Ctrl+E F)

The extension uses `build/` (not `build_amoled/` or `build_1_28/`) as its
output directory, so CLI and VS Code builds are in separate trees — they don't
interfere.

---

## Partition tables

Both boards use OTA-capable layouts (2 × 2MB app slots):

### AMOLED 2.06 (`boards/amoled_2_06/partitions.csv`)

| Name     | Type | SubType | Offset   | Size |
|----------|------|---------|----------|------|
| nvs      | data | nvs     | 0x9000   | 24K  |
| phy_init | data | phy     | 0xf000   | 4K   |
| otadata  | data | ota     | 0x10000  | 8K   |
| ota_0    | app  | ota_0   | 0x20000  | 2M   |
| ota_1    | app  | ota_1   | 0x220000 | 2M   |

### LCD 1.28 (`boards/lcd_1_28/partitions.csv`)

| Name     | Type | SubType | Offset   | Size |
|----------|------|---------|----------|------|
| nvs      | data | nvs     | 0x9000   | 24K  |
| phy_init | data | phy     | 0xf000   | 4K   |
| otadata  | data | ota     | 0x10000  | 8K   |
| ota_0    | app  | ota_0   | 0x20000  | 2M   |
| ota_1    | app  | ota_1   | 0x220000 | 2M   |
| anim     | data | fat     | 0x420000 | 10M  |

Current firmware size: ~1.47MB (28% free in 2MB slot).
