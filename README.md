# Sharingan — animated cosplay eye

A looping Sharingan/Rinnegan-style "eye" for an Obito (Naruto) cosplay mask: a
small round display behind a magnifying lens playing a board-agnostic animation.

This is a **standalone project**, separate from ScreenStreamWatch. It only
*reuses* the same Waveshare display bring-up by depending on the published BSP —
it does not copy or fork that firmware.

## Why it's structured this way

All animation logic renders into a fixed **240×240 RGB565** framebuffer that
knows nothing about the panel. The only board-specific code is two functions —
`eye_display_init()` and `eye_display_push_frame()`. So the same engine drives
both the board you have now and the round board arriving later; porting is a new
~120-line display backend, nothing else.

`sharingan/` is a single ESP-IDF project that builds ONE board at a time,
selected by `SHARINGAN_BOARD` (default: the AMOLED 2.06). Each board's firmware,
Kconfig defaults, and partition table live under `boards/<board>/`, so the two
firmwares stay separate — you just build from the repo root.

```
sharingan/
├── CMakeLists.txt                # THE project root; selects the board to build
├── components/
│   └── eye_core/                 # SHARED, board-agnostic engine (no BSP, no panel)
│       ├── eye_config.h          #   240×240 buffer geometry + RGB565 helper
│       ├── eye_params.[ch]       #   tunable params, scene table, NVS save/load
│       ├── eye_scene.[ch]        #   the realistic eye engine (8 dojutsu scenes)
│       ├── eye_info.[ch]         #   boot info screen (version, battery, uptime)
│       ├── eye_font.h            #   8x12 bitmap font (generated: tools/gen_font.py)
│       ├── eye_battery.h         #   battery contract (implemented per board)
│       ├── eye_imu.[ch]          #   QMI8658 accel + gyro flick gesture
│       ├── eye_console.[ch]      #   serial command REPL (live tuning)
│       ├── eye_demo.[ch]         #   render loop + screen list + gesture task
│       └── eye_display.h         #   the 2-function board contract
│
└── boards/                       # one folder per board => separate firmware
    ├── amoled_2_06/              # ESP32-S3-Touch-AMOLED-2.06 (default)
    │   ├── sdkconfig.defaults    #   octal PSRAM / 240MHz / LVGL
    │   ├── partitions.csv
    │   └── main/
    │       ├── app_main.c            # board setup, then eye_demo_run()
    │       ├── eye_display_amoled.c  # scales 240→410 (panel width) + centers
    │       ├── eye_battery_axp2101.c # AXP2101 PMIC fuel gauge (I2C)
    │       └── idf_component.yml     # waveshare AMOLED BSP + lvgl
    │
    └── lcd_1_28/                 # ESP32-S3-Touch-LCD-1.28 (round GC9A01)
        └── main/
            ├── app_main.c
            ├── eye_display_gc9a01.c  # raw esp_lcd, 1:1 push
            └── eye_battery_adc.c     # battery voltage via GPIO1 ADC (/3 divider)
```

## Screens — one flat cycle

The device boots on an **info screen**, and a **gyro flick** steps through one
flat list that wraps around; flicking the **opposite direction goes back**:

```
info  ->  clip 1 ... clip N (SD/flash playlist)  ->  8 dojutsu scenes  ->  info ...
```

The **info screen** shows firmware version, build date/time, IDF version, board
name, **battery level** (bar + percent + voltage + charging state), clip/scene
counts and uptime. Battery comes from the **AXP2101 PMIC** on the AMOLED (a
real fuel gauge) and from the **GPIO1 ADC** (÷3 divider, LiPo-curve estimate)
on the LCD 1.28.

## The dojutsu scenes (in-house animation engine)

