/*
 * Web control app for the Sharingan eye: esp_http_server + mDNS
 * (http://sharingan-eye.local). Serves the embedded single-page UI
 * (web_ui/index.html) and a JSON REST API over the shared eye engine.
 * No auth by design -- meant for a personal AP / home LAN.
 *
 * Board-agnostic: the clip directory (SD folder on the AMOLED, FATFS on the
 * LCD's 'anim' flash partition) comes from eye_net's config, and uploads/
 * deletes are plain stdio file ops either way.
 */

#include "eye_webserver.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mdns.h"

#include "eye_demo.h"
#include "eye_info.h"
#include "eye_params.h"
#include "eye_video.h"
#include "eye_net.h"
#include "eye_sync.h"
#include "eye_clip_params.h"

static const char *TAG = "eye_web";

#define UPLOAD_MAX     (12 * 1024 * 1024)
#define OTA_UPLOAD_MAX  (4 * 1024 * 1024)

/* --------------------------------------------------------- log ring buffer --
 * Captures ESP log output into a 4 KB ring so /dev/console can show it.
 * Uses a portMUX spinlock (works from tasks and ISRs, before RTOS starts). */
#define LOG_RING_CAP 4096
static char           s_log_buf[LOG_RING_CAP];
static int            s_log_head = 0;    /* index of oldest byte */
static int            s_log_fill = 0;    /* bytes currently stored */
static portMUX_TYPE   s_log_mux  = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_orig_vprintf;
static bool           s_log_hooked;

/* Strip ANSI escape codes (e.g. colour codes the terminal adds). */
static int strip_ansi(const char *src, int srclen, char *dst, int dstcap)
{
    int di = 0;
    bool esc = false;
    for (int i = 0; i < srclen && di < dstcap - 1; i++) {
        unsigned char c = (unsigned char)src[i];
        if (esc) {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == 'm') {
                esc = false;
            }
        } else if (c == 0x1b) {
            esc = true;
        } else {
            dst[di++] = (char)c;
        }
    }
    dst[di] = '\0';
    return di;
}

static void log_ring_push(const char *data, int len)
{
    portENTER_CRITICAL_SAFE(&s_log_mux);
    for (int i = 0; i < len; i++) {
        int wi = (s_log_head + s_log_fill) % LOG_RING_CAP;
        s_log_buf[wi] = data[i];
        if (s_log_fill < LOG_RING_CAP) {
            s_log_fill++;
        } else {
            s_log_head = (s_log_head + 1) % LOG_RING_CAP;
        }
    }
    portEXIT_CRITICAL_SAFE(&s_log_mux);
}

static int log_ring_snapshot(char *out, int outsize)
{
    portENTER_CRITICAL_SAFE(&s_log_mux);
    int fill = s_log_fill < outsize - 1 ? s_log_fill : outsize - 1;
    int start = (s_log_head + s_log_fill - fill + LOG_RING_CAP) % LOG_RING_CAP;
    for (int i = 0; i < fill; i++) {
        out[i] = s_log_buf[(start + i) % LOG_RING_CAP];
    }
    portEXIT_CRITICAL_SAFE(&s_log_mux);
    out[fill] = '\0';
    return fill;
}

static int log_ring_vprintf(const char *fmt, va_list args)
{
    /* Use heap so this hook adds zero stack burden — it's called from every
     * task that emits a log line, including sys_evt (2304-byte stack). */
    char *raw = malloc(256);
    if (raw) {
        va_list a2;
        va_copy(a2, args);
        int rawlen = vsnprintf(raw, 256, fmt, a2);
        va_end(a2);
        if (rawlen > 0) {
            char *stripped = malloc(256);
            if (stripped) {
                int slen = strip_ansi(raw, rawlen < 255 ? rawlen : 255,
                                      stripped, 256);
                if (slen > 0) log_ring_push(stripped, slen);
                free(stripped);
            }
        }
        free(raw);
    }
    return s_orig_vprintf(fmt, args);
}

void eye_webserver_log_init(void)
{
    if (!s_log_hooked) {
        s_orig_vprintf = esp_log_set_vprintf(log_ring_vprintf);
        s_log_hooked   = true;
    }
}

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

static httpd_handle_t s_httpd;
static bool           s_mdns_up;
static void         (*s_upload_cb)(int pct);
static void         (*s_ota_cb)(int pct);

void eye_webserver_set_upload_cb(void (*cb)(int pct)) { s_upload_cb = cb; }
void eye_webserver_set_ota_cb(void (*cb)(int pct))    { s_ota_cb    = cb; }

/* ------------------------------------------------------------- helpers -- */

static esp_err_t send_json(httpd_req_t *req, cJSON *obj)
{
    char *txt = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (txt == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_send(req, txt, HTTPD_RESP_USE_STRLEN);
    free(txt);
    return r;
}

static esp_err_t send_ok(httpd_req_t *req)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    return send_json(req, o);
}

