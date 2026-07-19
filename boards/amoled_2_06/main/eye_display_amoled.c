/*
 * Board-specific display backend for the Waveshare ESP32-S3-Touch-AMOLED-2.06.
 *
 * DIRECT-TO-PANEL (no LVGL)
 * -------------------------
 * The eye is pushed straight to the SH8601 with esp_lcd_panel_draw_bitmap and
 * we wait on our OWN color-trans-done semaphore. LVGL is deliberately NOT in the
 * frame path.
 *
 * Why: when the eye went through an LVGL image, the LVGL task's panel flush
 * intermittently lost its trans-done interrupt and wedged in wait_for_flushing
 * while holding the port mutex -- so the next frame's bsp_display_lock() blocked
 * forever and the display froze silently on a random frame (confirmed by
 * bisection: the hang was always "push sub-stage 'lock-wait'", heap flat, with
 * nothing but the eye animating). Owning the transaction and its completion
 * semaphore removes that entire failure class: a lost/late done just times out
 * and the next draw re-arms, instead of deadlocking.
 *
 * FULL-PANEL CANVAS + BANDED TRANSFER
 * -----------------------------------
 * We keep one full 410x502 RGB565 canvas in PSRAM. Each frame we clear it to
 * black and blit the (scaled/rotated) 240x240 eye into it at panel-center +
 * offset. Composing the whole panel means the bands above/below the eye are
 * always cleared regardless of offset/rotation -- no stale pixels.
 *
 * The panel is then pushed in horizontal BANDS through a small internal-RAM,
 * DMA-capable bounce buffer -- exactly how the working LVGL path flushed it. A
 * single full-frame QSPI transfer straight from PSRAM fails at spi queue time
 * ("spi transmit (queue) color failed"): that path never runs in the vendor's
 * default config (it stripes from internal RAM). So we stripe too.
 *
 * BYTE ORDER
 * ----------
 * The panel wants big-endian RGB565; the engine/JPEG decoder produce native
 * little-endian. The old LVGL path set swap_bytes=true; here we byte-swap each
 * pixel as we blit it into the canvas.
 *
 * SCALING / LENS TEST
 * -------------------
 * The board-agnostic engine renders a 240x240 eye. This backend nearest-neighbour
 * upscales it to a runtime size and centers it. scale_pct = 100 makes the eye
 * diameter equal the panel WIDTH (~410 px). scale_pct > 100 overscans toward the
 * panel height (left/right edges clipped); < 100 shrinks it.
 */

#include "eye_display.h"
#include "eye_config.h"

#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"

static const char *TAG = "eye_disp_amoled";

#define PANEL_W  BSP_LCD_H_RES   /* 410 */
#define PANEL_H  BSP_LCD_V_RES   /* 502 */

/* Rows per banded transfer. Even (SH8601 window alignment); small enough that
 * one band lives comfortably in internal DMA RAM (410*64*2 = 52 KB). */
#define BAND_ROWS  64

/* Largest eye square we ever build: the panel's longer side (fills height). */
#define EYE_MAX_OUT_DIM   BSP_LCD_V_RES   /* 502 */
#define EYE_SCALE_MIN_PCT 20
#define EYE_SCALE_MAX_PCT 130

/* Panel + IO handles from bsp_display_new (LVGL is never started). */
static esp_lcd_panel_handle_t     s_panel;
static esp_lcd_panel_io_handle_t  s_io;
static SemaphoreHandle_t          s_flush_done;   /* given by trans-done ISR   */

/* Serializes everything that talks on the QSPI IO handle: the flush task's pixel
 * DMA AND panel commands like brightness (SH8601 0x51). Without this, a
 * brightness change from the web/console task mid-transfer collides with the
 * flush and can lose its trans-done -- the exact wedge we removed LVGL to fix. */
static SemaphoreHandle_t          s_bus_mutex;

/* PIPELINE
 * --------
 * Two full-panel canvases in PSRAM. The render task composes a frame into a free
 * canvas and hands its index to the flush task, then goes on to decode the next
 * frame. The flush task pushes that canvas to the panel band-by-band (via the
 * internal DMA bounce buffer) IN PARALLEL on the other core. Overlapping the
 * ~30 ms panel transfer with the ~23 ms decode is what makes it smooth -- the
 * same trick LVGL used (flush in its own task). Two queues hand canvas indices
 * back and forth; q_free provides backpressure so the render task can be at most
 * one frame ahead and never overwrites a canvas mid-transfer. */