`eye_scene.c` draws a **realistic anime eye**, not a flat pattern: an eyelid
silhouette on near-black skin, sclera with corner/lid shading, a big iris with
the dojutsu pattern, and a specular glint. It **animates like the reference
GIFs**: the eye *opens* when you arrive at a scene, *blinks* naturally at random
intervals, the gaze makes subtle saccades, the pattern spins slowly and
occasionally *flares* (fast spin burst + pupil constriction), and switching
scenes plays a **blink-morph** (close → pattern changes → reopen).

| # | Scene | Look |
|---|-------|------|
| 0-2 | **tomoe1/2/3** | red Sharingan with 1/2/3 spinning tomoe |
| 3 | **mangekyou** | Obito/Kakashi pinwheel Mangekyou |
| 4 | **itachi** | Itachi's triple-blade Mangekyou |
| 5 | **rinnegan** | pale-violet ripple rings filling the whole eye |
| 6 | **tomoerin** | Rinnegan rings + 9 tomoe (Sasuke style) |
| 7 | **byakugan** | pale, pupil-less |

Every scene shares the same eye + behavior knobs (`set sspin/pupil/blink/gaze/
flare`, `color`), so you can recolor or retime any of them live.

## Switching screens — flick gesture

Give the board a quick **wrist flick** (a fast ~0→90°→back rotation, under
~500 ms) to go to the **next screen**; flick the **other way for the previous
screen**. The flick's rotation direction picks forward/backward. It latches, so
the new screen stays. This uses the QMI8658 **gyro** (a rate spike), sampled at
100 Hz in its own task.

The trigger defaults to **650 °/s** so only a deliberate flick fires it (casual
head/mask movement stays well under). Tune it live: **`set fhi <deg/s>` — higher
= less sensitive** (try 800–1000 if it still triggers too easily; 400 to make it
easier). `set flo` is the re-arm threshold; `flick off` disables it. If no IMU is
detected the demo still runs (use the console to switch).

## Serial console (live tuning)

Open the serial monitor (`idf.py -p COM4 monitor`, or the IDE's monitor), **click
into the terminal**, type a command and press **Enter** at the `eye>` prompt.
Input uses linenoise *dumb mode*, so it works even in monitors that don't echo —
you may not see characters as you type, but the command still runs. The boot log
prints which channel the console is on — on this board it should say
`serial console on USB-Serial-JTAG` (the USB-C port, not UART0). Commands:

| Command | Does |
|---------|------|
| `help` | list all commands |
| `list` | list all screens (info / clips / scenes) + current |
| `screen <n>` | jump to screen n |
| `scene <id\|name>` | jump to a dojutsu (e.g. `scene rinnegan`); `effect` = alias |
| `next` / `prev` | step screens (same as the two flick directions) |
| `info` | jump to the info screen |
| `play` / `stop` | jump to the first clip / back to the scenes |
| `clip` | show clip playlist info |
| `get` | print every parameter |
| `set <key> <val>` | tune a parameter (see keys below) |
| `color <r> <g> <b>` | recolor the **current** scene (0–255 each) |
| `flick <on\|off>` | enable/disable the flick gesture |
| `reset` | restore all params to built-in defaults |
| `save` / `load` | persist / restore all params to NVS |

**`set` keys:** `speed` (global %), `scale` (eye size %), `sspin` (iris spin
ms/rev, sign = direction), `pupil` (radius px), `blink` (avg ms between blinks,
0 = off), `gaze` (saccade amplitude px, 0 = still), `flare` (avg ms between
spin flares, 0 = off), `fhi`, `flo`.

**Eye size:** `set scale <pct>` resizes the eye on the panel live. `100` = the
default fit (eye diameter = panel width, ~33 mm — the lens-calibration size).
Lower shrinks it; on the AMOLED, above 100 overscans toward the panel height
(circle fills more vertically, left/right edges clipped). Range 20–130.

Examples:
```
eye> scene tomoe3
eye> set sspin -2500      # spin the other way, 2.5 s/rev
eye> color 255 40 0       # orange sharingan
eye> set blink 8000       # blink less often
eye> set gaze 0           # eye stops looking around
eye> set speed 200        # everything 2x faster
eye> set scale 75         # shrink the eye to 75%
eye> set scale 120        # overscan bigger (AMOLED)
eye> save                 # remember it across reboots
```

## WiFi web control (both boards)

Optional web app with everything the console does and more (scene/clip
switching, upload/delete clips, flip H/V, brightness, colors, save/reboot).
WiFi is **off at boot** to save battery; a 10-minute idle timeout turns it back
off. Enable it from the **info screen's touch sliders** (both boards run the
same LVGL info screen now), or via the console: `wifi ap` (own hotspot — SSID
`SharinganEye-XXXX`, password
`uchihaXXXX`, both shown on screen) or `wifi sta` (joins your LAN; first time
opens a captive **setup portal** on `SharinganEye-Setup`). Then browse to
**http://sharingan-eye.local** (fallback `http://192.168.4.1` in AP mode).
`bright <5-100>` sets panel brightness (persisted). Code: `components/eye_web/`.

