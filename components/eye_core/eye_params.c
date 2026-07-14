#include "eye_params.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "eye_params";

#define NVS_NS    "eye"
#define NVS_KEY   "params"
#define PARAMS_VERSION 5u   /* bump when eye_params_t layout changes */

static const char *k_scene_names[EYE_SCENE_COUNT] = {
    "tomoe1",
    "tomoe2",
    "tomoe3",
    "mangekyou",   /* Obito/Kakashi pinwheel */
    "itachi",
    "rinnegan",
    "tomoerin",    /* tomoe rinnegan */
    "byakugan",
};

void eye_params_init(eye_params_t *p)
{
    memset(p, 0, sizeof(*p));
    p->scene     = EYE_SCENE_TOMOE3;
    p->speed_pct = 100;
    p->scale_pct = 100;

    /* Primary color per scene: saturated values so lit areas hit the panel's
     * full brightness. Rinnegan violet matched to the reference art. */
    const uint8_t def[EYE_SCENE_COUNT][3] = {
        { 255,   0,   0 },  /* tomoe1                       */
        { 255,   0,   0 },  /* tomoe2                       */
        { 255,   0,   0 },  /* tomoe3                       */
        { 255,   0,   0 },  /* mangekyou (Obito)            */
        { 255,   0,   0 },  /* mangekyou (Itachi)           */
        { 186, 140, 235 },  /* rinnegan: pale ripple violet */
        { 172, 116, 224 },  /* tomoe rinnegan               */
        { 226, 228, 246 },  /* byakugan: bright pale white  */
    };
    for (int e = 0; e < EYE_SCENE_COUNT; e++) {
        p->col_r[e] = def[e][0];
        p->col_g[e] = def[e][1];
        p->col_b[e] = def[e][2];
    }

    p->spin_ms  = 5000;   /* slow, menacing idle spin */
    p->pupil_px = 15;
    p->blink_ms = 4200;   /* natural blink every ~2.5-7 s */
    p->gaze_px  = 10;     /* subtle saccades              */
    p->flare_ms = 11000;  /* occasional fast spin burst   */

    /* Deliberate wrist flick spikes to ~700-1500 deg/s; casual head/mask motion
     * stays well under. High trigger keeps it from firing on slight movement. */
    p->flick_hi_dps  = 650;
    p->flick_lo_dps  = 150;
    p->flick_enabled = true;

    p->clip_off_x   = 0;
    p->clip_off_y   = 0;
    p->clip_rot_deg = 0;
}

const char *eye_scene_name(int scene)
{
    if (scene < 0 || scene >= EYE_SCENE_COUNT) {
        return "?";
    }
    return k_scene_names[scene];
}