/* Read + parse a small JSON body; caller must cJSON_Delete non-NULL result. */
static cJSON *read_body(httpd_req_t *req)
{
    char buf[256];
    int len = req->content_len;
    if (len <= 0 || len >= (int)sizeof(buf)) {
        return NULL;
    }
    int got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, buf + got, len - got);
        if (r <= 0) {
            return NULL;
        }
        got += r;
    }
    buf[len] = '\0';
    return cJSON_Parse(buf);
}

static int body_int(cJSON *b, const char *key, int fallback)
{
    cJSON *v = cJSON_GetObjectItem(b, key);
    return cJSON_IsNumber(v) ? (int)v->valuedouble : fallback;
}

/* Validate a clip filename: plain basename ending in .eyv, no path tricks. */
static bool clip_name_ok(const char *name)
{
    size_t L = strlen(name);
    return L >= 5 && L < 120 &&
           strcasecmp(name + L - 4, ".eyv") == 0 &&
           strstr(name, "..") == NULL &&
           strchr(name, '/') == NULL && strchr(name, '\\') == NULL;
}

/* -------------------------------------------------------------- routes -- */

static esp_err_t h_index(httpd_req_t *req)
{
    eye_net_note_activity();
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, (const char *)index_html_start,
                           index_html_end - index_html_start);
}

static esp_err_t h_status(httpd_req_t *req)
{
    eye_net_note_activity();

    eye_info_status_t st;
    eye_info_gather(&st, (uint32_t)(esp_timer_get_time() / 1000));
    eye_net_status_t net;
    eye_net_get_status(&net);
    eye_params_t *p = eye_demo_params();

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "fw", st.fw_version);
    cJSON_AddStringToObject(o, "build_date", st.build_date);
    cJSON_AddStringToObject(o, "build_time", st.build_time);
    cJSON_AddStringToObject(o, "idf", st.idf_version);
    cJSON_AddStringToObject(o, "board", st.board);
    cJSON_AddNumberToObject(o, "uptime_s", st.uptime_s);

    cJSON *bat = cJSON_AddObjectToObject(o, "bat");
    cJSON_AddBoolToObject(bat, "present", st.bat_valid && st.bat.present);
    cJSON_AddBoolToObject(bat, "charging", st.bat.charging);
    cJSON_AddNumberToObject(bat, "mv", st.bat.mv);
    cJSON_AddNumberToObject(bat, "pct", st.bat.pct);

    cJSON *w = cJSON_AddObjectToObject(o, "wifi");
    cJSON_AddNumberToObject(w, "mode", net.mode);
    cJSON_AddNumberToObject(w, "state", net.state);
    cJSON_AddStringToObject(w, "ssid", net.ssid);
    cJSON_AddStringToObject(w, "ip", net.ip);
    cJSON_AddStringToObject(w, "url", net.url);
    cJSON_AddNumberToObject(w, "rssi", net.rssi);
    cJSON_AddNumberToObject(w, "peers", eye_sync_peer_count());

    cJSON_AddNumberToObject(o, "brightness", eye_net_brightness());
    cJSON_AddBoolToObject(o, "paused", eye_video_is_paused());
    cJSON_AddNumberToObject(o, "screen", eye_demo_screen());
    cJSON_AddNumberToObject(o, "screens", eye_demo_screen_count());
    cJSON_AddNumberToObject(o, "first_clip", eye_demo_first_clip_screen());

    cJSON *scenes = cJSON_AddArrayToObject(o, "scenes");
    for (int i = 0; i < EYE_SCENE_COUNT; i++) {
        cJSON_AddItemToArray(scenes, cJSON_CreateString(eye_scene_name(i)));
    }
    cJSON *clips = cJSON_AddArrayToObject(o, "clips");
    for (int i = 0; i < eye_video_count(); i++) {
        cJSON_AddItemToArray(clips, cJSON_CreateString(eye_video_clip_name(i)));
    }

    /* Storage stats for the clip directory filesystem. */
    {
        uint64_t total_kb = 0, free_kb = 0;
        if (eye_net_storage_info(&total_kb, &free_kb)) {
            cJSON *stor = cJSON_AddObjectToObject(o, "storage");
            cJSON_AddNumberToObject(stor, "kb_total", (double)total_kb);
            cJSON_AddNumberToObject(stor, "kb_free",  (double)free_kb);
        }
    }

    cJSON *pp = cJSON_AddObjectToObject(o, "params");
    cJSON_AddNumberToObject(pp, "scene", p->scene);
    cJSON_AddNumberToObject(pp, "speed", p->speed_pct);
    cJSON_AddNumberToObject(pp, "scale", p->scale_pct);
    cJSON_AddNumberToObject(pp, "fliph", p->flip_h);
    cJSON_AddNumberToObject(pp, "flipv", p->flip_v);
    cJSON_AddNumberToObject(pp, "sspin", p->spin_ms);
    cJSON_AddNumberToObject(pp, "pupil", p->pupil_px);
    cJSON_AddNumberToObject(pp, "blink", p->blink_ms);
    cJSON_AddNumberToObject(pp, "gaze", p->gaze_px);
    cJSON_AddNumberToObject(pp, "flare", p->flare_ms);
    cJSON_AddNumberToObject(pp, "fhi", p->flick_hi_dps);
    cJSON_AddNumberToObject(pp, "flo", p->flick_lo_dps);
    cJSON_AddBoolToObject(pp, "flick", p->flick_enabled);
    cJSON_AddNumberToObject(pp, "coff_x", p->clip_off_x);
    cJSON_AddNumberToObject(pp, "coff_y", p->clip_off_y);
    cJSON_AddNumberToObject(pp, "crot",   p->clip_rot_deg);
    cJSON *col = cJSON_AddObjectToObject(pp, "color");
    cJSON_AddNumberToObject(col, "r", p->col_r[p->scene]);
    cJSON_AddNumberToObject(col, "g", p->col_g[p->scene]);
    cJSON_AddNumberToObject(col, "b", p->col_b[p->scene]);

    return send_json(req, o);
}

