/*
 * Cross-board mesh over ESP-NOW: discovery (ANNOUNCE + CLIP list broadcasts,
 * cached into a roster) and addressed remote control (directed CMD frames).
 * See eye_sync.h for the model.
 *
 * All frames start with {magic, ver, ftype} so the receive callback can demux.
 * Directed commands are still sent to the broadcast address with the intended
 * target MAC in the payload, so we never manage per-peer ESP-NOW peers -- each
 * board just ignores frames not addressed to it (or to the broadcast target).
 */

#include "eye_sync.h"

#include <string.h>
#include <strings.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "eye_demo.h"
#include "eye_params.h"
#include "eye_video.h"
#include "eye_info.h"
#include "eye_net.h"

static const char *TAG = "eye_sync";

#define SYNC_MAGIC   0x53
#define SYNC_VER     3
#define OFFLINE_US   (6LL * 1000000)     /* peer silent this long -> offline   */
#define ANNOUNCE_MS  2000

enum { FRAME_ANNOUNCE = 1, FRAME_CLIP = 2, FRAME_CMD = 3 };

/* ------------------------------------------------------------ wire types -- */

typedef struct __attribute__((packed)) {
    uint8_t  magic, ver, ftype, _p;
    uint8_t  mac[6];
    char     name[24];
    char     board[28];
    char     ip[16];
    int16_t  screen, screens, first_clip;
    uint8_t  scene, brightness, flick, flip_h, flip_v;
    uint8_t  col_r, col_g, col_b;
    int16_t  speed, scale;
    int16_t  spin_ms, pupil_px, blink_ms, gaze_px, flare_ms, flick_hi;
    uint8_t  playing_kind, clip_count;
    char     playing[EYE_SYNC_NAME_LEN];
    char     startup[EYE_SYNC_NAME_LEN];
    uint32_t uptime_s;
} ann_t;

typedef struct __attribute__((packed)) {
    uint8_t  magic, ver, ftype, _p;
    uint8_t  mac[6];
    uint8_t  index, total;
    char     name[EYE_SYNC_NAME_LEN];
} clip_t;

typedef struct __attribute__((packed)) {
    uint8_t  magic, ver, ftype, _p;
    uint8_t  target[6];               /* FF*6 = all boards */
    uint8_t  ctype, _p2[3];
    char     key[16];
    char     name[EYE_SYNC_NAME_LEN];
    int32_t  a, b, c;
} cmd_wire_t;

static const uint8_t BCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

/* ---------------------------------------------------------------- state -- */

typedef struct {
    bool        used;
    int64_t     seen;
    eye_board_t b;
} peer_t;

static volatile bool     s_active;
static uint8_t           s_self_mac[6];
static char              s_self_id[6];
static peer_t           *s_peers;     /* allocated from PSRAM in eye_sync_init */
static SemaphoreHandle_t s_lock;
static QueueHandle_t     s_q;         /* incoming commands to apply */
static bool             s_started;    /* tasks/queue created once   */

bool eye_sync_active(void)        { return s_active; }
const char *eye_sync_self_id(void){ return s_self_id; }

static void id_from_mac(const uint8_t *mac, char *out /*[6]*/)
{
    snprintf(out, 6, "%02X%02X", mac[4], mac[5]);
}

/* -------------------------------------------------------- self snapshot -- */

