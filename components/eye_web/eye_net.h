#pragma once

/*
 * WiFi + web-server lifecycle, shared by both boards. The board's UI (LVGL
 * sliders on the AMOLED, `wifi`/`bright` console commands on either board)
 * only sets the DESIRED state; a small task here reconciles it (start/stop
 * wifi_ctrl, wifi_portal, eye_webserver), so callers never block. Also owns
 * brightness (NVS-persisted, applied via the board callback) and idle
 * auto-off. Board specifics come in through eye_net_config_t -- no BSP here.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    EYE_NET_AP   = 0,   /* always run our control SoftAP                     */
    EYE_NET_STA  = 1,   /* join saved network; captive portal if none        */
    EYE_NET_AUTO = 2,   /* boot default: try STA, fall back to control SoftAP */
} eye_net_mode_t;

typedef enum {
    EYE_NET_OFF = 0,
    EYE_NET_STARTING,        /* transition in progress            */
    EYE_NET_AP_UP,           /* SoftAP + web UI running           */
    EYE_NET_STA_CONNECTING,
    EYE_NET_STA_UP,          /* joined LAN + web UI running       */
    EYE_NET_PORTAL,          /* captive portal awaiting creds     */
    EYE_NET_FAILED,
} eye_net_state_t;

typedef struct {
    bool            enabled;      /* desired on/off                 */
    eye_net_mode_t  mode;         /* desired AP/STA                 */
    eye_net_state_t state;        /* actual                         */
    char            ssid[33];     /* AP: our SSID; STA: joined SSID */
    char            pass[17];     /* AP password ("" for STA)       */
    char            ip[16];
    char            url[40];      /* http://sharingan-eye.local     */
    int             rssi;         /* STA only                       */
} eye_net_status_t;

typedef struct {
    /* Where uploaded clips land / deletes act / the playlist scans:
     * AMOLED: the SD folder ("/sdcard/eye"); LCD: the FATFS-formatted 'anim'
     * flash partition ("/anim"). */
    const char *clip_dir;
    /* Panel brightness setter (5..100), or NULL if the board can't. */
    void (*brightness_set)(int pct);
    /* Fill *total_kb / *free_kb from the clip-storage filesystem, or NULL. */
    void (*get_storage_info)(uint64_t *total_kb, uint64_t *free_kb);
} eye_net_config_t;

/** Load prefs (brightness), apply them, and start the lifecycle task.
 *  WiFi comes up automatically at boot in EYE_NET_AUTO mode (join saved network,
 *  else open the control SoftAP). cfg is copied. */
esp_err_t eye_net_init(const eye_net_config_t *cfg);

void eye_net_set_enabled(bool on);
void eye_net_set_mode(eye_net_mode_t mode);
void eye_net_get_status(eye_net_status_t *out);

/** The configured clip directory (used by the web server). */
const char *eye_net_clip_dir(void);

/** Fill *total_kb / *free_kb; returns false if no callback configured. */
bool eye_net_storage_info(uint64_t *total_kb, uint64_t *free_kb);

/** Reset the idle auto-off timer (webserver calls this on every request). */
void eye_net_note_activity(void);

/** Panel brightness 5..100, persisted to NVS. No-op without a callback. */
int  eye_net_brightness(void);
void eye_net_set_brightness(int pct);

/** Forget saved STA credentials (next STA enable reopens the portal). */
esp_err_t eye_net_forget_wifi(void);

/** Save home-WiFi credentials and reconnect (AUTO mode). Used by the control
 *  SoftAP's web UI so a board can be onboarded without the captive portal. */
esp_err_t eye_net_provision(const char *ssid, const char *pass);

/** Registers the `wifi` and `bright` console commands. Pass this to
 *  eye_console_set_extra() before eye_demo_run(). */
void eye_net_register_console(void);

/** Two short status lines for the raster info screen (eye_info_set_net_provider
 *  compatible): e.g. "AP SharinganEye-7C3A" / "pw uchiha7C3A". Empty when off. */
void eye_net_info_lines(char *l1, size_t n1, char *l2, size_t n2);

#ifdef __cplusplus
}
#endif
