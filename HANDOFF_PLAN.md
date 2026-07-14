# Sharingan feature handoff plan

Handoff for continuing work after the Fable session ended mid-task (chat log from line 8785 in `claude_code_chat.md`). **Do not treat this as “greenfield”** — most firmware work is already landed in the tree; the next agent should verify, polish, and build the PC pre-renderer the user asked for at the very end.

---

## 1. What the user asked for

### A. Screen / mode system (firmware)

- Boot on an **info screen** (firmware version, compile date/time, board name, uptime, clip/scene counts).
- Show **battery capacity** using the correct hardware per board:
  - **AMOLED 2.06:** AXP2101 PMIC fuel gauge (I2C).
  - **LCD 1.28:** GPIO1 ADC with onboard ÷3 divider (LiPo curve estimate).
- Replace the old flat procedural effects with a **realistic anime eye** like the reference GIFs:
  - Eyelid silhouette on near-black skin.
  - Eye opening, blinking, morphing between patterns.
  - Sharingan spin, Rinnegan ripples, Mangekyō variants, Byakugan, etc.
- **One flat screen list** cycled by IMU flick:
  ```
  info → clip 1 … clip N (SD / flash playlist) → 8 in-house dojutsu scenes → info …
  ```
- **Bidirectional flick:** forward flick = next screen; opposite flick = previous screen.
- Old 4-effect engine (`eye_anim`) is obsolete; user cares about the **new** scenes, not the legacy effects.

### B. PC pre-rendered animations (follow-up, not started)

After the firmware work, the user asked for a **Python GUI** that:

- Generates the same style of eye animations **offline on the PC** (not real-time on ESP).
- Exports **GIF** (preview) and ideally **`.eyv`** (board playback) for higher visual fidelity.
- Is **customizable:** eye shape (more anime / less “normal round”), pattern design, spin, gaze wander, blink timing, colors.
- Leverages existing `.eyv` pipeline (`tools/make_anim.py`, `tools/eye_studio.py`) so clips are less taxing on the ESP than live procedural math.

**Priority:** realism and sell of the cosplay effect over having every possible dojutsu on day one.

---

## 2. Architecture (as implemented)

### Screen list (`eye_demo.c`)

| Index | Kind | Renderer |
|-------|------|----------|
| `0` | Info | `eye_info_render()` |
| `1 … N` | Video clip | `eye_video_next_frame()` (SD folder or flash partition) |
| `N+1 … N+8` | Dojutsu scene | `eye_scene_render()` |

- `eye_demo_set_screen(idx)` — wraps, runs enter hooks (clip select, `eye_scene_enter` vs `eye_scene_select`).
- `eye_demo_step_screen(±1)` — used by gesture task and console `next`/`prev`.
- Render task on **core 1**, always yields (watchdog-safe).

### Gesture (`eye_imu.c`)

- `eye_imu_poll_flick()` now returns **`+1` / `-1` / `0`** (signed peak gyro axis), not `bool`.
- Cooldown + re-arm thresholds unchanged (`fhi` / `flo` in params).

### Params (`eye_params` v3)

- `effect` → `scene` (`eye_scene_id_t`, 8 scenes).
- Per-scene colors; shared behavior knobs: `spin_ms`, `pupil_px`, `blink_ms`, `gaze_px`, `flare_ms`.
- NVS blob version bumped to **3** — first flash after upgrade resets saved tweaks.

### Battery contract (`eye_battery.h`)

| Board | File | Method |
|-------|------|--------|
| `amoled_2_06` | `boards/amoled_2_06/main/eye_battery_axp2101.c` | AXP2101 @ I2C 0x34 |
| `lcd_1_28` | `boards/lcd_1_28/main/eye_battery_adc.c` | ADC1_CH0 GPIO1, ÷3 divider |

### Scene engine (`eye_scene.c`)

- 240×240 RGB565, polar LUTs in PSRAM.
- Almond lid geometry (`EYE_HW`, `EYE_HU`, `EYE_HL`), blink state machine, gaze saccades, spin flares.
- 8 scenes: `tomoe1/2/3`, `mangekyou`, `itachi`, `rinnegan`, `tomoerin`, `byakugan`.
- `eye_scene_enter(id)` — dramatic open when arriving from info/clips.
- `eye_scene_select(id)` — blink-morph when scene → scene.

### Supporting pieces already in tree