/* Optional "target" field: which board a command is for. NULL/absent = the
 * board serving this page (self); "all" = every eye; else a peer's id. */
static const char *body_target(cJSON *b)
{
    cJSON *v = cJSON_GetObjectItem(b, "target");
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

/* Route cmd to target and turn the result into an HTTP response. */
static esp_err_t dispatch_reply(httpd_req_t *req, const char *target,
                                const eye_cmd_t *cmd)
{
    if (eye_sync_dispatch(target, cmd) == EYE_ROUTE_UNKNOWN) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "target offline");
    }
    return send_ok(req);
}

static esp_err_t h_scene(httpd_req_t *req)
{
    eye_net_note_activity();
    cJSON *b = read_body(req);
    if (!b) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
    }
    int sc = -1;
    cJSON *name = cJSON_GetObjectItem(b, "name");
    if (cJSON_IsString(name)) {
        sc = eye_scene_from_str(name->valuestring);
    } else {
        sc = body_int(b, "id", -1);
    }
    if (sc < 0 || sc >= EYE_SCENE_COUNT) {
        cJSON_Delete(b);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown scene");
    }
    eye_cmd_t c = { .type = EYE_CMD_SCENE, .a = sc };
    esp_err_t r = dispatch_reply(req, body_target(b), &c);
    cJSON_Delete(b);
    return r;
}

static esp_err_t h_screen(httpd_req_t *req)
{
    eye_net_note_activity();
    cJSON *b = read_body(req);
    if (!b) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
    }
    eye_cmd_t c = { .type = EYE_CMD_SCREEN, .a = body_int(b, "index", 0) };
    esp_err_t r = dispatch_reply(req, body_target(b), &c);
    cJSON_Delete(b);
    return r;
}

static esp_err_t h_step(httpd_req_t *req)
{
    eye_net_note_activity();
    cJSON *b = read_body(req);
    eye_cmd_t c = { .type = EYE_CMD_STEP, .a = b ? body_int(b, "delta", 1) : 1 };
    esp_err_t r = dispatch_reply(req, b ? body_target(b) : NULL, &c);
    cJSON_Delete(b);
    return r;
}

/* Restart all eyes' current clip to frame 0 simultaneously. Always targets
 * every eye in the mesh (target is ignored; sync is inherently a broadcast). */
static esp_err_t h_sync(httpd_req_t *req)
{
    eye_net_note_activity();
    cJSON *b = read_body(req);
    eye_cmd_t c = { .type = EYE_CMD_SYNC };
    esp_err_t r = dispatch_reply(req, "all", &c);
    cJSON_Delete(b);
    return r;
}

/* Pause / resume the target eye. {paused:true|false} sets explicitly;
 * omitting the field toggles. */
static esp_err_t h_pause(httpd_req_t *req)
{
    eye_net_note_activity();
    cJSON *b = read_body(req);
    int val;
    cJSON *pv = b ? cJSON_GetObjectItem(b, "paused") : NULL;
    if (cJSON_IsBool(pv)) {
        val = cJSON_IsTrue(pv) ? 1 : 0;
    } else {
        val = eye_video_is_paused() ? 0 : 1;   /* toggle */
    }
    eye_cmd_t c = { .type = EYE_CMD_PAUSE, .a = val };
    esp_err_t r = dispatch_reply(req, b ? body_target(b) : NULL, &c);
    cJSON_Delete(b);
    return r;
}

/* Clip control: {action:"play"|"stop"} or {name:"foo.eyv"} (selection is by
 * name so it works across boards with different playlists). */
