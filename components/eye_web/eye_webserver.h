#pragma once

/* The no-auth web control app + REST API (started/stopped by eye_net). */

#include "esp_err.h"

/**
 * Install the log-ring vprintf hook so /dev/console captures log output.
 * Call this early in app_main (before display init) to capture boot messages.
 * Safe to call multiple times.
 */
void eye_webserver_log_init(void);

esp_err_t eye_webserver_start(void);
void      eye_webserver_stop(void);

/**
 * Optional clip-upload progress hook (e.g. the LVGL "Loading animation"
 * overlay). Called from the httpd task with 0..100 while receiving and -1
 * when the upload ends (success or failure) so the overlay can hide.
 */
void eye_webserver_set_upload_cb(void (*cb)(int pct));

/**
 * Optional OTA-update progress hook. Same convention as upload: 0..100
 * during flash writing, -1 when done (reboot imminent) or on error.
 */
void eye_webserver_set_ota_cb(void (*cb)(int pct));
