/*
 * Board-specific display backend for the Waveshare ESP32-S3-Touch-AMOLED-2.06.
 *
 * Reuses the published Waveshare BSP for all panel bring-up (CO5300 QSPI init,
 * LVGL port, backlight) -- nothing here pokes the controller directly. Frames
 * reach the glass the way the proven streamer path does: an LVGL image whose
 * descriptor points at an RGB565 buffer, refreshed under the BSP's LVGL mutex.
 *
 * PING-PONG BUFFERS
 * -----------------
 * Two output buffers (s_out[0] and s_out[1]) are kept in PSRAM. push_frame
 * writes into the back buffer (not the one LVGL is currently DMA-ing), then
 * swaps under the BSP lock. This prevents the "spi transmit (queue) color
 * failed" crash that occurs when the CPU overwrites a buffer mid-DMA-transfer
 * (same fix used in ScreenStreamWatch for the same panel).
 *
 * SCALING / LENS TEST
 * -------------------
 * The board-agnostic engine renders a 240x240 eye. This backend nearest-neighbour
 * upscales it to a runtime size and centers it. scale_pct = 100 makes the eye
 * diameter equal the panel WIDTH (~410 px ~ 33 mm), matching the future 1.28"
 * round board's active area so a lens air-gap calibrated now carries over.
 * scale_pct < 100 shrinks it; scale_pct > 100 overscans toward the panel height
 * (the circle fills more vertically, with the left/right edges clipped).
 */

#include "eye_display.h"
#include "eye_config.h"

#include <string.h>
#include <math.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"

static const char *TAG = "eye_disp_amoled";

/* Largest output square we ever build: the panel's longer side (fills height). */
#define EYE_MAX_OUT_DIM   BSP_LCD_V_RES   /* 502 */
#define EYE_SCALE_MIN_PCT 20
#define EYE_SCALE_MAX_PCT 130

static lv_display_t  *s_disp;
static lv_obj_t      *s_img;
static lv_image_dsc_t s_dsc;

/* Ping-pong: two buffers of up to EYE_MAX_OUT_DIM^2 RGB565.
 * s_front is the index LVGL currently holds a reference to.
 * push_frame writes into s_out[1-s_front], then swaps under the BSP lock. */
static uint16_t      *s_out[2];
static int            s_front;

static uint16_t       s_map[EYE_MAX_OUT_DIM];   /* nearest-neighbour dst->src */
static int            s_out_dim;                 /* current output square size */
static int            s_scale_pct = -1;          /* last applied scale         */

/* Clip display transform, set by eye_display_set_transform before push_frame. */
static int  s_off_x;
static int  s_off_y;
static int  s_rot_deg;
static int  s_last_rot = -1;   /* last angle for which cos/sin were computed  */

/* Fixed-point cos/sin for the custom rotation loop (pre-computed per angle). */
static int32_t s_rot_cos = 4096;  /* cos(angle) × 2^12 */
static int32_t s_rot_sin = 0;     /* sin(angle) × 2^12 */

/* Update the scale map and output dimensions. Geometry metadata only -- no
 * LVGL calls. The next push_frame commits the new geometry alongside the
 * next frame under the BSP lock. */
static void update_scale(int pct)
{
    if (pct < EYE_SCALE_MIN_PCT) pct = EYE_SCALE_MIN_PCT;
    if (pct > EYE_SCALE_MAX_PCT) pct = EYE_SCALE_MAX_PCT;

    int dim = BSP_LCD_H_RES * pct / 100;        /* 100% == panel width */
    if (dim < 40)               dim = 40;
    if (dim > EYE_MAX_OUT_DIM)  dim = EYE_MAX_OUT_DIM;

    s_out_dim   = dim;
    s_scale_pct = pct;

    for (int d = 0; d < dim; d++) {
        s_map[d] = (uint16_t)((d * EYE_FB_W) / dim);
    }
    ESP_LOGI(TAG, "scale %d%% -> eye %dpx", pct, dim);
}

esp_err_t eye_display_init(void)
{
    s_disp = bsp_display_start();
    if (s_disp == NULL) {
        ESP_LOGE(TAG, "bsp_display_start failed");
        return ESP_FAIL;
    }
    bsp_display_backlight_on();

    const size_t max_bytes = (size_t)EYE_MAX_OUT_DIM * EYE_MAX_OUT_DIM * sizeof(uint16_t);
    for (int i = 0; i < 2; i++) {
        s_out[i] = (uint16_t *)heap_caps_malloc(max_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_out[i] == NULL) {
            s_out[i] = (uint16_t *)heap_caps_malloc(max_bytes, MALLOC_CAP_8BIT);
        }
        if (s_out[i] == NULL) {
            ESP_LOGE(TAG, "ping-pong buffer %d alloc failed (%u bytes)", i, (unsigned)max_bytes);
            return ESP_ERR_NO_MEM;
        }
        memset(s_out[i], 0, max_bytes);
    }
    s_front = 0;

    bsp_display_lock(0);
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    memset(&s_dsc, 0, sizeof(s_dsc));
    s_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_dsc.header.cf    = LV_COLOR_FORMAT_RGB565;
    s_dsc.header.flags = LV_IMAGE_FLAGS_MODIFIABLE;

    s_img = lv_image_create(scr);
    lv_image_set_antialias(s_img, false);
    lv_display_set_antialiasing(s_disp, false);
    bsp_display_unlock();

    /* Apply initial scale -- sets s_out_dim, s_map, and s_scale_pct. */
    update_scale(100);

    /* Push a blank frame to populate s_dsc.data and wire up the LVGL image. */
    eye_display_push_frame(NULL, 100);

    ESP_LOGI(TAG, "ready: panel %dx%d (ping-pong buffers)", BSP_LCD_H_RES, BSP_LCD_V_RES);
    return ESP_OK;
}