static esp_err_t h_clip(httpd_req_t *req)
{
    eye_net_note_activity();
    cJSON *b = read_body(req);
    if (!b) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
    }
    eye_cmd_t c = { 0 };
    cJSON *act  = cJSON_GetObjectItem(b, "action");
    cJSON *name = cJSON_GetObjectItem(b, "name");
    if (cJSON_IsString(act)) {
        c.type = EYE_CMD_CLIPACT;
        strncpy(c.key, act->valuestring, sizeof(c.key) - 1);
    } else if (cJSON_IsString(name)) {
        c.type = EYE_CMD_CLIP;
        strncpy(c.name, name->valuestring, sizeof(c.name) - 1);
    } else {
        cJSON_Delete(b);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "need action or name");
    }
    esp_err_t r = dispatch_reply(req, body_target(b), &c);
    cJSON_Delete(b);
    return r;
}

static esp_err_t h_param(httpd_req_t *req)
{
    eye_net_note_activity();
    cJSON *b = read_body(req);
    cJSON *key = b ? cJSON_GetObjectItem(b, "key") : NULL;
    if (!cJSON_IsString(key)) {
        cJSON_Delete(b);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
    }
    eye_cmd_t c = { .type = EYE_CMD_PARAM, .a = body_int(b, "value", 0) };
    strncpy(c.key, key->valuestring, sizeof(c.key) - 1);
    esp_err_t r = dispatch_reply(req, body_target(b), &c);
    cJSON_Delete(b);
    return r;
}

static esp_err_t h_color(httpd_req_t *req)
{
    eye_net_note_activity();
    cJSON *b = read_body(req);
    if (!b) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
    }
    eye_cmd_t c = { .type = EYE_CMD_COLOR,
                    .a = body_int(b, "r", 255),
                    .b = body_int(b, "g", 0),
                    .c = body_int(b, "b", 0) };
    esp_err_t r = dispatch_reply(req, body_target(b), &c);
    cJSON_Delete(b);
    return r;
}

static esp_err_t h_flip(httpd_req_t *req)
{
    eye_net_note_activity();
    cJSON *b = read_body(req);
    if (!b) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
    }
    eye_params_t *p = eye_demo_params();
    eye_cmd_t c = { .type = EYE_CMD_FLIP,
                    .a = body_int(b, "h", p->flip_h) ? 1 : 0,
                    .b = body_int(b, "v", p->flip_v) ? 1 : 0 };
    esp_err_t r = dispatch_reply(req, body_target(b), &c);
    cJSON_Delete(b);
    return r;
}

static esp_err_t h_brightness(httpd_req_t *req)
{
    eye_net_note_activity();
    cJSON *b = read_body(req);
    if (!b) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
    }
    eye_cmd_t c = { .type = EYE_CMD_BRIGHT, .a = body_int(b, "pct", 80) };
    esp_err_t r = dispatch_reply(req, body_target(b), &c);
    cJSON_Delete(b);
    return r;
}

static esp_err_t h_flick(httpd_req_t *req)
{
    eye_net_note_activity();
    cJSON *b = read_body(req);
    if (!b) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
    }
    eye_cmd_t c = { .type = EYE_CMD_FLICK, .a = body_int(b, "on", 1) != 0 };
    esp_err_t r = dispatch_reply(req, body_target(b), &c);
    cJSON_Delete(b);
    return r;
}

/* Startup clip: {name:"foo.eyv"} to set, {name:""} to clear (boot on info). */
static esp_err_t h_startup(httpd_req_t *req)
{
    eye_net_note_activity();
    cJSON *b = read_body(req);
    cJSON *name = b ? cJSON_GetObjectItem(b, "name") : NULL;
    eye_cmd_t c = { .type = EYE_CMD_STARTUP };
    if (cJSON_IsString(name)) {
        strncpy(c.name, name->valuestring, sizeof(c.name) - 1);
    }
    esp_err_t r = dispatch_reply(req, b ? body_target(b) : NULL, &c);
    cJSON_Delete(b);
    return r;
}

static esp_err_t h_persist(httpd_req_t *req)
{
    eye_net_note_activity();
    cJSON *b = read_body(req);
    cJSON *act = b ? cJSON_GetObjectItem(b, "action") : NULL;
    if (!cJSON_IsString(act)) {
        cJSON_Delete(b);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
    }
    eye_cmd_t c = { .type = EYE_CMD_PERSIST };
    strncpy(c.key, act->valuestring, sizeof(c.key) - 1);
    esp_err_t r = dispatch_reply(req, body_target(b), &c);
    cJSON_Delete(b);
    return r;
}

/* Roster of every eye in the mesh (self first), for the board selector + the
 * per-board views. Self is live; peers come from cached ESP-NOW announces. */