## Video / GIF clips

Real GIF/video clips are the middle section of the screen list: every clip in
the playlist is its own screen, so a **gyro flick** (or `next`/`prev`) walks
info → clips → scenes and wraps. Full design in [VIDEO_PLAN.md](VIDEO_PLAN.md).

### Generate a clip from scratch (recommended)

```sh
pip install -r tools/requirements.txt
python tools/eye_animator.py
```

**`eye_animator.py`** *creates* the dojutsu animation on the PC — no source GIF
needed — with far higher fidelity than the on-device procedural scenes: a
vector-rendered (skia) anime eye with sharp eyelids, glossy gradient iris,
crisp spinning pattern, specular glints and glow. All 8 dojutsu are available
(tomoe1/2/3, mangekyou, itachi, rinnegan, tomoerin, byakugan) with sliders for
eye shape (**anime ↔ round**), iris/pupil size, spin, blinks, gaze wander,
spin flare, colors. The timeline is **deterministic and seamless**: spin
completes whole revolutions per loop and blinks/gaze wrap cleanly, so the
exported clip loops forever without a visible jump. Preview is a fast proxy;
**Export GIF** / **Export .eyv** render at 4× supersampling in the background
(JPEG at 4:4:4 chroma so the red/black edges stay crisp).

### Convert an existing GIF/video

```sh
python tools/eye_studio.py yourclip.gif       # GUI (crop/trim/effects)
python tools/make_anim.py clip.gif -o a.eyv --fps 20 --quality 90   # or one-shot CLI
```

The **GUI** (`eye_studio.py`) does it all visually: scrub frames, drag a square
crop with the round-display circle drawn on, **trim** with In/Out markers, and a
tabbed **effects** panel (Photoshop-style) with live preview — brightness/
contrast/saturation/gamma/hue, **glow/bloom**, vignette, sharpen, per-channel
gains, color **tint**, invert, plus **animated** effects (hue-cycle, spin, pulse,
breathe) that move across the loop. Great for making a dull clip pop or recoloring
it. Then **Export .eyv**. (Effects live in `tools/eye_effects.py`.)

Clips are MJPEG (JPEG/frame) decoded on the fly; `set scale` still zooms them.
~18–30 fps depending on `--fps`.

### LCD 1.28″ — clips in flash (FATFS)

The `anim` partition is a **wear-levelled FATFS** volume (formatted
automatically on first boot), mounted at `/anim`. Manage clips over the **web
UI**: enable WiFi (`wifi ap` on the console), open `http://sharingan-eye.local`,
and upload/delete `.eyv` files on the Playback tab — exactly like the AMOLED's
SD card, no reflashing. (The old raw `flash_clip.ps1` / `.eyp` esptool flashing
is obsolete for this board.)

### AMOLED 2.06 — clips on SD card

No flashing for clips — the AMOLED reads them from the **SD card** folder
**`/eye/`**. Every `*.eyv` in that folder is a playlist entry (sorted by name).