| Artifact | Role |
|----------|------|
| `tools/gen_font.py` → `eye_font.h` | 8×12 bitmap font for info screen |
| `eye_info.c` | Info screen layout (version, battery bar, counts, uptime) |
| `eye_console.c` | `list`, `screen`, `scene`, `info`, `play`, `stop`, updated `set` keys |
| `eye_video.c` | `eye_video_select()`, `eye_video_clip_name()` for playlist screens |
| `tools/eye_studio.py` | Existing GUI for cropping external GIF/video → `.eyv` |
| `tools/eye_effects.py` | Post-process effects (glow, tint, vignette) for clips |
| `VIDEO_PLAN.md` | Clip container format (`.eyv`); **still mentions `eye_anim` — update when touching docs** |

### Deleted / replaced

- `eye_anim.c` / `eye_anim.h` — removed from `eye_core/CMakeLists.txt`.
- Stale `build/` may still reference `eye_anim.c.obj` until a clean rebuild.

---

## 3. Current repo status

### Done (code present, matches Fable’s final design)

- [x] Screen list + boot-on-info
- [x] Directional flick forward/back
- [x] Info screen + font
- [x] Battery drivers (AXP2101 + ADC)
- [x] `eye_scene` engine (8 dojutsu)
- [x] Params v3 + console commands
- [x] `app_main` wiring (AMOLED: battery init, SD clip dir `/sdcard/eye`)
- [x] README / PORTING updated for new layout

### Not verified on hardware (next agent should do first)

- [ ] **Clean build + flash** both boards (`idf.py fullclean build flash`)
- [ ] Boot log: `eye_scene: scene engine ready`, `eye_demo: running: X screens (info + …)`
- [ ] Info screen: battery % / voltage / charging on AMOLED with battery attached
- [ ] Flick cycles info → clips → scenes → info; reverse flick goes back
- [ ] Visual QA behind lens: tomoe comma shapes, rinnegan ring spacing, **anime eye shape** vs reference images
- [ ] FPS / watchdog under full-width AMOLED upscale (render still on core 1)
- [ ] LCD 1.28 board build (pulls `esp_adc` per `boards/lcd_1_28/main/CMakeLists.txt`)

### Known gaps / polish

- **Eye shape:** C engine uses almond lids but user said it still looks too “normal” vs reference GIFs — expect iteration.
- **AXP2101 %:** Waveshare notes voltage-derived SOC can jump on plug/unplug; document in UI if needed.
- **PC harness:** Fable compiled `eye_scene.c` on PC in a temp scratchpad (PPM/GIF dumps). **Not committed to repo** — worth adding `tools/scene_harness/` for regression previews.
- **Python procedural animator:** **Not started** (session ended when user asked for GUI + handoff plan).

---

## 4. Recommended work order for the next agent

### Phase 0 — Verify firmware (1 session)

1. `idf.py fullclean build` from `sharingan/` (AMOLED default).
2. Flash, confirm USB-Serial-JTAG console (`help`, `list`, `scene tomoe3`).
3. Exercise full screen cycle + battery on info screen.
4. Log issues: watchdog, flick direction inverted, battery garbage, scene art.

Only fix blocking bugs in Phase 0; defer art tweaks to Phase 2 unless unusable.

### Phase 1 — PC scene harness (quick win)

Add `tools/scene_harness/`:

- Minimal C stubs (`esp_err.h`, `esp_log.h`, `esp_heap_caps.h`, `esp_timer.h`) + `main.c` that calls `eye_scene_render()` frame-by-frame.
- `build.sh` / `build.ps1` → dump PNG sequence or animated GIF.
- Lets art iteration **without** reflash (Fable already proved this workflow).

### Phase 2 — On-device visual polish

Tune in `eye_scene.c` (or harness first, then port numbers):

| Parameter | Location | User concern |
|-----------|----------|--------------|
| Lid almond shape | `EYE_HW`, `EYE_HU`, `EYE_HL`, lid curve math | “More anime” silhouette |
| Tomoe commas | `tomoe_hit()`, `TOMOE_TAIL` | Reference image #12–14 |
| Rinnegan rings | `s_gap`, ripple drift | Hypnotic fill vs reference |
| Blink / open timing | `DUR_*` constants | Match reference GIF pacing |

### Phase 3 — Python procedural animator GUI (user’s new request)

**Goal:** `tools/eye_animator.py` — a desktop app that renders eye animations offline and exports GIF + `.eyv`.

**Decisions locked in with the user (2026-07-04):**

