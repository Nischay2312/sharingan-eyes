#pragma once

/*
 * Boot/info screen: firmware version, build date/time, board name, battery
 * state (via eye_battery.h), playlist/scene counts, uptime. This is screen 0
 * of the demo's screen list -- the device boots showing it and a flick moves
 * on to the clips.
 */

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#include "eye_battery.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Everything the info screen shows, gathered once so the raster fallback and
 *  a board's native (LVGL) UI draw from the exact same source. */
typedef struct {
    const char   *board;        /* human board name (eye_info_set_board)   */
    const char   *fw_version;   /* app version string                      */
    const char   *build_date;   /* compile date                            */
    const char   *build_time;   /* compile time                            */
    const char   *idf_version;  /* ESP-IDF version                         */
    eye_battery_t bat;          /* last battery reading                    */
    bool          bat_valid;    /* the reading above is usable             */
    int           clips;        /* number of video clips in the playlist   */
    int           scenes;       /* number of built-in dojutsu scenes       */
    uint32_t      uptime_s;     /* seconds since boot                      */
} eye_info_status_t;

/** Set the human-readable board name shown on the screen (call from app_main
 *  before eye_demo_run; the pointer must stay valid -- use a string literal). */
void eye_info_set_board(const char *name);

/** Optional provider of two short network-status lines for the RASTER info
 *  screen (boards without a native UI). eye_net_info_lines matches this
 *  signature; when both lines come back empty the usual hint is drawn. */
typedef void (*eye_info_net_fn)(char *l1, size_t n1, char *l2, size_t n2);
void eye_info_set_net_provider(eye_info_net_fn fn);

/** Fill `st` with the current info fields. Refreshes the battery reading at
 *  most once per second internally, so calling every frame is cheap. */
void eye_info_gather(eye_info_status_t *st, uint32_t t_ms);

/** Render the info screen into the 240x240 fb (raster fallback for boards
 *  without a native UI). Uses eye_info_gather() internally. */
void eye_info_render(uint16_t *fb, uint32_t t_ms);

#ifdef __cplusplus
}
#endif