1. Format an SD card FAT32, make a folder **`eye`**, copy your `.eyv` files into
   it (e.g. `eye/01_sharingan.eyv`, `eye/02_rinnegan.eyv`), insert the card.
2. Build/flash the AMOLED app once (`board.txt` = `amoled_2_06`, `idf.py flash`).
3. On boot it plays them; flick or `next` cycles. Swap clips later by just copying
   files onto the card — no reflash. (Folder set by `EYE_CLIP_DIR` in
   `boards/amoled_2_06/main/app_main.c`.)

## Lens calibration note (important)

On the AMOLED 2.06 the eye defaults to a diameter equal to the **panel width
(~410 px ≈ 33 mm)** and centered — not a raw 240 px. That makes the on-glass image
the same physical size as the 1.28″ round board's active area (Φ32.4 mm), so the
lens air-gap you calibrate now carries straight over. Use **`set scale <pct>`** to
resize the eye live (100 % = panel width); see the console section above.

---

## Build & flash (ESP-IDF / Espressif-IDE)

Tested toolchain: **ESP-IDF v5.5.x** (same as ScreenStreamWatch). Open/build the
repo root `G:\ElectronicsProj\sharingan` — it builds the AMOLED 2.06 by default.

### Espressif-IDE (Eclipse)
1. **File → Import → Espressif → Existing IDF Project**, point it at
   `G:\ElectronicsProj\sharingan` (the folder with `CMakeLists.txt`).
2. Set target **esp32s3** if asked.
3. Build (hammer), then **Flash** with the board's COM port selected.
4. Open the **Serial Monitor** to see logs (`sharingan`, `eye_demo`, `eye_imu`).

### Command line (ESP-IDF terminal)
```sh
cd G:\ElectronicsProj\sharingan
idf.py set-target esp32s3      # first time only
idf.py build
idf.py -p COM5 flash monitor   # use your board's port
```

### Switching boards (one file, no flags)

Both boards live in this one project. To switch which one builds, **edit
`board.txt`** (repo root) — change the single word to `amoled_2_06` or
`lcd_1_28` — and rebuild. CMake reconfigures automatically. The CMake log prints
`Sharingan board: <name>` so you can confirm. Each board keeps its own
`sdkconfig.<board>`, defaults, and partition table, so they never clobber.

```
# board.txt
amoled_2_06      # <- change to lcd_1_28 and rebuild to flash the round board
```

Switching in the same build directory triggers a near-full recompile (the two
boards use different components and PSRAM settings) — that's expected. If you'd
rather keep two warm builds, give each its own dir and the flag still works as a
one-off override:
```sh
idf.py -B build_amoled build flash                       # board.txt is ignored when -D is given
idf.py -B build_1_28 -DSHARINGAN_BOARD=lcd_1_28 build flash
```

First build pulls the Waveshare BSP + LVGL into `managed_components/`
(needs internet). `eye_core` is found automatically via `EXTRA_COMPONENT_DIRS`
in the project `CMakeLists.txt` — no registry publish needed.

Expected on boot (device shows the info screen; flick to move on):
```
I (...) eye_disp_amoled: ready: panel 410x502, eye 410px (scaled to width)
I (...) eye_bat: AXP2101 fuel gauge ready        # or ADC on the LCD 1.28
I (...) sharingan: AMOLED 2.06 eye demo starting
I (...) eye_scene: scene engine ready (8 scenes, 115200-byte polar LUTs)
I (...) eye_imu: QMI8658 found at 0x6B           # or "not detected" -> flick off
I (...) eye_console: serial console on USB-Serial-JTAG -- type 'help' and press Enter
I (...) eye_demo: running: 12 screens (info + 3 clips + 8 scenes), flick=on (QMI8658)
eye>
```

---

## Porting to the 1.28″ round board

Short version: write one new display backend and one I2C bus init; reuse
everything else. Full checklist in **[PORTING.md](PORTING.md)**.