#define NUM_CANVAS 2
static uint16_t      *s_canvas[NUM_CANVAS];
static uint16_t      *s_bounce;          /* PANEL_W * BAND_ROWS, internal DMA */
static QueueHandle_t  s_q_ready;         /* render -> flush: composed indices  */
static QueueHandle_t  s_q_free;          /* flush -> render: reusable indices  */
static bool           s_visible = true;   /* eye shown vs. blanked            */

static uint16_t       s_map[EYE_MAX_OUT_DIM];   /* nearest-neighbour dst->src */
static int            s_out_dim;                 /* current eye square size    */
static int            s_scale_pct = -1;          /* last applied scale         */

/* Clip display transform, set by eye_display_set_transform before push_frame. */
static int  s_off_x;
static int  s_off_y;
static int  s_rot_deg;
static int  s_last_rot = -1;   /* last angle for which cos/sin were computed  */

/* Fixed-point cos/sin for the custom rotation loop (pre-computed per angle). */
static int32_t s_rot_cos = 4096;  /* cos(angle) x 2^12 */
static int32_t s_rot_sin = 0;     /* sin(angle) x 2^12 */

static inline uint16_t swap16(uint16_t v)
{
    return (uint16_t)((v << 8) | (v >> 8));
}

/* trans-done ISR: the panel finished a draw_bitmap. Wake push_frame. */
static bool IRAM_ATTR on_color_trans_done(esp_lcd_panel_io_handle_t io,
                                          esp_lcd_panel_io_event_data_t *edata,
                                          void *user_ctx)
{
    (void)io; (void)edata; (void)user_ctx;
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_flush_done, &hp);
    return hp == pdTRUE;
}

/* Transfer one composed canvas to the panel, band by band, through the internal
 * DMA bounce buffer. Runs in the flush task, in parallel with the render task's
 * next decode. A lost/late trans-done just times out and we move on -- it can
 * never deadlock anything. */
static void flush_canvas(const uint16_t *canvas)
{
    /* Hold the bus for the whole frame so a brightness command can't slip
     * between bands. Released once the last band's DMA completes. */
    xSemaphoreTake(s_bus_mutex, portMAX_DELAY);
    for (int y0 = 0; y0 < PANEL_H; y0 += BAND_ROWS) {
        int rows = PANEL_H - y0;
        if (rows > BAND_ROWS) {
            rows = BAND_ROWS;
        }
        const size_t band_px = (size_t)PANEL_W * rows;

        memcpy(s_bounce, canvas + (size_t)y0 * PANEL_W, band_px * sizeof(uint16_t));

        esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, 0, y0, PANEL_W, y0 + rows,
                                                  s_bounce);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "draw_bitmap band y=%d failed: %s", y0, esp_err_to_name(err));
            xSemaphoreGive(s_bus_mutex);
            return;
        }
        if (xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGW(TAG, "flush trans-done timeout at band y=%d", y0);
        }
    }
    xSemaphoreGive(s_bus_mutex);
}

/* Bus-safe brightness: an SH8601 panel command (0x51) shares the QSPI IO handle
 * with the pixel DMA, so serialize it with the flush task via the bus mutex.
 * Called from the web/console task (board_brightness_set). */
void eye_display_set_brightness(int pct)
{
    if (s_bus_mutex) {
        xSemaphoreTake(s_bus_mutex, portMAX_DELAY);
        bsp_display_brightness_set(pct);
        xSemaphoreGive(s_bus_mutex);
    } else {
        bsp_display_brightness_set(pct);
    }
}