static void fill_self(eye_board_t *b)
{
    memset(b, 0, sizeof(*b));
    memcpy(b->mac, s_self_mac, 6);
    strncpy(b->id, s_self_id, sizeof(b->id) - 1);
    snprintf(b->name, sizeof(b->name), "SharinganEye-%s", s_self_id);

    eye_info_status_t st;
    eye_info_gather(&st, (uint32_t)(esp_timer_get_time() / 1000));
    strncpy(b->board, st.board ? st.board : "", sizeof(b->board) - 1);
    b->uptime_s = st.uptime_s;

    eye_net_status_t net;
    eye_net_get_status(&net);
    strncpy(b->ip, net.ip, sizeof(b->ip) - 1);

    b->is_self    = true;
    b->online     = true;
    b->screen     = eye_demo_screen();
    b->screens    = eye_demo_screen_count();
    b->first_clip = eye_demo_first_clip_screen();

    eye_params_t *p = eye_demo_params();
    b->scene      = p->scene;
    b->brightness = eye_net_brightness();
    b->speed      = p->speed_pct;
    b->scale      = p->scale_pct;
    b->flip_h     = p->flip_h;
    b->flip_v     = p->flip_v;
    b->flick      = p->flick_enabled;
    b->spin_ms    = p->spin_ms;
    b->pupil_px   = p->pupil_px;
    b->blink_ms   = p->blink_ms;
    b->gaze_px    = p->gaze_px;
    b->flare_ms   = p->flare_ms;
    b->flick_hi      = p->flick_hi_dps;
    b->clip_off_x    = p->clip_off_x;
    b->clip_off_y    = p->clip_off_y;
    b->clip_rot_deg  = p->clip_rot_deg;
    b->col_r      = p->col_r[p->scene];
    b->col_g      = p->col_g[p->scene];
    b->col_b      = p->col_b[p->scene];

    int nclips = eye_video_count();
    int scr    = b->screen;
    if (scr <= 0) {
        b->playing_kind = EYE_PLAY_INFO;
        strncpy(b->playing, "info", sizeof(b->playing) - 1);
    } else if (scr <= nclips) {
        b->playing_kind = EYE_PLAY_CLIP;
        strncpy(b->playing, eye_video_clip_name(scr - 1), sizeof(b->playing) - 1);
    } else {
        b->playing_kind = EYE_PLAY_SCENE;
        strncpy(b->playing, eye_scene_name(scr - 1 - nclips), sizeof(b->playing) - 1);
    }

    eye_demo_get_startup_clip(b->startup, sizeof(b->startup));

    b->clip_count = nclips < EYE_SYNC_MAX_CLIPS ? nclips : EYE_SYNC_MAX_CLIPS;
    for (int i = 0; i < b->clip_count; i++) {
        strncpy(b->clips[i], eye_video_clip_name(i), EYE_SYNC_NAME_LEN - 1);
    }
}

/* ------------------------------------------------------------- roster ---- */

static peer_t *get_peer_locked(const uint8_t *mac)
{
    peer_t *free_slot = NULL, *oldest = NULL;
    for (int i = 0; i < EYE_SYNC_MAX_BOARDS; i++) {
        if (s_peers[i].used && memcmp(s_peers[i].b.mac, mac, 6) == 0) {
            return &s_peers[i];
        }
        if (!s_peers[i].used && free_slot == NULL) {
            free_slot = &s_peers[i];
        }
        if (s_peers[i].used && (oldest == NULL || s_peers[i].seen < oldest->seen)) {
            oldest = &s_peers[i];
        }
    }
    peer_t *slot = free_slot ? free_slot : oldest;
    if (slot) {
        memset(slot, 0, sizeof(*slot));
        slot->used = true;
        memcpy(slot->b.mac, mac, 6);
        id_from_mac(mac, slot->b.id);
    }
    return slot;
}

int eye_sync_peer_count(void)
{
    int n = 0;
    int64_t now = esp_timer_get_time();
    if (s_lock == NULL) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < EYE_SYNC_MAX_BOARDS; i++) {
        if (s_peers[i].used && now - s_peers[i].seen < OFFLINE_US) {
            n++;
        }
    }
    xSemaphoreGive(s_lock);
    return n;
}

int eye_sync_roster(eye_board_t *out, int max)
{
    if (out == NULL || max <= 0) {
        return 0;
    }
    int n = 0;
    fill_self(&out[n++]);
    if (s_lock) {
        int64_t now = esp_timer_get_time();
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (int i = 0; i < EYE_SYNC_MAX_BOARDS && n < max; i++) {
            if (s_peers[i].used && now - s_peers[i].seen < OFFLINE_US) {
                out[n] = s_peers[i].b;
                out[n].is_self = false;
                out[n].online  = true;
                n++;
            }
        }
        xSemaphoreGive(s_lock);
    }
    return n;
}

static bool lookup_mac(const char *id, uint8_t *mac_out)
{
    bool found = false;
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < EYE_SYNC_MAX_BOARDS; i++) {
        if (s_peers[i].used && now - s_peers[i].seen < OFFLINE_US &&
            strcasecmp(s_peers[i].b.id, id) == 0) {
            memcpy(mac_out, s_peers[i].b.mac, 6);
            found = true;
            break;
        }
    }
    xSemaphoreGive(s_lock);
    return found;
}

