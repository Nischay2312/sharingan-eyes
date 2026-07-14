# Plan: GIF/Video playback on the eye

Goal: take any GIF/short video, convert it on the PC into a display-ready file,
flash it to the board, and have the firmware loop it on the eye. First target:
the **1.28″ round GC9A01** board. The design stays board-agnostic so the same
clip later plays on the AMOLED 2.06 with no re-conversion.

---

## 1. How it fits the current architecture

Nothing about the board interface changes. The engine already renders into a
fixed **240×240 RGB565** framebuffer and hands it to the board via
`eye_display_push_frame(fb240, scale_pct)`. A video is just a different source of
those 240×240 frames:

```
procedural effects ─┐
                    ├─> 240×240 RGB565 fb ─> eye_display_push_frame() ─> panel
video player ───────┘
```

So the video player decodes each frame into the same `fb` and reuses the exact
push path (byte-swap + BGR + scaling all already handled per board). On the round
board it's 1:1; on the AMOLED the same frames auto-scale to panel width.

---

## 2. Storage on the board

Frames live in their own **flash data partition** (`anim`), flashed independently
of the firmware so iterating on a clip doesn't require an app rebuild.

`boards/lcd_1_28/partitions.csv` (16 MB flash) gains:

```
# Name,   Type, SubType, Offset,  Size
nvs,      data, nvs,     0x9000,  0x6000,
phy_init, data, phy,     0xf000,  0x1000,
factory,  app,  factory, 0x10000, 4M,
anim,     data, spiffs,  ,        10M,
```

The firmware finds it by name and **memory-maps** it
(`esp_partition_mmap`) so JPEG frames are read straight from flash — **zero RAM
copy**, no filesystem layer.

Capacity (10 MB): at ~10–14 KB per JPEG frame that's ~750–1000 frames ≈
**35–50 s at 20 fps** — far more than a looping eye needs.

---

## 3. Clip container format (`.eyv`)

A single packed file, little-endian:

```
Header (32 bytes)
  0  magic        char[4]  "EYEV"
  4  version      u16      = 1
  6  format       u16      0 = MJPEG (JPEG per frame), 1 = raw RGB565_LE
  8  width        u16      240
 10  height       u16      240
 12  frame_count  u32
 16  frame_ms     u16      ms per frame (e.g. 50 = 20 fps)
 18  loop         u8       1 = loop forever, 0 = play once
 19  reserved     u8[13]
Frame index   frame_count × { offset u32, size u32 }   (offset from file start)
Frame data    concatenated JPEG (or raw RGB565) blobs
```

- **MJPEG** is the default (small, lets clips be long). JPEG decodes to
  `RGB565_LE` — the exact byte order the engine already uses.
- **raw RGB565** is supported for ultra-short clips where you want zero decode
  cost (112.5 KiB/frame, so only a handful fit).

---

## 4. PC conversion tool — `tools/make_anim.py`

```
python tools/make_anim.py input.gif -o anim.eyv \
       --fps 20 --quality 80 --fit cover --mask circle
```

Pipeline:
1. **Read frames** — GIF via Pillow (honors per-frame durations); video via
   `imageio`/`ffmpeg`. 
2. **Resample fps** to `--fps` (drop/duplicate frames to hit the target).
3. **Fit to square** — `--fit cover` (center-crop, default) or `contain`
   (letterbox), then resize to 240×240.
4. **Circular mask** (`--mask circle`, default for the round board) — paint
   outside the Φ circle black so it matches the bezel. `--mask none` to skip.
5. **(optional) color tweaks** — `--saturation`/`--gamma` to match the punchy
   look we tuned for the panel.
6. **Encode** each frame: JPEG at `--quality` (default 80) → or raw RGB565 with
   `--format raw`.
7. **Write `.eyv`** — header + index + data, print frame count / total size /
   estimated fps headroom.

Deps (PC only, documented in `tools/requirements.txt`): `pillow`, `imageio`,
`imageio-ffmpeg`, `numpy`.

---

## 5. Firmware changes

**New module `components/eye_core/eye_video.[ch]`** (board-agnostic):
- `eye_video_init()` — find `anim` partition, mmap it, validate header, keep the
  frame index.
