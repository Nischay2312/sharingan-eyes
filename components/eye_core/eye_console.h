#pragma once

/*
 * Serial command console for live-tuning the eye over UART/USB.
 *
 * Starts an esp_console REPL (works in `idf.py monitor`) that mutates the
 * shared eye_params_t and drives the demo's screen list. Commands: help, list,
 * screen, scene, next, prev, info, play, stop, clip, get, set, color, flick,
 * reset, save, load. See eye_console.c for usage.
 */

#include "esp_err.h"
#include "eye_params.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register commands and start the console REPL task (non-blocking).
 * @param params  The live parameter block the commands operate on (not copied).
 */
esp_err_t eye_console_start(eye_params_t *params);

/**
 * @brief Let the board add extra console commands (e.g. eye_web's `wifi` and
 *        `bright`). Call BEFORE eye_demo_run(); the function runs right after
 *        the core commands are registered, when esp_console is initialized.
 */
void eye_console_set_extra(void (*register_fn)(void));

#ifdef __cplusplus
}
#endif