static void flush_task(void *arg)
{
    (void)arg;
    for (;;) {
        int idx;
        if (xQueueReceive(s_q_ready, &idx, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        flush_canvas(s_canvas[idx]);
        xQueueSend(s_q_free, &idx, portMAX_DELAY);   /* return buffer to render */
    }
}

/* Update the scale map and eye dimension. Geometry only. */
static void update_scale(int pct)
{
    if (pct < EYE_SCALE_MIN_PCT) pct = EYE_SCALE_MIN_PCT;
    if (pct > EYE_SCALE_MAX_PCT) pct = EYE_SCALE_MAX_PCT;

    int dim = PANEL_W * pct / 100;              /* 100% == panel width */
    if (dim < 40)               dim = 40;
    if (dim > EYE_MAX_OUT_DIM)  dim = EYE_MAX_OUT_DIM;
    dim &= ~1;                                  /* even width */

    s_out_dim   = dim;
    s_scale_pct = pct;

    for (int d = 0; d < dim; d++) {
        s_map[d] = (uint16_t)((d * EYE_FB_W) / dim);
    }
    ESP_LOGI(TAG, "scale %d%% -> eye %dpx", pct, dim);
}

esp_err_t eye_display_init(void)
{
    const bsp_display_config_t cfg = {
        .max_transfer_sz = PANEL_W * PANEL_H * (int)sizeof(uint16_t),
    };
    esp_err_t err = bsp_display_new(&cfg, &s_panel, &s_io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bsp_display_new failed: %s", esp_err_to_name(err));
        return err;
    }

    s_flush_done = xSemaphoreCreateBinary();
    s_bus_mutex  = xSemaphoreCreateMutex();
    if (s_flush_done == NULL || s_bus_mutex == NULL) {
        ESP_LOGE(TAG, "flush/bus semaphore alloc failed");
        return ESP_ERR_NO_MEM;
    }

    const esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = on_color_trans_done,
    };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(s_io, &cbs, NULL));

    /* bsp_display_new only resets/inits; turn the panel on ourselves. */
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));
    bsp_display_brightness_set(100);
    bsp_display_backlight_on();

    const size_t bytes = (size_t)PANEL_W * PANEL_H * sizeof(uint16_t);
    for (int i = 0; i < NUM_CANVAS; i++) {
        s_canvas[i] = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_canvas[i] == NULL) {
            s_canvas[i] = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
        }
        if (s_canvas[i] == NULL) {
            ESP_LOGE(TAG, "canvas %d alloc failed (%u bytes)", i, (unsigned)bytes);
            return ESP_ERR_NO_MEM;
        }
        memset(s_canvas[i], 0, bytes);
    }

    /* Internal, DMA-capable bounce buffer for one band. */
    const size_t band_bytes = (size_t)PANEL_W * BAND_ROWS * sizeof(uint16_t);
    s_bounce = heap_caps_malloc(band_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (s_bounce == NULL) {
        ESP_LOGE(TAG, "bounce alloc failed (%u bytes)", (unsigned)band_bytes);
        return ESP_ERR_NO_MEM;
    }

    /* Pipeline queues: all canvases start free. */
    s_q_ready = xQueueCreate(NUM_CANVAS, sizeof(int));
    s_q_free  = xQueueCreate(NUM_CANVAS, sizeof(int));
    if (s_q_ready == NULL || s_q_free == NULL) {
        ESP_LOGE(TAG, "pipeline queue alloc failed");
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < NUM_CANVAS; i++) {
        xQueueSend(s_q_free, &i, 0);
    }
    /* Flush task on core 0 so the panel transfer overlaps the eye engine's
     * render/decode task, which eye_demo_run pins to core 1. Keeping the two on
     * separate cores is what lets the transfer and the next decode run at once. */
    xTaskCreatePinnedToCore(flush_task, "eye_flush", 4096, NULL, 5, NULL, 0);

    update_scale(100);

    /* Blank the panel once so it starts black, not garbage. */
    eye_display_push_frame(NULL, 100);

    ESP_LOGI(TAG, "ready: panel %dx%d (direct draw, pipelined)", PANEL_W, PANEL_H);
    return ESP_OK;
}

void eye_display_set_transform(int off_x, int off_y, int rot_deg)
{
    s_off_x   = off_x;
    s_off_y   = off_y;
    s_rot_deg = rot_deg;
}

void eye_display_show_eye(bool show);

void eye_display_show_eye(bool show)
{
    s_visible = show;   /* next push_frame either blits the eye or stays black */
}

/* Zero every canvas pixel OUTSIDE the eye box [bx,bx+dim) x [by,by+dim). The box
 * itself is fully overwritten by the blit (the rotate path writes black outside
 * the source circle), so only the surrounding margins need clearing -- far
 * cheaper than memset-ing the whole 410x502 panel. Clearing the current-frame
 * margins also blacks out wherever a moved box used to be. */