/* --------------------------------------------------------- apply command -- */

static void eye_cmd_apply(const eye_cmd_t *c)
{
    eye_params_t *p = eye_demo_params();
    switch (c->type) {
    case EYE_CMD_PARAM:
        eye_params_set(p, c->key, c->a);
        break;
    case EYE_CMD_COLOR:
        eye_params_set_color(p, (uint8_t)c->a, (uint8_t)c->b, (uint8_t)c->c);
        break;
    case EYE_CMD_SCENE:
        if (c->a >= 0 && c->a < EYE_SCENE_COUNT) {
            eye_demo_set_screen(eye_demo_screen_for_scene(c->a));
        }
        break;
    case EYE_CMD_SCREEN:
        eye_demo_set_screen(c->a);
        break;
    case EYE_CMD_STEP:
        eye_demo_step_screen(c->a >= 0 ? 1 : -1);
        break;
    case EYE_CMD_CLIP: {
        int idx   = eye_video_index_for_name(c->name);
        int first = eye_demo_first_clip_screen();
        if (idx >= 0 && first >= 0) {
            eye_demo_set_screen(first + idx);
        }
        break;
    }
    case EYE_CMD_CLIPACT:
        if (strcmp(c->key, "stop") == 0) {
            eye_demo_set_screen(eye_demo_screen_for_scene(p->scene));
        } else if (strcmp(c->key, "play") == 0) {
            int scr = eye_demo_first_clip_screen();
            if (scr >= 0) {
                eye_demo_set_screen(scr);
            }
        }
        break;
    case EYE_CMD_BRIGHT:
        eye_net_set_brightness(c->a);
        break;
    case EYE_CMD_FLIP:
        p->flip_h = c->a ? 1 : 0;
        p->flip_v = c->b ? 1 : 0;
        break;
    case EYE_CMD_FLICK:
        p->flick_enabled = c->a != 0;
        break;
    case EYE_CMD_PERSIST:
        if (strcmp(c->key, "save") == 0) {
            eye_params_save(p);
        } else if (strcmp(c->key, "load") == 0) {
            eye_params_load(p);
        } else if (strcmp(c->key, "reset") == 0) {
            int scene = p->scene;
            eye_params_init(p);
            p->scene = scene;
        }
        break;
    case EYE_CMD_STARTUP:
        eye_demo_set_startup_clip(c->name);
        break;
    case EYE_CMD_SYNC:
        eye_video_restart();
        break;
    case EYE_CMD_PAUSE:
        eye_video_pause(c->a != 0);
        break;
    default:
        break;
    }
}

static void apply_task(void *arg)
{
    (void)arg;
    eye_cmd_t c;
    for (;;) {
        if (xQueueReceive(s_q, &c, portMAX_DELAY) == pdTRUE) {
            eye_cmd_apply(&c);
        }
    }
}

/* ------------------------------------------------------------- receive --- */

