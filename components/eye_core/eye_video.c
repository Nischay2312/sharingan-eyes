#include "eye_video.h"
#include "eye_config.h"

#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_partition.h"
#include "esp_jpeg_dec.h"

static const char *TAG = "eye_video";

#define MAX_CLIPS  24
#define PATH_MAX_  256

/* Per-clip container header (one .eyv). Little-endian. Frame offsets in the
 * index are relative to the START of the clip, so a clip can sit anywhere. */
typedef struct __attribute__((packed)) {
    char     magic[4];      /* "EYEV" */
    uint16_t version;
    uint16_t format;        /* 0 = MJPEG, 1 = raw RGB565_LE */
    uint16_t width;
    uint16_t height;
    uint32_t frame_count;
    uint16_t frame_ms;
    uint8_t  loop;
    uint8_t  reserved[13];
} eyv_header_t;

typedef struct __attribute__((packed)) {
    uint32_t offset;        /* from clip start */
    uint32_t size;
} eyv_frame_t;

/* Multi-clip pack header (one .eyp = N concatenated .eyv with a directory). */
typedef struct __attribute__((packed)) {
    char     magic[4];      /* "EYEP" */
    uint16_t version;
    uint16_t clip_count;
    uint8_t  reserved[8];
} eyp_header_t;

typedef struct __attribute__((packed)) {
    uint32_t offset;        /* clip start, from pack file start */
    uint32_t size;
} eyp_entry_t;

#define EYV_FMT_MJPEG  0
#define EYV_FMT_RAW    1

typedef enum { SRC_PART, SRC_DIR, SRC_FILE } src_mode_t;

static src_mode_t s_mode = SRC_PART;
static char       s_arg[PATH_MAX_];

/* Playlist */
static int      s_clip_count;
static int      s_cur;
static uint32_t s_clip_base[MAX_CLIPS];               /* partition: clip offsets */
static char     s_clip_path[MAX_CLIPS][PATH_MAX_];    /* dir/file: clip paths    */

/* Source + current clip */
static const esp_partition_t *s_part;
static FILE                  *s_fp;        /* current clip file (dir/file mode) */
static uint32_t               s_base;      /* current clip base (partition mode) */
static eyv_header_t           s_hdr;
static eyv_frame_t           *s_index;
static uint8_t               *s_scratch;
static size_t                 s_scratch_cap;
static uint32_t               s_pos;       /* current frame within the clip */
static bool                   s_ok;
static bool                   s_paused;
static SemaphoreHandle_t      s_lock;

void eye_video_set_partition(const char *label)
{
    s_mode = SRC_PART;
    snprintf(s_arg, sizeof(s_arg), "%s", label ? label : "anim");
}
void eye_video_set_dir(const char *dir)
{
    s_mode = SRC_DIR;
    snprintf(s_arg, sizeof(s_arg), "%s", dir ? dir : "");
}
void eye_video_set_file(const char *path)
{
    s_mode = SRC_FILE;
    snprintf(s_arg, sizeof(s_arg), "%s", path ? path : "");
}

/* Read len bytes from the current clip at clip-relative offset off. */
static esp_err_t clip_read(uint32_t off, void *dst, size_t len)
{
    if (s_fp) {
        if (fseek(s_fp, (long)(s_base + off), SEEK_SET) != 0) {
            return ESP_FAIL;
        }
        return fread(dst, 1, len, s_fp) == len ? ESP_OK : ESP_FAIL;
    }
    return esp_partition_read(s_part, s_base + off, dst, len);
}

