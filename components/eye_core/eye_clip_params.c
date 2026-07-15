#include "eye_clip_params.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG     = "eye_clip";
#define NVS_NS             "eyeclip"
#define CLIP_NVS_VERSION   2u

typedef struct __attribute__((packed)) {
    uint8_t  version;
    int16_t  off_x;
    int16_t  off_y;
    int16_t  rot_deg;
    int16_t  scale_pct;
    int8_t   flip_h;
    int8_t   flip_v;
} clip_nvs_t;

/* FNV-1a 32-bit hash: turns an arbitrarily long filename into a stable 32-bit
 * value so distinct names never collide on the (max 15-char) NVS key. */
static uint32_t fnv1a(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; ++s) {
        h ^= (uint8_t)*s;
        h *= 16777619u;
    }
    return h;
}

/* Derive a ≤15-char NVS key from a clip filename.
 * Strip directory prefix and .eyv extension, then key = up-to-6 readable
 * characters + 8-hex hash of the FULL name. Keying on just the first 15 chars
 * (the old scheme) collided whenever two clips shared a 15-char prefix, e.g.
 * "obito_skin_eye_sharinga..." and "obito_skin_eye_rinnegan..." -> same slot. */
static void make_key(const char *clip_name, char key[16])
{
    const char *b = strrchr(clip_name, '/');
    b = b ? b + 1 : clip_name;
    char tmp[64];
    strncpy(tmp, b, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    size_t L = strlen(tmp);
    if (L > 4 && strcasecmp(tmp + L - 4, ".eyv") == 0) {
        tmp[L - 4] = '\0';
    }
    uint32_t hash = fnv1a(tmp);
    char pre[7];
    size_t i = 0;
    for (const char *p = tmp; *p && i < 6; ++p) {
        char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9')) {
            pre[i++] = c;
        }
    }
    pre[i] = '\0';
    snprintf(key, 16, "%s%08lx", pre, (unsigned long)hash);
}

bool eye_clip_override_load(const char *clip_name, eye_clip_override_t *out)
{
    if (!clip_name || !out) return false;

    char key[16];
    make_key(clip_name, key);

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;

    clip_nvs_t blob = { 0 };
    size_t len = sizeof(blob);
    esp_err_t err = nvs_get_blob(h, key, &blob, &len);
    nvs_close(h);

    if (err != ESP_OK || len != sizeof(blob) || blob.version != CLIP_NVS_VERSION) {
        return false;
    }
    out->off_x     = blob.off_x;
    out->off_y     = blob.off_y;
    out->rot_deg   = blob.rot_deg;
    out->scale_pct = blob.scale_pct;
    out->flip_h    = blob.flip_h;
    out->flip_v    = blob.flip_v;
    return true;
}

esp_err_t eye_clip_override_save(const char *clip_name, const eye_clip_override_t *p)
{
    if (!clip_name || !p) return ESP_ERR_INVALID_ARG;

    char key[16];
    make_key(clip_name, key);

    clip_nvs_t blob = {
        .version   = CLIP_NVS_VERSION,
        .off_x     = p->off_x,
        .off_y     = p->off_y,
        .rot_deg   = p->rot_deg,
        .scale_pct = p->scale_pct,
        .flip_h    = p->flip_h,
        .flip_v    = p->flip_v,
    };

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(h, key, &blob, sizeof(blob));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "saved override for '%s' (key '%s'): off=%d,%d rot=%d scale=%d%% flip=%d,%d",
             clip_name, key, p->off_x, p->off_y, p->rot_deg, p->scale_pct, p->flip_h, p->flip_v);
    return err;
}

void eye_clip_override_clear(const char *clip_name)
{
    if (!clip_name) return;

    char key[16];
    make_key(clip_name, key);

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, key);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "cleared override for '%s'", clip_name);
}
