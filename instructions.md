CONTEXT
I'm building an animated "eye" for an Obito (Naruto) cosplay mask: a small round
display behind a magnifying lens, playing a looping Sharingan/Rinnegan-style
animation. I have one board now and a second arriving in 2 days. I want code that
runs on the board I have today but is structured to port to the other with minimal
changes.

BOARD I HAVE NOW (build target for this task)
- Waveshare ESP32-S3-Touch-AMOLED-2.06
- 2.06" AMOLED, 410x502, CO5300 driver, QSPI interface
- FT3168 capacitive touch (I2C), QMI8658 6-axis IMU
- ESP32-S3R8, 8MB PSRAM, 32MB flash
- Wiki: https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-2.06

BOARD ARRIVING IN 2 DAYS (port target: architect for it, do not build for it yet)
- Waveshare ESP32-S3-Touch-LCD-1.28
- 1.28" round LCD, 240x240, GC9A01 driver, SPI interface, ~33.4mm active diameter
- CST816 touch (I2C), QMI8658 6-axis IMU
- ESP32-S3R2, 2MB PSRAM, 16MB flash
- Docs: https://docs.waveshare.com/ESP32-S3-Touch-LCD-1.28

ENVIRONMENT
- ESP-IDF via Espressif-IDE on Windows.
- Existing project at G:\ElectronicsProj\ScreenStreamWatch, already set up for the
  AMOLED 2.06 board. Read its README, docs, and components first. REUSE its existing
  display bring-up. Do NOT hand-roll the CO5300 QSPI init.
- Read the local project docs AND both wiki URLs above before writing any code.

TASK
1. Read the project and both boards' docs to understand the display pipeline that's
   already in place (LVGL vs raw esp_lcd, pin config, panel init).
2. Add a self-contained "animated eye" demo that runs on the AMOLED 2.06 now.
3. Animation: a looping circular eye. A placeholder is fine (e.g. concentric rings
   with a rotating tomoe/ripple). I'll swap in real Sharingan/Rinnegan frames later,
   so make the animation source easy to replace.

ARCHITECTURE REQUIREMENTS (important, for portability)
- Render everything into a fixed 240x240 RGB565 framebuffer. All animation and logic
  draws into this buffer and must be board-agnostic.
- Put ALL board-specific code behind a thin interface: display_init() and
  push_frame(uint16_t* buf240x240). On the AMOLED, push_frame centers/blits the
  240x240 buffer into the 410x502 panel. (The 1.28 board's push_frame will later be a
  1:1 push; I'll add that when it arrives.)
- If the project uses LVGL, render the buffer via an LVGL canvas; if it talks to
  esp_lcd directly, push the raw buffer. Pick based on what you find.
- Keep the QMI8658 IMU read in a board-agnostic helper (same chip on both boards).
  Stub in a simple tilt trigger that switches between two animations.

LENS-TEST REQUIREMENT
- The future round board's active area is 33.4mm. The AMOLED 2.06 is ~33mm wide.
  Draw the eye inside a circle whose diameter equals the full panel WIDTH (~410px),
  centered, so the physical image size matches the future board. This lets me
  calibrate the lens air-gap now and have it carry over.

DELIVERABLES
- The new source files and how they plug into the existing project.
- Build and flash instructions for ESP-IDF / Espressif-IDE.
- A short note listing exactly what I'll change to port to the 1.28 round GC9A01
  board later.