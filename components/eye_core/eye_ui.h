#pragma once

/*
 * Optional board-provided "native" info screen -- the board-specific
 * counterpart of eye_display.h, in the same spirit as eye_battery.h. eye_core
 * calls these; each board firmware provides an implementation (there is NO
 * default in eye_core):
 *
 *   - BOTH boards now use the shared components/eye_ui_lvgl/eye_ui_lvgl.c:
 *     LVGL widgets (battery ring arc, Montserrat text, WiFi switches,
 *     brightness slider) locked through esp_lvgl_port. A board without LVGL
 *     would pass NULL hooks and get the built-in raster info screen
 *     (eye_info_render) instead.
 *
 * A board that has a native UI passes an eye_ui_hooks_t to eye_demo_run(); the
 * engine then drives the info screen through it and does NOT rasterise
 * eye_info_render() into the framebuffer. Passing NULL (or hooks whose
 * available() returns false) selects the raster fallback. The hooks are handed
 * over as function pointers -- like eye_display_push_frame -- so eye_core keeps
 * no hard link dependency on board code. Implementations take whatever display
 * lock they need; they may be called from the render task.
 */

#include <stdbool.h>

#include "eye_info.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /** True if this board renders the info screen with its own native UI. */
    bool (*available)(void);
    /** Show (true) or hide (false) the native info screen. Showing must also
     *  hide the eye output so the widgets are visible; hiding restores it. */
    void (*show)(bool show);
    /** Refresh dynamic fields (battery, uptime, counts) while info is shown. */
    void (*update)(const eye_info_status_t *st);
} eye_ui_hooks_t;

/* Board implementations (amoled_2_06/main/eye_ui_amoled.c). app_main wires
 * these into an eye_ui_hooks_t and passes it to eye_demo_run(). */
bool eye_ui_available(void);
void eye_ui_info_show(bool show);
void eye_ui_info_update(const eye_info_status_t *st);

#ifdef __cplusplus
}
#endif