static void recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (info == NULL || data == NULL || len < 4) {
        return;
    }
    if (data[0] != SYNC_MAGIC || data[1] != SYNC_VER) {
        return;
    }
    switch (data[2]) {
    case FRAME_ANNOUNCE: {
        if (len != (int)sizeof(ann_t)) {
            return;
        }
        const ann_t *a = (const ann_t *)data;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        peer_t *pp = get_peer_locked(a->mac);
        if (pp) {
            eye_board_t *b = &pp->b;
            strncpy(b->name, a->name, sizeof(b->name) - 1);
            strncpy(b->board, a->board, sizeof(b->board) - 1);
            strncpy(b->ip, a->ip, sizeof(b->ip) - 1);
            b->screen = a->screen;   b->screens = a->screens;
            b->first_clip = a->first_clip;
            b->scene = a->scene;     b->brightness = a->brightness;
            b->speed = a->speed;     b->scale = a->scale;
            b->flip_h = a->flip_h;   b->flip_v = a->flip_v;
            b->flick = a->flick;
            b->spin_ms = a->spin_ms;   b->pupil_px = a->pupil_px;
            b->blink_ms = a->blink_ms; b->gaze_px = a->gaze_px;
            b->flare_ms = a->flare_ms; b->flick_hi = a->flick_hi;
            b->col_r = a->col_r;     b->col_g = a->col_g;  b->col_b = a->col_b;
            b->playing_kind = a->playing_kind;
            strncpy(b->playing, a->playing, sizeof(b->playing) - 1);
            strncpy(b->startup, a->startup, sizeof(b->startup) - 1);
            b->clip_count = a->clip_count < EYE_SYNC_MAX_CLIPS
                          ? a->clip_count : EYE_SYNC_MAX_CLIPS;
            b->uptime_s = a->uptime_s;
            pp->seen = esp_timer_get_time();
        }
        xSemaphoreGive(s_lock);
        break;
    }
    case FRAME_CLIP: {
        if (len != (int)sizeof(clip_t)) {
            return;
        }
        const clip_t *c = (const clip_t *)data;
        if (c->index >= EYE_SYNC_MAX_CLIPS) {
            return;
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        peer_t *pp = get_peer_locked(c->mac);
        if (pp) {
            strncpy(pp->b.clips[c->index], c->name, EYE_SYNC_NAME_LEN - 1);
            if (c->total > pp->b.clip_count) {
                pp->b.clip_count = c->total < EYE_SYNC_MAX_CLIPS
                                 ? c->total : EYE_SYNC_MAX_CLIPS;
            }
            pp->seen = esp_timer_get_time();
        }
        xSemaphoreGive(s_lock);
        break;
    }
    case FRAME_CMD: {
        if (len != (int)sizeof(cmd_wire_t)) {
            return;
        }
        const cmd_wire_t *w = (const cmd_wire_t *)data;
        bool for_us = memcmp(w->target, s_self_mac, 6) == 0 ||
                      memcmp(w->target, BCAST, 6) == 0;
        if (!for_us || s_q == NULL) {
            return;
        }
        eye_cmd_t c = { .type = w->ctype, .a = w->a, .b = w->b, .c = w->c };
        memcpy(c.key, w->key, sizeof(c.key));
        memcpy(c.name, w->name, sizeof(c.name));
        c.key[sizeof(c.key) - 1] = '\0';
        c.name[sizeof(c.name) - 1] = '\0';
        xQueueSend(s_q, &c, 0);
        break;
    }
    default:
        break;
    }
}

/* -------------------------------------------------------------- announce -- */

static void send_announce(void)
{
    eye_board_t self;
    fill_self(&self);

    ann_t a = { .magic = SYNC_MAGIC, .ver = SYNC_VER, .ftype = FRAME_ANNOUNCE };
    memcpy(a.mac, self.mac, 6);
    strncpy(a.name, self.name, sizeof(a.name) - 1);
    strncpy(a.board, self.board, sizeof(a.board) - 1);
    strncpy(a.ip, self.ip, sizeof(a.ip) - 1);
    a.screen = self.screen;   a.screens = self.screens;
    a.first_clip = self.first_clip;
    a.scene = self.scene;     a.brightness = self.brightness;
    a.flick = self.flick;     a.flip_h = self.flip_h;  a.flip_v = self.flip_v;
    a.col_r = self.col_r;     a.col_g = self.col_g;    a.col_b = self.col_b;
    a.speed = self.speed;     a.scale = self.scale;
    a.spin_ms = self.spin_ms; a.pupil_px = self.pupil_px;
    a.blink_ms = self.blink_ms; a.gaze_px = self.gaze_px;
    a.flare_ms = self.flare_ms; a.flick_hi = self.flick_hi;
    a.playing_kind = self.playing_kind;
    a.clip_count = self.clip_count;
    strncpy(a.playing, self.playing, sizeof(a.playing) - 1);
    strncpy(a.startup, self.startup, sizeof(a.startup) - 1);
    a.uptime_s = self.uptime_s;
    esp_now_send(BCAST, (const uint8_t *)&a, sizeof(a));

    /* Clip list, one small frame per clip (so peers can offer them by name). */
    for (int i = 0; i < self.clip_count; i++) {
        clip_t c = { .magic = SYNC_MAGIC, .ver = SYNC_VER, .ftype = FRAME_CLIP,
                     .index = (uint8_t)i, .total = (uint8_t)self.clip_count };
        memcpy(c.mac, self.mac, 6);
        strncpy(c.name, self.clips[i], sizeof(c.name) - 1);
        esp_now_send(BCAST, (const uint8_t *)&c, sizeof(c));
        vTaskDelay(pdMS_TO_TICKS(5));   /* don't overrun the ESP-NOW tx queue */
    }
}

