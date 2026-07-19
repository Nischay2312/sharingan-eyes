# Changelog

All notable changes to the Sharingan eye firmware are recorded here.

## v1.1

### Fixed
- **AMOLED silent freeze.** The AMOLED (SH8601) display would freeze on a
  random frame with no crash or watchdog. Root cause (found by bisection): the
  eye was drawn as an LVGL image, and the LVGL task's QSPI panel flush
  intermittently lost its trans-done interrupt and wedged in `wait_for_flushing`
  while holding the port mutex, so the next frame's display lock blocked forever.
  It was unrelated to WiFi, brightness, console, IMU, or the JPEG decoder.
- **Per-clip settings no longer collide.** Overrides were keyed on the first 15
  chars of the filename; clips sharing a 15-char prefix overwrote each other.
  Keys are now a short readable prefix plus an FNV-1a hash of the full name.
  (Note: existing saved overrides are orphaned by the key change and must be
  re-saved once.)

### Changed
- **AMOLED display bypasses LVGL entirely.** The backend now gets the raw
  panel/IO handles from `bsp_display_new`, owns its own color-trans-done
  semaphore, and pushes frames straight to the panel. A lost/late trans-done
  just times out and the next frame re-arms — it can no longer deadlock.
- **Pipelined rendering.** A flush task on core 0 transfers each composed frame
  in parallel with the render/decode task on core 1 (double-buffered with
  backpressure), holding the smooth 20 fps the LVGL path had.
- Frames are composed into a full-panel canvas and transferred band-by-band
  through an internal DMA bounce buffer; only the margins around the eye are
  cleared each frame (not the whole panel).
- **AMOLED info screen renders as raster** (via `eye_info_render`) instead of
  the old LVGL widget screen, since LVGL is no longer in the AMOLED build.
- **Brightness is bus-safe.** Web/console brightness (SH8601 command 0x51) is
  serialized with the flush task's pixel DMA via a bus mutex, so it can't
  collide on the shared QSPI bus.

### Added
- Storage/SD size bar in the web UI (Info tab card + Clips tab header).

## v1.0

Initial release: dual-board ESP32-S3 firmware for wearable animated eye
displays.

- Boards: Waveshare LCD 1.28 (GC9A01, 240×240) and AMOLED 2.06 (SH8601).
- WiFi web UI for clip upload/delete/play, OTA firmware updates, and eye sync
  across multiple boards.
- PSRAM-optimised RAM layout; Python desktop clip manager (`tools/eye_manager.py`).
