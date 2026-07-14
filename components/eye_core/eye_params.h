#pragma once

/*
 * Live-tunable parameters shared by the renderer, the flick-gesture task, and
 * the serial console. One mutable eye_params_t is owned by the demo; the console
 * task writes fields and the render loop reads them each frame. Fields are plain
 * scalars (aligned int/byte writes are atomic on the ESP32-S3 and any race just
 * shows up as a one-frame visual blip), so no lock is needed for a demo.
 */

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The in-house dojutsu scenes rendered by eye_scene.c -- each one is the same
 * realistic animated eye (lids, blinks, gaze) with a different iris pattern.
 */
typedef enum {
    EYE_SCENE_TOMOE1 = 0,       /* one-tomoe Sharingan                       */
    EYE_SCENE_TOMOE2,           /* two-tomoe Sharingan                       */
    EYE_SCENE_TOMOE3,           /* three-tomoe Sharingan                     */
    EYE_SCENE_MANGEKYOU,        /* Mangekyou (Obito/Kakashi pinwheel)        */
    EYE_SCENE_ITACHI,           /* Mangekyou (Itachi triple blade)           */
    EYE_SCENE_RINNEGAN,         /* Rinnegan: ripple rings fill the whole eye */
    EYE_SCENE_TOMOE_RINNEGAN,   /* Rinnegan rings + 9 tomoe                  */
    EYE_SCENE_BYAKUGAN,         /* pale, pupil-less                          */
    EYE_SCENE_COUNT
} eye_scene_id_t;

typedef struct {
    int scene;        /* current eye_scene_id_t (what the scene screens show) */
    int speed_pct;    /* global animation speed scale, 10..2000 (100=norm)    */
    int scale_pct;    /* eye size on the panel, 20..130 (100 = default fit)   */
    int flip_h;       /* 1 = mirror the framebuffer horizontally              */
    int flip_v;       /* 1 = mirror the framebuffer vertically                */

    /* Per-scene primary color (indexed by eye_scene_id_t). */
    uint8_t col_r[EYE_SCENE_COUNT];
    uint8_t col_g[EYE_SCENE_COUNT];
    uint8_t col_b[EYE_SCENE_COUNT];

    /* Realistic-eye behavior (eye_scene.c). */
    int spin_ms;      /* iris pattern ms/revolution; sign = direction         */
    int pupil_px;     /* pupil radius, px                                     */
    int blink_ms;     /* average gap between natural blinks, ms (0 = off)     */
    int gaze_px;      /* max gaze-saccade amplitude, px (0 = eye stays still) */
    int flare_ms;     /* average gap between spin "flares", ms (0 = off)      */

    /* Flick gesture (gyro) */
    int  flick_hi_dps;      /* trigger threshold, deg/s                       */
    int  flick_lo_dps;      /* re-arm threshold, deg/s                        */
    bool flick_enabled;     /* enable flick-to-cycle                          */

    /* GIF / clip display transform (applied when a video clip is playing).
     * Per-clip overrides (eye_clip_params) take precedence when available. */
    int clip_off_x;         /* X offset from center, display px (-200..200)   */
    int clip_off_y;         /* Y offset from center, display px (-200..200)   */
    int clip_rot_deg;       /* rotation 0..359 degrees                        */
} eye_params_t;

/** Populate p with sensible defaults for all scenes. */
void eye_params_init(eye_params_t *p);

/** Human-readable scene name (e.g. "tomoe3"), or "?" if out of range. */
const char *eye_scene_name(int scene);

/** Parse a scene name OR a numeric index; returns -1 if unrecognized. */
int eye_scene_from_str(const char *s);

/**
 * @brief Set one scalar parameter by key (e.g. "speed", "blink", "gaze").
 * @return true if the key is known and the value was applied (clamped).
 */
bool eye_params_set(eye_params_t *p, const char *key, int value);

/** Set the current scene's primary color (clamped to 0..255). */
void eye_params_set_color(eye_params_t *p, uint8_t r, uint8_t g, uint8_t b);

/** Print the full parameter set to stdout (for the console `get` command). */
void eye_params_print(const eye_params_t *p);

/** Persist / restore the whole struct to NVS (namespace "eye"). */
esp_err_t eye_params_save(const eye_params_t *p);
esp_err_t eye_params_load(eye_params_t *p);

#ifdef __cplusplus
}
#endif