int eye_scene_from_str(const char *s)
{
    if (s == NULL || s[0] == '\0') {
        return -1;
    }
    /* Numeric index? */
    char *end = NULL;
    long idx = strtol(s, &end, 10);
    if (end != s && *end == '\0') {
        return (idx >= 0 && idx < EYE_SCENE_COUNT) ? (int)idx : -1;
    }
    /* Name (prefix match allowed). */
    for (int e = 0; e < EYE_SCENE_COUNT; e++) {
        if (strncmp(s, k_scene_names[e], strlen(s)) == 0) {
            return e;
        }
    }
    return -1;
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Key table: maps a console key to a field + clamp range. */
typedef struct {
    const char *key;
    size_t      offset;     /* offsetof(eye_params_t, field) */
    int         lo, hi;
    const char *help;
} param_entry_t;

#define P_ENTRY(k, field, lo, hi, help) \
    { k, offsetof(eye_params_t, field), lo, hi, help }

static const param_entry_t k_params[] = {
    P_ENTRY("speed", speed_pct,     10, 2000, "global speed %"),
    P_ENTRY("scale", scale_pct,     20,  130, "eye size % (100=fit)"),
    P_ENTRY("fliph", flip_h,         0,    1, "mirror horizontally"),
    P_ENTRY("flipv", flip_v,         0,    1, "mirror vertically"),
    P_ENTRY("sspin", spin_ms,   -60000, 60000, "iris spin ms/rev (sign=dir)"),
    P_ENTRY("pupil", pupil_px,       6,   40, "pupil radius px"),
    P_ENTRY("blink", blink_ms,       0, 60000, "avg ms between blinks (0=off)"),
    P_ENTRY("gaze",  gaze_px,        0,   24, "gaze wander px (0=still)"),
    P_ENTRY("flare", flare_ms,       0, 120000, "avg ms between spin flares (0=off)"),
    P_ENTRY("fhi",   flick_hi_dps,  50, 2000, "flick trigger deg/s"),
    P_ENTRY("flo",   flick_lo_dps,  10, 1000, "flick re-arm deg/s"),
    P_ENTRY("coff_x", clip_off_x, -200, 200, "GIF x offset from center, display px"),
    P_ENTRY("coff_y", clip_off_y, -200, 200, "GIF y offset from center, display px"),
    P_ENTRY("crot",   clip_rot_deg,   0, 359, "GIF rotation 0..359 degrees"),
};

bool eye_params_set(eye_params_t *p, const char *key, int value)
{
    for (size_t i = 0; i < sizeof(k_params) / sizeof(k_params[0]); i++) {
        if (strcmp(key, k_params[i].key) == 0) {
            int *field = (int *)((char *)p + k_params[i].offset);
            *field = clampi(value, k_params[i].lo, k_params[i].hi);
            return true;
        }
    }
    return false;
}

void eye_params_set_color(eye_params_t *p, uint8_t r, uint8_t g, uint8_t b)
{
    int e = p->scene;
    if (e < 0 || e >= EYE_SCENE_COUNT) {
        return;
    }
    p->col_r[e] = r;
    p->col_g[e] = g;
    p->col_b[e] = b;
}

void eye_params_print(const eye_params_t *p)
{
    printf("scene    = %d (%s)\n", p->scene, eye_scene_name(p->scene));
    printf("speed    = %d %%\n", p->speed_pct);
    printf("scale    = %d %%\n", p->scale_pct);
    printf("flip     = h:%d v:%d\n", p->flip_h, p->flip_v);
    printf("color    = %u %u %u  (current scene)\n",
           p->col_r[p->scene], p->col_g[p->scene], p->col_b[p->scene]);
    printf("-- eye --\n");
    printf("  sspin  = %d ms/rev\n", p->spin_ms);
    printf("  pupil  = %d px\n", p->pupil_px);
    printf("  blink  = %d ms (0=off)\n", p->blink_ms);
    printf("  gaze   = %d px (0=still)\n", p->gaze_px);
    printf("  flare  = %d ms (0=off)\n", p->flare_ms);
    printf("-- flick --\n");
    printf("  enabled= %s\n", p->flick_enabled ? "yes" : "no");
    printf("  fhi    = %d deg/s\n", p->flick_hi_dps);
    printf("  flo    = %d deg/s\n", p->flick_lo_dps);
    printf("-- clip transform --\n");
    printf("  coff_x = %d px\n", p->clip_off_x);
    printf("  coff_y = %d px\n", p->clip_off_y);
    printf("  crot   = %d deg\n", p->clip_rot_deg);
}

/* On flash we store a small versioned blob so old saves are ignored cleanly. */
typedef struct {
    uint32_t      version;
    eye_params_t  params;
} nvs_blob_t;

esp_err_t eye_params_save(const eye_params_t *p)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    nvs_blob_t blob = { .version = PARAMS_VERSION, .params = *p };
    err = nvs_set_blob(h, NVS_KEY, &blob, sizeof(blob));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    ESP_LOGI(TAG, "save: %s", esp_err_to_name(err));
    return err;
}

esp_err_t eye_params_load(eye_params_t *p)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    nvs_blob_t blob = { 0 };
    size_t len = sizeof(blob);
    err = nvs_get_blob(h, NVS_KEY, &blob, &len);
    nvs_close(h);
    if (err != ESP_OK || len != sizeof(blob) || blob.version != PARAMS_VERSION) {
        return ESP_ERR_NOT_FOUND;
    }
    *p = blob.params;
    ESP_LOGI(TAG, "loaded saved params from NVS");
    return ESP_OK;
}
