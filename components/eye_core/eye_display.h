#pragma once

/*
 * Board-specific display contract for the animated-eye demo.
 *
 * These two functions are the ONLY seam that differs between boards. Each board
 * provides its own implementation in boards/<board>/main/eye_display_*.c:
 *
 *   - AMOLED 2.06  -> eye_display_amoled.c  (scale 240->panel width, center)
 *   - 1.28" GC9A01 -> eye_display_gc9a01.c  (1:1 copy)
 *
 * eye_core itself never calls these directly -- the board passes
 * eye_display_push_frame to eye_demo_run() as a function pointer -- so the
 * shared engine stays free of any board/BSP dependency.
 */

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Bring up the physical panel (reuse the board BSP display bring-up). */
esp_err_t eye_display_init(void);

/**
 * @brief Present one finished 240x240 RGB565 frame on the panel.
 * @param fb240      Source 240x240 RGB565 buffer.
 * @param scale_pct  Eye size on the panel; 100 = the board's default fit. The
 *                   board clamps it to what its panel can physically show.
 */
void eye_display_push_frame(const uint16_t *fb240, int scale_pct);

/**
 * @brief Set the clip display transform applied by the next push_frame call.
 *
 * Call this just before eye_display_push_frame whenever the clip transform
 * parameters change. Boards that don't support transform may provide a no-op.
 * A weak default no-op is defined in eye_demo.c so boards compile without it.
 *
 * @param off_x    X offset from panel center in display pixels (-200..200).
 * @param off_y    Y offset from panel center in display pixels (-200..200).
 * @param rot_deg  Rotation 0..359 degrees (clockwise).
 */
void eye_display_set_transform(int off_x, int off_y, int rot_deg);

#ifdef __cplusplus
}
#endif