| Decision | Choice |
|----------|--------|
| Language | Python (integrates with existing `tools/` pipeline) |
| Rendering backend | **`skia-python`** — AA bézier paths, radial/sweep gradients, built-in blur for glow, path booleans for tomoe cutouts. Do **NOT** port `eye_scene.c` integer math; it reproduces the crunchy ESP look. |
| First scene / quality benchmark | **Mangekyō pinwheel** (matches user reference `G:\Chrome_Downloads\e342aa99d92cb8660fa1918986c595d0-ezgif.com-crop.gif`) |
| Timeline | **Fixed-length seamless loop** — spin completes integer revolutions; blinks/gaze placed so last frame flows into first. Loop length is a first-class GUI control. |
| On-device procedural scenes | Keep for now; may be hidden later if generated clips clearly win |
| GUI toolkit | Tkinter (consistency with `eye_studio.py`, zero new deps) |

#### 3.1 Design principles

- **Vector art, not pixel math** — build lids/iris/patterns as skia paths with gradients and blur. Only *parameter names* (spin, blink, gaze, flare) mirror the firmware so the mental model carries over.
- **Deterministic seamless loop** — no RNG at render time. The timeline is generated once from a seed + loop length; every event (blink, saccade, flare) has a start/duration placed so the loop wraps invisibly.
- **Lids are curves, not constants** — two cubic béziers meeting at pointed corners (upper peak biased nose-side), with an “anime ↔ round” interpolation slider. The C engine’s `EYE_HW/HU/HL` triple cannot make the reference silhouette.
- **Proxy preview / full-quality export** — GUI preview renders 240 px, no supersampling, cheap glow (fast enough for Tk). Export renders 4× (960 px), full blur, downsamples to 240, on a background thread with progress.
- **Export path reuses existing tools** (verified present in `make_anim.py`):
  - `frame_to_blob(im240, fmt, quality)` + `pack_eyv(out_path, blobs, fps, fmt, loop)`.
  - **Required tweak:** `encode_jpeg()` currently uses default 4:2:0 chroma subsampling, which smears saturated red/black edges (worst case for this art). Export at `quality≈95, subsampling=0` (4:4:4) and **verify `esp_new_jpeg` decodes 4:4:4 on-device early** — if it can’t, fall back to raw RGB565 for short clips or high-quality 4:2:0.
  - Optional post-pass via `eye_effects.py` (vignette etc.) on PIL frames.

#### 3.2 GUI layout (Tkinter, match `eye_studio.py` style)

```
┌─────────────────────────────────────────────────────────────┐
│ [Scene ▼]  [Preview]  [Timeline scrubber]  [Play loop]      │
├──────────────────┬──────────────────────────────────────────┤
│ Eye shape        │  Live 480×480 preview (2× of 240 proxy)  │
│  anime↔round     │                                          │
│  width, height   │                                          │
│  corner sharpness│                                          │
├──────────────────┤                                          │
│ Loop / behavior  │                                          │
│  loop length s   │                                          │
│  spin rev/loop   │                                          │
│  blinks per loop │                                          │
│  gaze amplitude  │                                          │
│  flare on/off    │                                          │
├──────────────────┤                                          │
│ Pattern / color  │                                          │
│  scene-specific  │                                          │
│  RGB sliders     │                                          │
├──────────────────┤                                          │
│ Export           │                                          │
│  fps, quality    │                                          │
│  [Export GIF]    │                                          │
│  [Export .eyv]   │                                          │
└──────────────────┴──────────────────────────────────────────┘
```

Note the behavior knobs are **per-loop** (spin revolutions per loop, blinks per
loop), not average intervals — that is what makes the loop seamless by
construction.

#### 3.3 Module split

```
tools/
  eye_animator.py      # Tk app entry
  eye_render/
    __init__.py
    geometry.py        # lid béziers, anime↔round interpolation, masks (skia paths)
    patterns.py        # tomoe, mangekyou, rinnegan, byakugan (skia paths + gradients)
    timeline.py        # seamless-loop event placement (blinks, saccades, flares)
    pipeline.py        # frame loop: skia surface → PIL → GIF / frame_to_blob/pack_eyv
  scene_harness/       # optional: keep C reference in sync
```

#### 3.4 Anime eye shape (explicit user ask)

Reference GIFs use a **taller, sharper almond** than a human eye:

- Increase vertical asymmetry (upper lid more curved, lower flatter).
- Corner points pulled horizontally (`EYE_HW` ratio > height).
- Optional “anime preset” slider: interpolates lid control points between round-human and sharp-anime.
- Sclera only in central region; lid fold grey band like reference image #11.

Validate by side-by-side overlay with user’s reference PNGs in the chat.

#### 3.5 Integration with firmware screen list

Two playback modes for generated content:

| Mode | How it appears on device |
|------|--------------------------|
| **A. Clip** | Export `.eyv` → SD `/eye/` or flash `anim` partition → screens `1…N` in list |
| **B. Live scene** | Keep procedural `eye_scene.c` for tuning via serial `set` |