static void add_board_json(cJSON *arr, const eye_board_t *bd)
{
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "id", bd->id);
    cJSON_AddStringToObject(e, "name", bd->name);
    cJSON_AddStringToObject(e, "board", bd->board);
    cJSON_AddStringToObject(e, "ip", bd->ip);
    cJSON_AddBoolToObject(e, "self", bd->is_self);
    cJSON_AddBoolToObject(e, "online", bd->online);
    cJSON_AddNumberToObject(e, "screen", bd->screen);
    cJSON_AddNumberToObject(e, "screens", bd->screens);
    cJSON_AddNumberToObject(e, "first_clip", bd->first_clip);
    cJSON_AddNumberToObject(e, "scene", bd->scene);
    cJSON_AddNumberToObject(e, "brightness", bd->brightness);
    cJSON_AddNumberToObject(e, "speed", bd->speed);
    cJSON_AddNumberToObject(e, "scale", bd->scale);
    cJSON_AddNumberToObject(e, "fliph", bd->flip_h);
    cJSON_AddNumberToObject(e, "flipv", bd->flip_v);
    cJSON_AddBoolToObject(e, "flick", bd->flick);
    cJSON_AddNumberToObject(e, "sspin", bd->spin_ms);
    cJSON_AddNumberToObject(e, "pupil", bd->pupil_px);
    cJSON_AddNumberToObject(e, "blink", bd->blink_ms);
    cJSON_AddNumberToObject(e, "gaze", bd->gaze_px);
    cJSON_AddNumberToObject(e, "flare", bd->flare_ms);
    cJSON_AddNumberToObject(e, "fhi", bd->flick_hi);
    cJSON_AddNumberToObject(e, "coff_x", bd->clip_off_x);
    cJSON_AddNumberToObject(e, "coff_y", bd->clip_off_y);
    cJSON_AddNumberToObject(e, "crot",   bd->clip_rot_deg);
    cJSON_AddNumberToObject(e, "playing_kind", bd->playing_kind);
    cJSON_AddStringToObject(e, "playing", bd->playing);
    cJSON_AddStringToObject(e, "startup", bd->startup);
    cJSON_AddNumberToObject(e, "uptime_s", bd->uptime_s);
    cJSON *col = cJSON_AddObjectToObject(e, "color");
    cJSON_AddNumberToObject(col, "r", bd->col_r);
    cJSON_AddNumberToObject(col, "g", bd->col_g);
    cJSON_AddNumberToObject(col, "b", bd->col_b);
    cJSON *clips = cJSON_AddArrayToObject(e, "clips");
    for (int i = 0; i < bd->clip_count && i < EYE_SYNC_MAX_CLIPS; i++) {
        cJSON_AddItemToArray(clips, cJSON_CreateString(bd->clips[i]));
    }
    cJSON_AddItemToArray(arr, e);
}

static esp_err_t h_mesh(httpd_req_t *req)
{
    eye_net_note_activity();
    eye_board_t *r = calloc(EYE_SYNC_MAX_BOARDS, sizeof(eye_board_t));
    if (r == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    int n = eye_sync_roster(r, EYE_SYNC_MAX_BOARDS);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "self", eye_sync_self_id());
    cJSON *arr = cJSON_AddArrayToObject(o, "boards");
    for (int i = 0; i < n; i++) {
        add_board_json(arr, &r[i]);
    }
    free(r);
    return send_json(req, o);
}

static esp_err_t h_upload(httpd_req_t *req)
{
    eye_net_note_activity();

    /* name from the query string; keep only a safe basename ending in .eyv */
    char query[192] = { 0 }, name[128] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));
    if (httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK ||
        !clip_name_ok(name)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name");
    }
    if (req->content_len <= 0 || req->content_len > UPLOAD_MAX) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad size");
    }

    mkdir(eye_net_clip_dir(), 0775);   /* ok if it exists / is an FS root */
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", eye_net_clip_dir(), name);
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        int e = errno;
        ESP_LOGE(TAG, "fopen(%s) failed: errno %d (%s)", path, e, strerror(e));
        char msg[96];
        snprintf(msg, sizeof(msg), "fopen failed: errno %d (%s)", e, strerror(e));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, msg);
    }

    char *buf = malloc(4096);
    int remaining = req->content_len;
    int timeouts = 0, last_pct = -1;
    esp_err_t err = ESP_OK;
    if (s_upload_cb) {
        s_upload_cb(0);
    }
    while (remaining > 0 && buf) {
        int r = httpd_req_recv(req, buf, remaining > 4096 ? 4096 : remaining);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            /* WiFi hiccup (beacon loss / power save): keep waiting instead of
             * aborting -- this was the intermittent "write failed" upload. */
            if (++timeouts <= 6) {
                continue;
            }
            err = ESP_FAIL;
            break;
        }
        if (r <= 0 || fwrite(buf, 1, r, f) != (size_t)r) {
            err = ESP_FAIL;
            break;
        }
        timeouts = 0;
        remaining -= r;
        int pct = (int)(100 - (100LL * remaining) / req->content_len);
        if (s_upload_cb && pct != last_pct && (pct - last_pct >= 2 || pct == 100)) {
            last_pct = pct;
            s_upload_cb(pct);
        }
    }
    free(buf);
    fclose(f);
    if (s_upload_cb) {
        s_upload_cb(-1);   /* done (either way): hide the overlay */
    }
    if (err != ESP_OK || remaining > 0) {
        remove(path);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "upload interrupted (WiFi?) or storage full");
    }

    eye_video_rescan();
    eye_demo_refresh_clips();
    ESP_LOGI(TAG, "uploaded %s (%d bytes)", path, req->content_len);
    return send_ok(req);
}