static void announce_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (s_active) {
            send_announce();
        }
        vTaskDelay(pdMS_TO_TICKS(ANNOUNCE_MS));
    }
}

/* ------------------------------------------------------------- dispatch --- */

static void send_cmd(const uint8_t *target, const eye_cmd_t *cmd)
{
    if (!s_active) {
        return;
    }
    cmd_wire_t w = { .magic = SYNC_MAGIC, .ver = SYNC_VER, .ftype = FRAME_CMD,
                     .ctype = cmd->type, .a = cmd->a, .b = cmd->b, .c = cmd->c };
    memcpy(w.target, target, 6);
    memcpy(w.key, cmd->key, sizeof(w.key));
    memcpy(w.name, cmd->name, sizeof(w.name));
    esp_now_send(BCAST, (const uint8_t *)&w, sizeof(w));
}

eye_route_t eye_sync_dispatch(const char *target_id, const eye_cmd_t *cmd)
{
    if (cmd == NULL) {
        return EYE_ROUTE_UNKNOWN;
    }
    if (target_id == NULL || target_id[0] == '\0' ||
        strcasecmp(target_id, s_self_id) == 0) {
        eye_cmd_apply(cmd);
        return EYE_ROUTE_LOCAL;
    }
    if (strcasecmp(target_id, "all") == 0) {
        eye_cmd_apply(cmd);
        send_cmd(BCAST, cmd);
        return EYE_ROUTE_ALL;
    }
    uint8_t mac[6];
    if (s_lock && lookup_mac(target_id, mac)) {
        send_cmd(mac, cmd);
        return EYE_ROUTE_REMOTE;
    }
    return EYE_ROUTE_UNKNOWN;
}

/* ------------------------------------------------------------- lifecycle -- */

esp_err_t eye_sync_init(void)
{
    if (s_active) {
        return ESP_OK;
    }
    if (!s_started) {
        esp_read_mac(s_self_mac, ESP_MAC_WIFI_STA);
        id_from_mac(s_self_mac, s_self_id);
        /* Peers table is large (eye_board_t × MAX_BOARDS); allocate from PSRAM
         * so the static BSS doesn't eat internal RAM needed for task stacks. */
        s_peers = heap_caps_calloc(EYE_SYNC_MAX_BOARDS, sizeof(peer_t),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_peers) {
            s_peers = calloc(EYE_SYNC_MAX_BOARDS, sizeof(peer_t));
        }
        s_lock = xSemaphoreCreateMutex();
        s_q    = xQueueCreate(8, sizeof(eye_cmd_t));
        if (s_peers == NULL || s_lock == NULL || s_q == NULL) {
            return ESP_ERR_NO_MEM;
        }
        xTaskCreatePinnedToCore(apply_task,    "eye_sync",  4096, NULL, 3, NULL, 0);
        xTaskCreatePinnedToCore(announce_task, "eye_ann",   6144, NULL, 3, NULL, 0);
        s_started = true;
    }

    esp_err_t err = esp_now_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_init: %s", esp_err_to_name(err));
        return err;
    }
    esp_now_register_recv_cb(recv_cb);

    esp_now_peer_info_t peer = { 0 };
    memcpy(peer.peer_addr, BCAST, 6);
    peer.ifidx   = WIFI_IF_STA;
    peer.channel = 0;
    peer.encrypt = false;
    err = esp_now_add_peer(&peer);
    if (err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST) {
        ESP_LOGE(TAG, "add broadcast peer: %s", esp_err_to_name(err));
        esp_now_deinit();
        return err;
    }

    s_active = true;
    ESP_LOGI(TAG, "mesh up as %s (broadcast)", s_self_id);
    return ESP_OK;
}

void eye_sync_deinit(void)
{
    if (!s_active) {
        return;
    }
    s_active = false;
    esp_now_unregister_recv_cb();
    esp_now_del_peer(BCAST);
    esp_now_deinit();
}