User preference leans **A** for final cosplay quality; keep **B** for live tweaking. Document workflow in README:

```
python tools/eye_animator.py
# tune → Export .eyv → copy to SD card /eye/ → flick to clip screens
```

#### 3.6 Dependencies

Add to `tools/requirements.txt`:

```
pillow
numpy
skia-python        # vector renderer for eye_animator (pip installable, Windows wheels available)
```

#### 3.7 Milestones for Phase 3 — STATUS (2026-07-04)

1. [x] **Static Mangekyō frame** — skia render matches the reference look
   (sharp anime lids, glossy red iris, slim pinwheel blades, glint, glow).
2. [x] **Seamless loop** — 7 s / 175 frames; first-vs-last frame mean abs diff
   ≈ 1.3/255 (imperceptible); blink, saccades and flare all wrap cleanly.
3. [x] **`.eyv` export** — 4:4:4 JPEG q95 via local encoder + `pack_eyv`;
   header validated (EYEV v1, MJPEG, 240×240, 40 ms/frame, loop). ~15 KB/frame.
   **Still to do: decode-verify on the actual device** (SD `/eye/` on AMOLED).
4. [x] **GUI** — `tools/eye_animator.py`: scene picker, eye-shape sliders
   (anime↔round), loop controls, proxy preview + play, background 4× export
   with progress bar.
5. [x] **All 8 scenes** — tomoe1/2/3, mangekyou, itachi, rinnegan, tomoerin,
   byakugan render on the same pipeline (montage-verified).
6. [x] **Anime shape preset** — `EyeShape.anime` 0..1 slider interpolates
   round↔anime lid béziers.

**Remaining:** on-device playback test of an exported `.eyv` (JPEG 4:4:4
decode by `esp_new_jpeg`, red-edge quality, fps). If 4:4:4 fails to decode,
re-export with `subsampling=2` (4:2:0) at q97 or use `fmt="raw"` for short
clips.

---

## 5. Files to touch (checklist)

### Firmware verification only

- `components/eye_core/eye_demo.c`
- `components/eye_core/eye_scene.c`
- `boards/amoled_2_06/main/eye_battery_axp2101.c`
- `boards/lcd_1_28/main/eye_battery_adc.c`

### PC animator (new work)

- `tools/eye_animator.py` (new)
- `tools/eye_render/` (new package)
- `tools/requirements.txt`
- `tools/scene_harness/` (recommended)
- `README.md` — “Generating clips” section
- `VIDEO_PLAN.md` — replace `eye_anim_render` references with `eye_video_next_frame` / screen list

### Do not resurrect

- `eye_anim.c` / `eye_anim.h` — stay deleted.

---

## 6. Console quick reference (post-upgrade)

```
eye> list              # all screens; current marked
eye> info              # jump to boot info
eye> next / prev       # same as forward / backward flick
eye> scene rinnegan    # jump to scene
eye> play              # first clip (if any)
eye> get               # scene + spin/blink/gaze/flare
eye> set gaze 14
eye> set blink 5000
eye> color 255 0 0
eye> save
```

Expected boot (example):

```
I eye_bat: AXP2101 fuel gauge ready
I eye_scene: scene engine ready (8 scenes, …)
I eye_demo: running: 9 screens (info + 0 clips + 8 scenes), flick=on (QMI8658)
```

(Device boots on **info screen**, not a scene.)

---

## 7. Reference images

The user attached reference images in the chat (labeled #11–#14 in `claude_code_chat.md`):

- **#11** — eyelid silhouette, open/morph, sharingan spin (target motion language).
- **#12–#14** — sharingan / rinnegan / multi-type designs (target pattern art).

Use these for visual acceptance tests in harness GIFs before on-device flash.

---

## 8. Success criteria

### Firmware (Phase 0–2)

- [ ] No task watchdog under normal AMOLED use.
- [ ] Info screen shows plausible battery data on both boards.
- [ ] Full screen cycle works via flick both directions.
- [ ] At least **tomoe3** and **rinnegan** scenes read as “cosplay believable” behind the lens.

### PC animator (Phase 3)

- [ ] User can tune eye shape and export a looping GIF without editing code.
- [ ] Exported `.eyv` plays on device in the clip section of the screen list.
- [ ] Offline render looks **clearly better** than live `eye_scene.c` (glow, smoother lids, sharper tomoe).

---

*Generated from `claude_code_chat.md` (from user message at line 8785) and inspection of the `sharingan/` tree. No code changes in this step — plan only.*