static esp_err_t h_delete(httpd_req_t *req)
{
    eye_net_note_activity();
    cJSON *b = read_body(req);
    cJSON *name = b ? cJSON_GetObjectItem(b, "name") : NULL;
    if (!cJSON_IsString(name) || !clip_name_ok(name->valuestring)) {
        cJSON_Delete(b);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name");
    }
    char path[256];
    snprintf(path, sizeof(path), "%s/%.127s", eye_net_clip_dir(), name->valuestring);
    cJSON_Delete(b);

    if (remove(path) != 0) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such clip");
    }
    eye_video_rescan();
    eye_demo_refresh_clips();
    ESP_LOGI(TAG, "deleted %s", path);
    return send_ok(req);
}

static esp_err_t h_wifi_forget(httpd_req_t *req)
{
    eye_net_note_activity();
    eye_net_forget_wifi();
    return send_ok(req);
}

/* Scan for nearby networks (so the control-AP page can onboard to home WiFi
 * without the captive portal). Works in APSTA -- the SoftAP shares the radio. */
static esp_err_t h_wifi_scan(httpd_req_t *req)
{
    eye_net_note_activity();
    wifi_scan_config_t scan_cfg = { 0 };
    if (esp_wifi_scan_start(&scan_cfg, true) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "scan failed");
    }
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 20) {
        n = 20;
    }
    cJSON *arr = cJSON_CreateArray();
    if (n > 0) {
        wifi_ap_record_t *recs = calloc(n, sizeof(*recs));
        if (recs == NULL) {
            cJSON_Delete(arr);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        }
        esp_wifi_scan_get_ap_records(&n, recs);
        for (int i = 0; i < n; i++) {
            if (recs[i].ssid[0] == '\0') {
                continue;
            }
            bool dup = false;
            for (int j = 0; j < i && !dup; j++) {
                dup = strcmp((char *)recs[j].ssid, (char *)recs[i].ssid) == 0;
            }
            if (dup) {
                continue;
            }
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "s", (char *)recs[i].ssid);
            cJSON_AddNumberToObject(o, "r", recs[i].rssi);
            cJSON_AddItemToArray(arr, o);
        }
        free(recs);
    }
    return send_json(req, arr);
}

static esp_err_t h_wifi_connect(httpd_req_t *req)
{
    eye_net_note_activity();
    cJSON *b = read_body(req);
    cJSON *ssid = b ? cJSON_GetObjectItem(b, "ssid") : NULL;
    cJSON *pass = b ? cJSON_GetObjectItem(b, "password") : NULL;
    if (!cJSON_IsString(ssid) || ssid->valuestring[0] == '\0') {
        cJSON_Delete(b);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required");
    }
    esp_err_t r = eye_net_provision(ssid->valuestring,
                                    cJSON_IsString(pass) ? pass->valuestring : "");
    cJSON_Delete(b);
    if (r != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "save failed");
    }
    return send_ok(req);   /* the board reconnects a moment later */
}

static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(400));
    esp_restart();
}

static esp_err_t h_ota(httpd_req_t *req)
{
    eye_net_note_activity();

    if (req->content_len <= 0 || req->content_len > OTA_UPLOAD_MAX) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad size");
    }

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (part == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "no OTA partition (partition table wrong?)");
    }

    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "ota_begin failed");
    }

    char *buf = malloc(4096);
    if (buf == NULL) {
        esp_ota_abort(ota_handle);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }

    int remaining = req->content_len;
    int timeouts = 0, last_pct = -1;
    bool failed = false;
    if (s_ota_cb) s_ota_cb(0);
    while (remaining > 0) {
        int chunk = remaining > 4096 ? 4096 : remaining;
        int r = httpd_req_recv(req, buf, chunk);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts <= 6) {
                continue;
            }
            failed = true;
            break;
        }
        if (r <= 0) {
            failed = true;
            break;
        }
        timeouts = 0;
        err = esp_ota_write(ota_handle, buf, r);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(err));
            failed = true;
            break;
        }
        remaining -= r;
        int pct = (int)(100 - (100LL * remaining) / req->content_len);
        if (s_ota_cb && pct != last_pct && (pct - last_pct >= 2 || pct == 100)) {
            last_pct = pct;
            s_ota_cb(pct);
        }
    }
    free(buf);

    if (failed) {
        if (s_ota_cb) s_ota_cb(-1);
        esp_ota_abort(ota_handle);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "upload interrupted or write error");
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "ota_end failed (image invalid?)");
    }

    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition: %s", esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "set_boot_partition failed");
    }

    ESP_LOGI(TAG, "OTA flash ok (%d bytes), rebooting", req->content_len);
    if (s_ota_cb) s_ota_cb(-1);   /* hide overlay; device reboots in 400 ms */
    send_ok(req);
    xTaskCreate(reboot_task, "ota_reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

/* GET /api/clip_param?name=foo.eyv
 * Returns {"name":"...","off_x":0,"off_y":0,"rot_deg":0,"scale_pct":0,
 *           "has_override":false} */
static esp_err_t h_clip_param_get(httpd_req_t *req)
{
    eye_net_note_activity();
    char query[192] = { 0 }, name[128] = { 0 };
    httpd_req_get_url_query_str(req, query, sizeof(query));
    httpd_query_key_value(query, "name", name, sizeof(name));

    eye_clip_override_t ov = { 0 };
    bool has = name[0] ? eye_clip_override_load(name, &ov) : false;

    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "name",      name);
    cJSON_AddNumberToObject(o, "off_x",     has ? ov.off_x     : 0);
    cJSON_AddNumberToObject(o, "off_y",     has ? ov.off_y     : 0);
    cJSON_AddNumberToObject(o, "rot_deg",   has ? ov.rot_deg   : 0);
    cJSON_AddNumberToObject(o, "scale_pct", has ? ov.scale_pct : 0);
    cJSON_AddNumberToObject(o, "flip_h",    has ? ov.flip_h    : 0);
    cJSON_AddNumberToObject(o, "flip_v",    has ? ov.flip_v    : 0);
    cJSON_AddBoolToObject(o, "has_override", has);
    return send_json(req, o);
}