- `eye_video_available()`, `eye_video_frame_count()`, `eye_video_frame_ms()`.
- `eye_video_decode(uint32_t idx, uint16_t *fb240)` — MJPEG: decode that frame's
  JPEG (from the mmap'd flash pointer) into `fb` via `esp_new_jpeg`; raw: memcpy.

**Dependency:** add `espressif/esp_new_jpeg` (same decoder the AMOLED streamer
used) to both boards' `idf_component.yml`; `eye_core` REQUIRES it + `esp_partition`.

**Integration in `eye_demo.c`:** add a playback mode. The render task does:
```
if (mode == VIDEO && eye_video_available())
    eye_video_decode(frame++, fb);   // wraps / stops per loop flag
else
    eye_anim_render(fb, t, &params);
push_frame(fb, params.scale_pct);
pace to frame_ms (video) or target fps (effects), always yielding
```

**Console commands** (extend `eye_console.c`):
- `play` — switch to video mode (if a clip is present).
- `stop` — back to procedural effects.
- `clip` — print the loaded clip's info (frames, fps, size).
- Optional `autoplay on|off` (persisted) — start in video mode on boot.

Flick gesture keeps cycling procedural effects; in video mode a flick can
`stop` back to effects (decided below).

---

## 6. Playback, timing, performance

- Decode 240×240 JPEG on the S3 ≈ 10–20 ms; SPI push ≈ 23 ms @ 40 MHz.
  → realistic **18–25 fps**. We can bump the GC9A01 SPI clock to **80 MHz**
  (Waveshare's demo uses it) to roughly halve the push and comfortably hit
  25–30 fps.
- Same always-yield pacing as the effect loop so the **task watchdog** stays fed.
- Double-buffering already exists in the board push, so decode(N+1) overlaps the
  SPI of frame N reasonably well.
- `scale_pct` still applies, so `set scale` shrinks/zooms the video too.

---

## 7. Developer workflow

```
# 1. Build a clip
python tools/make_anim.py sharingan.gif -o anim.eyv --fps 20

# 2. Flash just the clip to the anim partition (no app rebuild)
parttool.py --port COM4 --partition-name anim write_partition --input anim.eyv
#   (or: esptool.py --port COM4 write_flash <anim_offset> anim.eyv)

# 3. On the device console
eye> clip      # confirm it loaded
eye> play      # watch it loop
```

Firmware change (one-time): rebuild/flash the app once to add the player +
partition table.

---

## 8. Recommended defaults (please confirm / adjust)

| Decision | Recommended | Alternatives |
|---|---|---|
| Frame storage | MJPEG (JPEG/frame) | raw RGB565 (tiny clips only) |
| Location | raw `anim` partition + mmap | SPIFFS/LittleFS file |
| Trigger | console `play`/`stop`, effects by default on boot | auto-play clip on boot |
| Flick in video mode | flick → back to effects | flick ignored |
| Target fps | 20 (cap; bump SPI to 80 MHz) | 15 (safer) / 25–30 (needs 80 MHz) |
| Fit | cover (center-crop) + circle mask | contain (letterbox) |

---

## 9. Implementation milestones

1. **Format + tool** — write `make_anim.py`, define `.eyv`, produce a test clip;
   add a tiny PC-side verifier that previews frames.
2. **Partition + storage** — add `anim` partition, `eye_video_init()` mmap +
   header parse, log clip info on boot.
3. **Decode + play** — `eye_video_decode()` (MJPEG via esp_new_jpeg), wire the
   render task's video branch, `play`/`stop`/`clip` console commands.
4. **Tune** — bump SPI to 80 MHz, verify fps, circular mask alignment, colors.
5. **Polish** — `autoplay` pref, `--fit/--mask/--saturation` options, docs.

---

## 10. Future (out of scope for v1)

- AMOLED 2.06 playback (same `.eyv`, frames auto-scale; could ship a 410-native
  clip for max sharpness).
- Multiple clips / playlist, crossfade between clip and procedural effect.
- Audio on the AMOLED board (it has a speaker; the round board does not).
- On-the-fly streaming over USB/Wi-Fi instead of pre-flashing.
```
