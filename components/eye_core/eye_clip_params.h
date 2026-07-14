#pragma once

/*
 * Per-clip display-transform overrides, persisted in NVS under "eyeclip".
 * When a clip has an override, it takes precedence over the global eye_params_t
 * clip transform fields (clip_off_x, clip_off_y, clip_rot_deg, scale_pct).
 */

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int16_t off_x;       /* x offset from center, display px  */
    int16_t off_y;       /* y offset from center, display px  */
    int16_t rot_deg;     /* rotation 0..359 degrees           */
    int16_t scale_pct;   /* eye size %; 0 = use global scale  */
    int8_t  flip_h;      /* 1 = flip horizontally             */
    int8_t  flip_v;      /* 1 = flip vertically               */
} eye_clip_override_t;

/** Load the per-clip override for `clip_name`.
 *  Returns true if an override exists and was loaded into `out`.
 *  Returns false if no override saved (out is left unchanged). */
bool eye_clip_override_load(const char *clip_name, eye_clip_override_t *out);

/** Save a per-clip override. Returns ESP_OK on success. */
esp_err_t eye_clip_override_save(const char *clip_name, const eye_clip_override_t *p);

/** Delete the per-clip override (revert to global settings). */
void eye_clip_override_clear(const char *clip_name);

#ifdef __cplusplus
}
#endif