/* (Re)load clip i: open it, validate header, read its frame index. */
static esp_err_t load_clip(int i)
{
    s_ok = false;
    if (s_fp) {
        fclose(s_fp);
        s_fp = NULL;
    }

    if (s_mode == SRC_PART) {
        s_base = s_clip_base[i];
    } else {
        s_fp = fopen(s_clip_path[i], "rb");
        if (s_fp == NULL) {
            ESP_LOGE(TAG, "open %s failed", s_clip_path[i]);
            return ESP_FAIL;
        }
        s_base = 0;
    }

    if (clip_read(0, &s_hdr, sizeof(s_hdr)) != ESP_OK ||
        memcmp(s_hdr.magic, "EYEV", 4) != 0 ||
        s_hdr.width != EYE_FB_W || s_hdr.height != EYE_FB_H || s_hdr.frame_count == 0) {
        ESP_LOGE(TAG, "clip %d invalid header", i);
        return ESP_FAIL;
    }

    free(s_index);
    s_index = NULL;
    size_t ib = (size_t)s_hdr.frame_count * sizeof(eyv_frame_t);
    s_index = heap_caps_malloc(ib, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_index == NULL) {
        s_index = malloc(ib);
    }
    if (s_index == NULL || clip_read(sizeof(s_hdr), s_index, ib) != ESP_OK) {
        return ESP_FAIL;
    }

    if (s_hdr.format == EYV_FMT_MJPEG) {
        size_t maxsz = 0;
        for (uint32_t f = 0; f < s_hdr.frame_count; f++) {
            if (s_index[f].size > maxsz) {
                maxsz = s_index[f].size;
            }
        }
        if (maxsz > s_scratch_cap) {
            free(s_scratch);
            s_scratch = heap_caps_malloc(maxsz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (s_scratch == NULL) {
                s_scratch = malloc(maxsz);
            }
            if (s_scratch == NULL) {
                s_scratch_cap = 0;
                return ESP_ERR_NO_MEM;
            }
            s_scratch_cap = maxsz;
        }
    }

    s_cur = i;
    s_pos = 0;
    s_ok = true;
    ESP_LOGI(TAG, "clip %d/%d: %u frames, %ums/frame (~%u fps), %s",
             i + 1, s_clip_count, (unsigned)s_hdr.frame_count, s_hdr.frame_ms,
             s_hdr.frame_ms ? (unsigned)(1000 / s_hdr.frame_ms) : 0,
             s_hdr.format == EYV_FMT_RAW ? "raw" : "mjpeg");
    return ESP_OK;
}

static int path_cmp(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

static void scan_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (d == NULL) {
        ESP_LOGW(TAG, "opendir %s failed", dir);
        return;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL && s_clip_count < MAX_CLIPS) {
        size_t L = strlen(e->d_name);
        if (L > 4 && strcasecmp(e->d_name + L - 4, ".eyv") == 0) {
            /* Bounded precision so the total is provably < PATH_MAX_ (silences
             * -Wformat-truncation): 200 + '/' + 54 = 255. */
            snprintf(s_clip_path[s_clip_count], PATH_MAX_, "%.200s/%.54s", dir, e->d_name);
            s_clip_count++;
        }
    }
    closedir(d);
    if (s_clip_count > 1) {
        qsort(s_clip_path, s_clip_count, PATH_MAX_, path_cmp);
    }
}

static esp_err_t build_playlist(void)
{
    s_clip_count = 0;

    if (s_mode == SRC_PART) {
        const char *label = s_arg[0] ? s_arg : "anim";
        s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                          ESP_PARTITION_SUBTYPE_ANY, label);
        if (s_part == NULL) {
            ESP_LOGI(TAG, "no '%s' partition; video disabled", label);
            return ESP_ERR_NOT_FOUND;
        }
        char magic[4] = { 0 };
        esp_partition_read(s_part, 0, magic, 4);
        if (memcmp(magic, "EYEP", 4) == 0) {
            eyp_header_t ph = { 0 };
            esp_partition_read(s_part, 0, &ph, sizeof(ph));
            int n = ph.clip_count > MAX_CLIPS ? MAX_CLIPS : ph.clip_count;
            for (int i = 0; i < n; i++) {
                eyp_entry_t ent = { 0 };
                esp_partition_read(s_part, sizeof(ph) + i * sizeof(ent), &ent, sizeof(ent));
                s_clip_base[i] = ent.offset;
            }
            s_clip_count = n;
        } else if (memcmp(magic, "EYEV", 4) == 0) {
            s_clip_base[0] = 0;
            s_clip_count = 1;
        } else {
            ESP_LOGW(TAG, "anim partition has no clip (flash a .eyv or .eyp)");
            return ESP_ERR_INVALID_STATE;
        }
    } else if (s_mode == SRC_DIR) {
        scan_dir(s_arg);
    } else { /* SRC_FILE */
        snprintf(s_clip_path[0], PATH_MAX_, "%s", s_arg);
        s_clip_count = 1;
    }

    if (s_clip_count == 0) {
        ESP_LOGI(TAG, "no clips found; video disabled");
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

esp_err_t eye_video_rescan(void)
{
    if (s_lock == NULL) {
        return eye_video_init();
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_ok = false;
    esp_err_t r = build_playlist();
    if (r == ESP_OK) {
        r = load_clip(s_cur < s_clip_count ? s_cur : 0);
    }
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "rescan: %d clip(s)", s_clip_count);
    return r;
}

esp_err_t eye_video_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    s_ok = false;
    if (build_playlist() != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t r = load_clip(0);
    if (r == ESP_OK) {
        ESP_LOGI(TAG, "playlist: %d clip(s)", s_clip_count);
    }
    return r;
}

static esp_err_t decode_into(uint32_t idx, uint16_t *fb)
{
    if (idx >= s_hdr.frame_count) {
        idx = 0;
    }
    uint32_t off = s_index[idx].offset;
    uint32_t sz  = s_index[idx].size;

    if (s_hdr.format == EYV_FMT_RAW) {
        if (sz != (uint32_t)EYE_FB_PX * 2) {
            return ESP_FAIL;
        }
        return clip_read(off, fb, sz);
    }

    if (sz > s_scratch_cap || clip_read(off, s_scratch, sz) != ESP_OK) {
        return ESP_FAIL;
    }

    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    jpeg_dec_handle_t dec = NULL;
    if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK) {
        return ESP_FAIL;
    }
    jpeg_dec_io_t io = { .inbuf = s_scratch, .inbuf_len = (int)sz };
    jpeg_dec_header_info_t hdr = { 0 };
    esp_err_t ret = ESP_FAIL;
    if (jpeg_dec_parse_header(dec, &io, &hdr) == JPEG_ERR_OK) {
        int out_len = 0;
        jpeg_dec_get_outbuf_len(dec, &out_len);
        if (out_len == (int)(EYE_FB_PX * 2)) {
            io.outbuf = (uint8_t *)fb;
            if (jpeg_dec_process(dec, &io) == JPEG_ERR_OK) {
                ret = ESP_OK;
            }
        }
    }
    jpeg_dec_close(dec);
    return ret;
}

esp_err_t eye_video_next_frame(uint16_t *fb240)
{
    if (!s_ok || fb240 == NULL) {
        return ESP_FAIL;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t r = decode_into(s_pos, fb240);
    if (!s_paused) {
        if (++s_pos >= s_hdr.frame_count) {
            s_pos = s_hdr.loop ? 0 : (s_hdr.frame_count ? s_hdr.frame_count - 1 : 0);
        }
    }
    xSemaphoreGive(s_lock);
    return r;
}

void eye_video_restart(void)
{
    if (!s_ok) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_pos = 0;
    xSemaphoreGive(s_lock);
}

void eye_video_pause(bool on)  { s_paused = on; }
bool eye_video_is_paused(void) { return s_paused; }

void eye_video_step(int delta)
{
    if (s_clip_count <= 1) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int i = ((s_cur + delta) % s_clip_count + s_clip_count) % s_clip_count;
    load_clip(i);
    xSemaphoreGive(s_lock);
}

void eye_video_select(int index)
{
    if (!s_ok || index < 0 || index >= s_clip_count) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (index != s_cur) {
        load_clip(index);
    } else {
        s_pos = 0;        /* re-entering the same clip restarts it */
    }
    xSemaphoreGive(s_lock);
}

const char *eye_video_clip_name(int index)
{
    static char s_name[128];
    if (index < 0 || index >= s_clip_count) {
        return "?";
    }
    if (s_mode == SRC_PART) {
        snprintf(s_name, sizeof(s_name), "clip %d", index + 1);
        return s_name;
    }
    /* dir/file mode: basename of the clip path */
    const char *p = s_clip_path[index];
    const char *b = strrchr(p, '/');
    snprintf(s_name, sizeof(s_name), "%s", b ? b + 1 : p);
    return s_name;
}

int eye_video_index_for_name(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return -1;
    }
    for (int i = 0; i < s_clip_count; i++) {
        char buf[40];
        const char *n;
        if (s_mode == SRC_PART) {
            snprintf(buf, sizeof(buf), "clip %d", i + 1);
            n = buf;
        } else {
            const char *p = s_clip_path[i];
            const char *b = strrchr(p, '/');
            n = b ? b + 1 : p;
        }
        if (strcasecmp(n, name) == 0) {
            return i;
        }
    }
    return -1;
}

bool eye_video_available(void) { return s_ok; }
int  eye_video_count(void) { return s_clip_count; }
int  eye_video_current_index(void) { return s_cur; }
uint16_t eye_video_frame_ms(void) { return s_ok ? s_hdr.frame_ms : 0; }

void eye_video_print_info(void)
{
    if (!s_ok) {
        printf("no clip loaded (flash a .eyv/.eyp, or put .eyv on the SD card)\n");
        return;
    }
    printf("playlist: %d clip(s), playing %d/%d\n", s_clip_count, s_cur + 1, s_clip_count);
    if (s_mode != SRC_PART) {
        printf("  file: %s\n", s_clip_path[s_cur]);
    }
    printf("  %u frames, %ux%u, %u ms (~%u fps), %s, loop=%s\n",
           (unsigned)s_hdr.frame_count, s_hdr.width, s_hdr.height, s_hdr.frame_ms,
           s_hdr.frame_ms ? (unsigned)(1000 / s_hdr.frame_ms) : 0,
           s_hdr.format == EYV_FMT_RAW ? "raw" : "mjpeg",
           s_hdr.loop ? "yes" : "no");
}