/* POST /api/clip_param  {"name":"foo.eyv","off_x":10,"off_y":-5,"rot_deg":5,"scale_pct":0}
 * Use scale_pct=0 to keep global scale.
 * Add "clear":true to delete the per-clip override entirely. */
static esp_err_t h_clip_param_post(httpd_req_t *req)
{
    eye_net_note_activity();
    cJSON *b = read_body(req);
    cJSON *name_j = b ? cJSON_GetObjectItem(b, "name") : NULL;
    if (!cJSON_IsString(name_j) || !clip_name_ok(name_j->valuestring)) {
        cJSON_Delete(b);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name");
    }
    const char *name = name_j->valuestring;

    cJSON *clear_j = cJSON_GetObjectItem(b, "clear");
    if (cJSON_IsTrue(clear_j)) {
        eye_clip_override_clear(name);
        eye_demo_reload_clip_override();
        cJSON_Delete(b);
        return send_ok(req);
    }

    eye_clip_override_t ov = {
        .off_x     = (int16_t)body_int(b, "off_x",     0),
        .off_y     = (int16_t)body_int(b, "off_y",     0),
        .rot_deg   = (int16_t)body_int(b, "rot_deg",   0),
        .scale_pct = (int16_t)body_int(b, "scale_pct", 0),
        .flip_h    = (int8_t)(body_int(b, "flip_h", 0) ? 1 : 0),
        .flip_v    = (int8_t)(body_int(b, "flip_v", 0) ? 1 : 0),
    };
    /* clamp */
    if (ov.off_x   < -200) ov.off_x   = -200;
    if (ov.off_x   >  200) ov.off_x   =  200;
    if (ov.off_y   < -200) ov.off_y   = -200;
    if (ov.off_y   >  200) ov.off_y   =  200;
    if (ov.rot_deg <    0) ov.rot_deg =    0;
    if (ov.rot_deg >  359) ov.rot_deg =  359;
    if (ov.scale_pct < 0 || ov.scale_pct > 130) ov.scale_pct = 0;

    esp_err_t err = eye_clip_override_save(name, &ov);
    if (err == ESP_OK) eye_demo_reload_clip_override();
    cJSON_Delete(b);
    return err == ESP_OK ? send_ok(req)
         : httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs write failed");
}

/* GET /dev/log  -- plain-text snapshot of the log ring (for the console page) */
static esp_err_t h_dev_log(httpd_req_t *req)
{
    char *buf = malloc(LOG_RING_CAP + 1);
    if (!buf) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    int len = log_ring_snapshot(buf, LOG_RING_CAP + 1);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t r = httpd_resp_send(req, buf, len);
    free(buf);
    return r;
}