static void clear_margins(uint16_t *canvas, int bx, int by, int dim)
{
    int y_lo = by < 0 ? 0 : (by > PANEL_H ? PANEL_H : by);
    int y_hi = by + dim < 0 ? 0 : (by + dim > PANEL_H ? PANEL_H : by + dim);
    int x_lo = bx < 0 ? 0 : (bx > PANEL_W ? PANEL_W : bx);
    int x_hi = bx + dim < 0 ? 0 : (bx + dim > PANEL_W ? PANEL_W : bx + dim);

    /* Full-width bands above and below the box. */
    if (y_lo > 0) {
        memset(canvas, 0, (size_t)y_lo * PANEL_W * sizeof(uint16_t));
    }
    if (y_hi < PANEL_H) {
        memset(canvas + (size_t)y_hi * PANEL_W, 0,
               (size_t)(PANEL_H - y_hi) * PANEL_W * sizeof(uint16_t));
    }
    /* Left/right strips beside the box (only if it is narrower than the panel). */
    for (int y = y_lo; y < y_hi; y++) {
        uint16_t *row = canvas + (size_t)y * PANEL_W;
        if (x_lo > 0) {
            memset(row, 0, (size_t)x_lo * sizeof(uint16_t));
        }
        if (x_hi < PANEL_W) {
            memset(row + x_hi, 0, (size_t)(PANEL_W - x_hi) * sizeof(uint16_t));
        }
    }
}

void eye_display_push_frame(const uint16_t *fb240, int scale_pct)
{
    if (s_canvas[0] == NULL) {
        return;
    }

    if (scale_pct != s_scale_pct) {
        update_scale(scale_pct);
    }

    /* Claim a free canvas (blocks if both are still being transferred -- this is
     * the pipeline backpressure that paces us to the panel and prevents any
     * canvas being overwritten mid-flush). */
    int idx;
    if (xQueueReceive(s_q_free, &idx, portMAX_DELAY) != pdTRUE) {
        return;
    }
    uint16_t *canvas = s_canvas[idx];

    const int dim = s_out_dim;
    const int bx  = (PANEL_W - dim) / 2 + s_off_x;
    const int by  = (PANEL_H - dim) / 2 + s_off_y;

    if (fb240 == NULL || !s_visible) {
        /* Nothing to draw: whole panel black. */
        memset(canvas, 0, (size_t)PANEL_W * PANEL_H * sizeof(uint16_t));
    } else {
        clear_margins(canvas, bx, by, dim);

        if (s_rot_deg != 0) {
            if (s_rot_deg != s_last_rot) {
                float rad = (float)s_rot_deg * (3.14159265f / 180.0f);
                s_rot_cos = (int32_t)(cosf(rad) * 4096.0f + 0.5f);
                s_rot_sin = (int32_t)(sinf(rad) * 4096.0f + 0.5f);
                s_last_rot = s_rot_deg;
            }
            const int32_t cx = dim / 2;
            const int32_t cy = dim / 2;
            for (int dy = 0; dy < dim; dy++) {
                int py = by + dy;
                if (py < 0 || py >= PANEL_H) continue;
                const int32_t dy0 = dy - cy;
                uint16_t *crow = canvas + (size_t)py * PANEL_W;
                for (int dx = 0; dx < dim; dx++) {
                    int px = bx + dx;
                    if (px < 0 || px >= PANEL_W) continue;
                    const int32_t dx0 = dx - cx;
                    int32_t sx = ((s_rot_cos * dx0 + s_rot_sin * dy0) >> 12) + cx;
                    int32_t sy = ((-s_rot_sin * dx0 + s_rot_cos * dy0) >> 12) + cy;
                    if ((uint32_t)sx < (uint32_t)dim && (uint32_t)sy < (uint32_t)dim) {
                        crow[px] = swap16(fb240[(size_t)s_map[sy] * EYE_FB_W + s_map[sx]]);
                    } else {
                        crow[px] = 0;   /* black outside the rotated circle */
                    }
                }
            }
        } else {
            for (int dy = 0; dy < dim; dy++) {
                int py = by + dy;
                if (py < 0 || py >= PANEL_H) continue;
                const uint16_t *srow = fb240 + (size_t)s_map[dy] * EYE_FB_W;
                uint16_t       *crow = canvas + (size_t)py * PANEL_W;
                for (int dx = 0; dx < dim; dx++) {
                    int px = bx + dx;
                    if (px < 0 || px >= PANEL_W) continue;
                    crow[px] = swap16(srow[s_map[dx]]);
                }
            }
        }
    }

    /* Hand the composed canvas to the flush task and return immediately; the
     * transfer runs in parallel with our next decode. */
    xQueueSend(s_q_ready, &idx, portMAX_DELAY);
}
