#pragma once

/*
 * The in-house dojutsu engine: a realistic anime-style EYE (not a full-screen
 * pattern) rendered procedurally at 240x240 -- eyelid silhouette, the eye
 * opening/closing, natural blinks, subtle gaze saccades, a spinning iris
 * pattern with occasional "flares", and a specular glint. Switching scenes
 * plays a blink transition (close -> morph -> open), like the reference GIFs.
 *
 * Scene ids/names live in eye_params.h (eye_scene_id_t / eye_scene_name) so
 * the console and NVS blob share them.
 */

#include <stdint.h>
#include "esp_err.h"
#include "eye_params.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Allocate the polar LUTs (PSRAM preferred). Call once before rendering. */
esp_err_t eye_scene_init(void);

/** Currently shown scene id. */
int eye_scene_current(void);

/**
 * Switch to scene `id` with a blink transition (the eye closes, the pattern
 * morphs, the eye reopens). Selecting the current scene replays the blink.
 */
void eye_scene_select(int id);

/**
 * Jump straight to scene `id` and play the full dramatic eye-OPENING
 * animation -- used when arriving at a scene from another screen (info /
 * clips), as opposed to morphing between two scenes.
 */
void eye_scene_enter(int id);

/**
 * Render one frame into the 240x240 RGB565 buffer.
 * @param t_ms  monotonic ms (e.g. esp_timer); speed scaling happens inside.
 * @param p     live parameters (colors, spin, blink/gaze/flare behavior).
 */
void eye_scene_render(uint16_t *fb, uint32_t t_ms, const eye_params_t *p);

#ifdef __cplusplus
}
#endif
