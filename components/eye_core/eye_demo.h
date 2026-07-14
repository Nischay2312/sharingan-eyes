#pragma once

/*
 * Shared, board-agnostic demo driver.
 *
 * Owns the render loop, the SCREEN list, and the flick-gesture logic so both
 * board firmwares behave identically. The board supplies:
 *   - a push_frame function (how finished frames reach its panel), and
 *   - the I2C bus the QMI8658 lives on (or NULL to skip the IMU).
 *
 * Screens form one flat cycle, traversed by gyro flicks (forward flick = next,
 * opposite flick = previous) or the console:
 *
 *   [0] info  ->  [1..N] video clips (SD/flash playlist)  ->  dojutsu scenes
 *    ^                                                             |
 *    +------------------------------ wraps ------------------------+
 *
 * The device boots on the info screen.
 */

#include <stddef.h>
#include <stdint.h>
#include "driver/i2c_master.h"

#include "eye_ui.h"
#include "eye_params.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Signature of the board's frame-presenter (see eye_display.h).
 * scale_pct sizes the eye on the panel (100 = the board's default fit); the
 * board clamps it to what its panel can show.
 */
typedef void (*eye_push_frame_fn)(const uint16_t *fb240, int scale_pct);

/**
 * @brief Run the eye demo forever (never returns).
 *
 * Allocates the 240x240 framebuffer, initializes the renderer, IMU, video
 * playlist and console, then starts the render + gesture tasks.
 *
 * @param push_frame  Board frame presenter (required).
 * @param imu_bus     I2C bus for the QMI8658, or NULL to disable the gesture.
 * @param ui          Native info-screen hooks, or NULL for the built-in raster
 *                    info screen. Not copied -- must stay valid (use a static).
 */
void eye_demo_run(eye_push_frame_fn push_frame, i2c_master_bus_handle_t imu_bus,
                  const eye_ui_hooks_t *ui);

/* ---- screen list (used by the console; safe from any task) ---- */

int  eye_demo_screen_count(void);
int  eye_demo_screen(void);                       /* current screen index    */
void eye_demo_set_screen(int idx);                /* wraps; runs enter hooks */
void eye_demo_step_screen(int delta);             /* +1 next / -1 previous   */
void eye_demo_describe_screen(int idx, char *buf, size_t n);
int  eye_demo_screen_for_scene(int scene_id);     /* scene id -> screen idx  */
int  eye_demo_first_clip_screen(void);            /* -1 when no clips        */

/** The live parameter block (for the web server; same one the console uses). */
eye_params_t *eye_demo_params(void);

/** Re-read the clip count after eye_video_rescan() changed the playlist. */
void eye_demo_refresh_clips(void);

/** Re-read the per-clip NVS override for the currently playing clip.
 *  Call after the web API saves new per-clip params so they take effect
 *  immediately without needing a clip switch. No-op if not on a clip screen. */
void eye_demo_reload_clip_override(void);

/** Boot-screen clip: the device plays this clip (matched by display name) at
 *  power-on instead of the info screen. Empty/NULL name => boot on info (the
 *  default). Persisted to NVS; takes effect next boot. */
void eye_demo_set_startup_clip(const char *name);
void eye_demo_get_startup_clip(char *buf, size_t n);

#ifdef __cplusplus
}
#endif