/* GET /dev/console  -- mini terminal UI (direct URL only, not in the main nav) */
static esp_err_t h_dev_console(httpd_req_t *req)
{
    static const char page[] =
        "<!DOCTYPE html><html><head>"
        "<meta charset='utf-8'>"
        "<title>Console \xe2\x80\x94 SharinganEye</title>"
        "<style>"
        "* { box-sizing: border-box; margin: 0; padding: 0; }"
        "body { background: #0d0d0d; color: #c0ffc0; font: 12px/1.4 'Courier New', monospace; height: 100vh; display: flex; flex-direction: column; }"
        "#hdr { background: #1a1a1a; border-bottom: 1px solid #2a2a2a; padding: 8px 14px; display: flex; align-items: center; gap: 10px; flex-shrink: 0; }"
        "#hdr h3 { color: #eee; font-size: 14px; font-weight: normal; }"
        "#dot { width: 9px; height: 9px; border-radius: 50%; background: #0f0; flex-shrink: 0; }"
        "#ts { color: #555; font-size: 11px; margin-left: auto; }"
        "#log { flex: 1; overflow-y: auto; padding: 10px 14px; white-space: pre-wrap; word-break: break-all; }"
        "</style>"
        "</head><body>"
        "<div id='hdr'><div id='dot'></div><h3>SharinganEye &mdash; Serial Console</h3><span id='ts'></span></div>"
        "<div id='log'>connecting\xe2\x80\xa6</div>"
        "<script>"
        "var log=document.getElementById('log');"
        "var dot=document.getElementById('dot');"
        "var ts=document.getElementById('ts');"
        "var stick=true;"
        "log.addEventListener('scroll',function(){"
        "  stick=log.scrollTop+log.clientHeight>=log.scrollHeight-30;"
        "});"
        "function poll(){"
        "  fetch('/dev/log').then(function(r){return r.text();}).then(function(t){"
        "    log.textContent=t;"
        "    if(stick)log.scrollTop=log.scrollHeight;"
        "    dot.style.background='#0f0';"
        "    ts.textContent=new Date().toLocaleTimeString();"
        "  }).catch(function(){dot.style.background='#f44';});"
        "}"
        "poll(); setInterval(poll,1500);"
        "</script>"
        "</body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t h_reboot(httpd_req_t *req)
{
    send_ok(req);
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

/* --------------------------------------------------------------- start -- */

esp_err_t eye_webserver_start(void)
{
    if (s_httpd != NULL) {
        return ESP_OK;
    }

    if (!s_mdns_up && mdns_init() == ESP_OK) {
        mdns_hostname_set("sharingan-eye");
        char inst[32];
        snprintf(inst, sizeof(inst), "SharinganEye-%s", eye_sync_self_id());
        mdns_instance_name_set(inst);
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
        s_mdns_up = true;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;         /* status JSON + upload handler headroom */
    cfg.max_uri_handlers = 31;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 12;    /* ride out WiFi power-save stalls        */
    cfg.send_wait_timeout = 12;
    esp_err_t err = httpd_start(&s_httpd, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t uris[] = {
        { .uri = "/",                .method = HTTP_GET,  .handler = h_index },
        { .uri = "/api/status",      .method = HTTP_GET,  .handler = h_status },
        { .uri = "/api/mesh",        .method = HTTP_GET,  .handler = h_mesh },
        { .uri = "/api/startup",     .method = HTTP_POST, .handler = h_startup },
        { .uri = "/api/scene",       .method = HTTP_POST, .handler = h_scene },
        { .uri = "/api/screen",      .method = HTTP_POST, .handler = h_screen },
        { .uri = "/api/step",        .method = HTTP_POST, .handler = h_step },
        { .uri = "/api/sync",        .method = HTTP_POST, .handler = h_sync },
        { .uri = "/api/pause",       .method = HTTP_POST, .handler = h_pause },
        { .uri = "/api/clip",        .method = HTTP_POST, .handler = h_clip },
        { .uri = "/api/param",       .method = HTTP_POST, .handler = h_param },
        { .uri = "/api/color",       .method = HTTP_POST, .handler = h_color },
        { .uri = "/api/flip",        .method = HTTP_POST, .handler = h_flip },
        { .uri = "/api/brightness",  .method = HTTP_POST, .handler = h_brightness },
        { .uri = "/api/flick",       .method = HTTP_POST, .handler = h_flick },
        { .uri = "/api/persist",     .method = HTTP_POST, .handler = h_persist },
        { .uri = "/api/upload",      .method = HTTP_POST, .handler = h_upload },
        { .uri = "/api/delete",      .method = HTTP_POST, .handler = h_delete },
        { .uri = "/api/wifi/forget", .method = HTTP_POST, .handler = h_wifi_forget },
        { .uri = "/api/wifi/scan",   .method = HTTP_GET,  .handler = h_wifi_scan },
        { .uri = "/api/wifi/connect",.method = HTTP_POST, .handler = h_wifi_connect },
        { .uri = "/api/reboot",      .method = HTTP_POST, .handler = h_reboot },
        { .uri = "/api/ota",         .method = HTTP_POST, .handler = h_ota },
        { .uri = "/api/clip_param",  .method = HTTP_GET,  .handler = h_clip_param_get },
        { .uri = "/api/clip_param",  .method = HTTP_POST, .handler = h_clip_param_post },
        { .uri = "/dev/console",     .method = HTTP_GET,  .handler = h_dev_console },
        { .uri = "/dev/log",         .method = HTTP_GET,  .handler = h_dev_log },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_httpd, &uris[i]);
    }

    ESP_LOGI(TAG, "web UI up: http://sharingan-eye.local");
    return ESP_OK;
}

void eye_webserver_stop(void)
{
    if (s_httpd != NULL) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    if (s_mdns_up) {
        mdns_free();
        s_mdns_up = false;
    }
}