/* Set the clip display transform for the next push_frame (non-locking: called
 * from render_task immediately before push_frame in the same task iteration). */
void eye_display_set_transform(int off_x, int off_y, int rot_deg)
{
    s_off_x   = off_x;
    s_off_y   = off_y;
    s_rot_deg = rot_deg;
}

/* Show/hide the eye image so the native LVGL info screen can take over the
 * panel. Safe to call from any task -- takes the LVGL lock. */
void eye_display_show_eye(bool show);

void eye_display_show_eye(bool show)
{
    if (s_img == NULL) {
        return;
    }
    bsp_display_lock(0);
    if (show) {
        lv_obj_remove_flag(s_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_img);
    } else {
        lv_obj_add_flag(s_img, LV_OBJ_FLAG_HIDDEN);
    }
    bsp_display_unlock();
}

void eye_display_push_frame(const uint16_t *fb240, int scale_pct)
{
    if (s_out[0] == NULL || s_img == NULL) {
        return;
    }

    if (scale_pct != s_scale_pct) {
        update_scale(scale_pct);
    }

    const int dim  = s_out_dim;
    const int back = 1 - s_front;
    uint16_t *dst  = s_out[back];

    /* Write new frame into the back buffer -- LVGL is reading s_out[s_front]
     * via DMA, so this write is safe without holding the BSP lock. */
    if (fb240 != NULL) {
        if (s_rot_deg != 0) {
            /* Combined scale + rotate in one pass using integer fixed-point
             * arithmetic. Pre-computes cos/sin (Q12) when the angle changes;
             * skips LVGL's per-frame SW transform entirely. */
            if (s_rot_deg != s_last_rot) {
                float rad = (float)s_rot_deg * (3.14159265f / 180.0f);
                s_rot_cos = (int32_t)(cosf(rad) * 4096.0f + 0.5f);
                s_rot_sin = (int32_t)(sinf(rad) * 4096.0f + 0.5f);
                s_last_rot = s_rot_deg;
            }
            const int32_t cx = dim / 2;
            const int32_t cy = dim / 2;
            for (int dy = 0; dy < dim; dy++) {
                const int32_t dy0 = dy - cy;
                uint16_t *drow = dst + (size_t)dy * dim;
                for (int dx = 0; dx < dim; dx++) {
                    const int32_t dx0 = dx - cx;
                    int32_t sx = ((s_rot_cos * dx0 + s_rot_sin * dy0) >> 12) + cx;
                    int32_t sy = ((-s_rot_sin * dx0 + s_rot_cos * dy0) >> 12) + cy;
                    if ((uint32_t)sx < (uint32_t)dim && (uint32_t)sy < (uint32_t)dim) {
                        drow[dx] = fb240[(size_t)s_map[sy] * EYE_FB_W + s_map[sx]];
                    } else {
                        drow[dx] = 0;
                    }
                }
            }
        } else {
            for (int dy = 0; dy < dim; dy++) {
                const uint16_t *srow = fb240 + (size_t)s_map[dy] * EYE_FB_W;
                uint16_t       *drow = dst + (size_t)dy * dim;
                for (int dx = 0; dx < dim; dx++) {
                    drow[dx] = srow[s_map[dx]];
                }
            }
        }
    } else {
        memset(dst, 0, (size_t)dim * dim * sizeof(uint16_t));
    }

    /* Swap under lock: LVGL now points at the freshly written buffer. */
    bsp_display_lock(0);
    s_front = back;
    s_dsc.header.w      = (uint32_t)dim;
    s_dsc.header.h      = (uint32_t)dim;
    s_dsc.header.stride = (uint16_t)LV_DRAW_BUF_STRIDE(dim, LV_COLOR_FORMAT_RGB565);
    s_dsc.data_size     = (uint32_t)dim * dim * sizeof(uint16_t);
    s_dsc.data          = (const uint8_t *)s_out[s_front];
    lv_image_set_src(s_img, &s_dsc);
    lv_obj_set_size(s_img, dim, dim);
    lv_image_set_inner_align(s_img, LV_IMAGE_ALIGN_CENTER);
    /* Apply x/y offset from center. */
    lv_obj_align(s_img, LV_ALIGN_CENTER, s_off_x, s_off_y);
    /* Rotation is now done in the pixel loop above; LVGL always blits straight. */
    lv_obj_invalidate(s_img);
    bsp_display_unlock();
}
