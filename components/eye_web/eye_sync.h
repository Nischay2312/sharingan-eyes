#pragma once

/*
 * Cross-board mesh over ESP-NOW: discovery + addressed remote control.
 *
 * Every board periodically broadcasts an ANNOUNCE (identity + live status: which
 * screen/clip is showing, its params, its startup clip) and its CLIP list. Each
 * board caches a ROSTER of everyone it has heard from, so any board's web panel
 * can list all eyes that are online and show what each is doing.
 *
 * Control is ADDRESSED, not mirrored: the web UI picks a target board and every
 * command is routed to just that board (or "all"). A command to the local board
 * is applied directly; a command to a peer is sent as a directed ESP-NOW frame
 * that only the matching board acts on. This lets you drive several eyes from
 * one page and still give each its own settings.
 *
 * Clip FILES are never sent over the mesh (too big; storage is board-local) --
 * you upload clips by connecting to each board directly. Only names/selection
 * and settings travel over ESP-NOW.
 *
 * Works whenever WiFi is up (eye_net brings sync up/down with the web server),
 * as long as the boards share a radio channel (both on one router, or both on
 * their own SoftAP -- all default to channel 1).
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EYE_SYNC_MAX_BOARDS  8
#define EYE_SYNC_MAX_CLIPS   24
#define EYE_SYNC_NAME_LEN    64

/* What a board is currently showing (matches eye_demo's screen kinds). */
enum { EYE_PLAY_INFO = 0, EYE_PLAY_CLIP = 1, EYE_PLAY_SCENE = 2 };

/* One board in the mesh roster (self or a peer). */
typedef struct {
    char     id[6];        /* stable short id = MAC4 hex, e.g. "7C3A"   */
    uint8_t  mac[6];
    char     name[24];     /* SoftAP-style name, e.g. SharinganEye-7C3A */
    char     board[28];    /* human board type                          */
    char     ip[16];       /* IPv4 string, e.g. "192.168.4.1"           */
    bool     is_self;
    bool     online;
    int      screen, screens, first_clip;
    int      scene, brightness, speed, scale;
    int      flip_h, flip_v;
    bool     flick;
    int      spin_ms, pupil_px, blink_ms, gaze_px, flare_ms, flick_hi;
    int      clip_off_x, clip_off_y, clip_rot_deg;
    uint8_t  col_r, col_g, col_b;
    int      playing_kind;                  /* EYE_PLAY_*                */
    char     playing[EYE_SYNC_NAME_LEN];    /* current clip/scene name   */
    char     startup[EYE_SYNC_NAME_LEN];    /* configured boot clip ("") */
    int      clip_count;
    char     clips[EYE_SYNC_MAX_CLIPS][EYE_SYNC_NAME_LEN];
    uint32_t uptime_s;
} eye_board_t;

/* ---- lifecycle (called by eye_net alongside the web server) ---- */
esp_err_t eye_sync_init(void);
void      eye_sync_deinit(void);
bool      eye_sync_active(void);

/* ---- roster ---- */
/** Number of peers currently online (excludes self). */
int  eye_sync_peer_count(void);
/** This board's short id (MAC4 hex). */
const char *eye_sync_self_id(void);
/** Fill `out` with self (first) + online peers, newest status. Returns count. */
int  eye_sync_roster(eye_board_t *out, int max);

/* ---- addressed commands ---- */
/* Command opcodes (also used on the wire). */
enum {
    EYE_CMD_PARAM = 1,   /* key + a=value                       */
    EYE_CMD_COLOR,       /* a=r b=g c=b                         */
    EYE_CMD_SCENE,       /* a=scene id                          */
    EYE_CMD_SCREEN,      /* a=screen index                      */
    EYE_CMD_STEP,        /* a=+1/-1                             */
    EYE_CMD_CLIP,        /* name=clip name (select by name)     */
    EYE_CMD_CLIPACT,     /* key="play"|"stop"                   */
    EYE_CMD_BRIGHT,      /* a=pct                               */
    EYE_CMD_FLIP,        /* a=h b=v                             */
    EYE_CMD_FLICK,       /* a=on                                */
    EYE_CMD_PERSIST,     /* key="save"|"load"|"reset"           */
    EYE_CMD_STARTUP,     /* name=boot clip ("" clears)          */
    EYE_CMD_SYNC,        /* restart current clip to frame 0     */
    EYE_CMD_PAUSE,       /* a=1 pause / a=0 resume              */
};

typedef struct {
    uint8_t type;                     /* EYE_CMD_*                     */
    char    key[16];
    char    name[EYE_SYNC_NAME_LEN];
    int32_t a, b, c;
} eye_cmd_t;

typedef enum {
    EYE_ROUTE_LOCAL,    /* applied on this board                       */
    EYE_ROUTE_REMOTE,   /* sent to one peer                            */
    EYE_ROUTE_ALL,      /* applied here + sent to every peer           */
    EYE_ROUTE_UNKNOWN,  /* target id is not self and not a known peer  */
} eye_route_t;

/** Route a command to `target_id` and apply/send as needed:
 *   - NULL/""/self id  -> apply locally           (EYE_ROUTE_LOCAL)
 *   - "all"            -> apply here + all peers   (EYE_ROUTE_ALL)
 *   - a peer's id      -> send to that peer only   (EYE_ROUTE_REMOTE)
 *   - anything else    -> EYE_ROUTE_UNKNOWN (nothing happens)         */
eye_route_t eye_sync_dispatch(const char *target_id, const eye_cmd_t *cmd);

#ifdef __cplusplus
}
#endif
