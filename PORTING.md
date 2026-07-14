# Porting to the ESP32-S3-Touch-LCD-1.28 (round, GC9A01)

The whole point of the architecture: **only the display backend and the I2C bus
setup change.** The 240×240 framebuffer, the animation, the IMU/QMI8658 driver,
the flick gesture, and the render loop are all shared in `components/eye_core/`
and move over untouched.

> **Status: this board is now wired up and buildable.** The GC9A01 backend, the
> `esp_lcd_gc9a01` dependency, the I2C bus, and the per-board sdkconfig are done.
> The one thing you MUST verify is the **SPI pin map** in
> `boards/lcd_1_28/main/eye_display_gc9a01.c` (see step 2) — if the screen stays
> black after flashing, that's almost certainly it.

## Board facts (from Waveshare docs)

| | AMOLED 2.06 (now) | LCD 1.28 (port) |
|---|---|---|
| Panel | 410×502, CO5300, **QSPI** | 240×240, GC9A01, **SPI** |
| Active area | ~33 mm wide | Φ32.4 mm round |
| Touch | FT3168 (I²C) | CST816S (I²C) |
| IMU | QMI8658 | **QMI8658 (same)** |
| SoC | ESP32-S3**R8**, 8 MB **octal** PSRAM, 32 MB flash | ESP32-S3**R2**, 2 MB **quad** PSRAM, 16 MB flash |

## What's already done

1. **1:1 push (no scaling).** `eye_display_gc9a01.c::eye_display_push_frame()`
   composes into a PSRAM ping-pong buffer (avoids tearing) and does a single
   `esp_lcd_panel_draw_bitmap()`. `scale < 100` shrinks + centers; the round panel
   can't overscan so `scale > 100` clamps to 1:1.
2. **GC9A01 driver dependency** added (`espressif/esp_lcd_gc9a01` in
   `boards/lcd_1_28/main/idf_component.yml`), backend enabled (no stub).
3. **IMU I²C bus** created on **SDA=GPIO6 / SCL=GPIO7** in `app_main.c`; the shared
   `eye_imu.c` probes the QMI8658 at 0x6B then 0x6A. Same flick gesture as AMOLED.
4. **sdkconfig** set for **quad** PSRAM / 16 MB flash, partition table wired.

## What YOU must verify on hardware

1. **SPI pin map** — top of `eye_display_gc9a01.c`. Confirmed from docs: `BL=2`,
   `SDA=6`, `SCL=7`. The SPI pins use Waveshare's standard 1.28″ demo mapping —
   **verify these against your schematic:**
   ```
   SCLK=10  MOSI=11  CS=9  DC=8  RST=14
   ```
   Black screen after flashing ⇒ fix these first (RST/DC/CS most likely).
2. **Color order / inversion** — the backend sets `invert_color(true)` and BGR
   element order (the usual GC9A01 combo). If colors look inverted, drop the
   invert; if red/blue are swapped, change `rgb_ele_order` to RGB.
3. **Orientation** — if the eye is mirrored/rotated on the glass, add
   `esp_lcd_panel_mirror()` / `esp_lcd_panel_swap_xy()` in the backend (never in
   the shared engine).
4. **Console** — left at the default UART0 (the board's USB-serial is on
   GPIO43/44 = UART0). If you can't type commands, switch it to USB-Serial-JTAG
   exactly like the AMOLED board: add `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` to
   `boards/lcd_1_28/sdkconfig.defaults` and delete `sdkconfig.lcd_1_28`.

## What you do NOT touch

`eye_config.h`, `eye_scene.[ch]`, `eye_info.[ch]`, `eye_imu.[ch]`,
`eye_demo.[ch]`. If you find yourself editing any of these to make a board
work, that's a smell — the board-specific concern probably belongs in the
board's backend instead. A new board must provide THREE small files: the
display backend (`eye_display.h` contract), a battery reader (`eye_battery.h`
contract — PMIC, ADC, or a stub reporting `present=false`), and `app_main.c`.

## Build & flash the 1.28″ board

Build it in its **own build directory** so it never collides with the AMOLED
build (different deps, sdkconfig, and partition table):

```sh
cd G:\ElectronicsProj\sharingan
idf.py -B build_1_28 -DSHARINGAN_BOARD=lcd_1_28 set-target esp32s3
idf.py -B build_1_28 -DSHARINGAN_BOARD=lcd_1_28 build
idf.py -B build_1_28 -DSHARINGAN_BOARD=lcd_1_28 -p <COM> flash monitor
```

The AMOLED board keeps building the normal way (`idf.py build`, default `build/`),
so you can flip between the two without clobbering either.

First build pulls `esp_lcd_gc9a01` into `managed_components/` (needs internet).
Expect on boot: `eye_disp_gc9a01: GC9A01 ready (240x240, 1:1)`.
