/*
 * Board-specific display backend for the Waveshare ESP32-S3-Touch-LCD-1.28
 * (GC9A01, 240x240 round, SPI) -- now running LVGL, mirroring the AMOLED
 * architecture: esp_lcd brings the panel up (incl. Waveshare's vendor gamma),
 * esp_lvgl_port owns flushing, and the eye reaches the glass as an LVGL image
 * refreshed each frame. That lets the shared native info screen (eye_ui_lvgl,
 * with touch via the CST816S) run on this board too.
 *
 * Color pipeline: the engine renders native little-endian RGB565; the GC9A01
 * wants big-endian on the wire, so the LVGL port is configured with
 * .flags.swap_bytes = true (this replaces the old manual per-pixel swap).
 * BGR element order + INVON + the vendor init stay exactly as proven.
 */

#include "eye_display.h"
#include "eye_config.h"

#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_gc9a01.h"
#include "esp_lcd_touch_cst816s.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"

static const char *TAG = "eye_disp_gc9a01";

/* ---- Pin map (VERIFY against the 1.28" schematic) ---- */
#define LCD_SPI_HOST   SPI2_HOST
#define LCD_PIN_SCLK   10
#define LCD_PIN_MOSI   11
#define LCD_PIN_CS      9
#define LCD_PIN_DC      8
#define LCD_PIN_RST    14
#define LCD_PIN_BL      2     /* backlight (LEDC PWM)                  */
#define TP_PIN_RST     13     /* CST816S touch, shared I2C bus SDA6/7  */
#define TP_PIN_INT      5
#define LCD_PCLK_HZ    (80 * 1000 * 1000)

/* Board-local extras beside the eye_display.h contract (app_main wires them). */
void      eye_display_set_brightness(int pct);
esp_err_t eye_display_add_touch(i2c_master_bus_handle_t bus);

static esp_lcd_panel_handle_t    s_panel;
static esp_lcd_panel_io_handle_t s_io;
static lv_display_t             *s_lvdisp;
static lv_obj_t                 *s_img;
static lv_image_dsc_t            s_dsc;
static uint16_t                 *s_out;    /* 240x240 native-LE RGB565 */

/* Clip display transform (set by eye_display_set_transform). */
static int     s_off_x;
static int     s_off_y;
static int     s_rot_deg;
static int     s_last_rot  = -1;
static int32_t s_rot_cos   = 4096;  /* cos(angle) × 2^12 */
static int32_t s_rot_sin   = 0;     /* sin(angle) × 2^12 */

/* Nearest-neighbour scale map: s_map[d] = source fb240 pixel for eye pixel d.
 * Sized for max scale (130% of 240 = 312; use 320 for headroom). */
#define LCD_MAP_MAX  320
static uint16_t s_map[LCD_MAP_MAX];
static int      s_eye_dim       = EYE_FB_W;
static int      s_eye_scale_pct = -1;

static void update_scale(int pct)
{
    int dim = EYE_FB_W * pct / 100;
    if (dim < 40)        dim = 40;
    if (dim > LCD_MAP_MAX) dim = LCD_MAP_MAX;
    s_eye_dim       = dim;
    s_eye_scale_pct = pct;
    for (int d = 0; d < dim; d++) {
        s_map[d] = (uint16_t)(d * EYE_FB_W / dim);
    }
}

void eye_display_set_transform(int off_x, int off_y, int rot_deg)
{
    s_off_x   = off_x;
    s_off_y   = off_y;
    s_rot_deg = rot_deg;
}

/*
 * Waveshare's exact GC9A01 init (from their LCD_1in28.cpp). The generic init in
 * esp_lcd_gc9a01 produces washed-out, pinkish color; this vendor sequence loads
 * the proper gamma (0xF0-0xF3) and power/VREG (0xC3/0xC4...) tuning for deep,
 * saturated reds. Sent on top of the driver's init -- the appearance registers
 * override, the driver still owns address windowing for the LVGL flush.
 */
typedef struct {
    uint8_t cmd;
    uint8_t data[14];
    uint8_t len;        /* data byte count */
    uint16_t delay_ms;  /* post-command delay */
} gc_init_cmd_t;

static const gc_init_cmd_t k_gc9a01_init[] = {
    {0xEF, {0}, 0, 0},
    {0xEB, {0x14}, 1, 0},
    {0xFE, {0}, 0, 0},
    {0xEF, {0}, 0, 0},
    {0xEB, {0x14}, 1, 0},
    {0x84, {0x40}, 1, 0},
    {0x85, {0xFF}, 1, 0},
    {0x86, {0xFF}, 1, 0},
    {0x87, {0xFF}, 1, 0},
    {0x88, {0x0A}, 1, 0},
    {0x89, {0x21}, 1, 0},
    {0x8A, {0x00}, 1, 0},
    {0x8B, {0x80}, 1, 0},
    {0x8C, {0x01}, 1, 0},
    {0x8D, {0x01}, 1, 0},
    {0x8E, {0xFF}, 1, 0},
    {0x8F, {0xFF}, 1, 0},
    {0xB6, {0x00, 0x20}, 2, 0},
    {0x3A, {0x05}, 1, 0},  /* 16-bit/pixel (RGB565) */
    {0x90, {0x08, 0x08, 0x08, 0x08}, 4, 0},
    {0xBD, {0x06}, 1, 0},
    {0xBC, {0x00}, 1, 0},
    {0xFF, {0x60, 0x01, 0x04}, 3, 0},
    {0xC3, {0x13}, 1, 0},  /* VREG1A: contrast/saturation */
    {0xC4, {0x13}, 1, 0},  /* VREG1B */
    {0xC9, {0x22}, 1, 0},
    {0xBE, {0x11}, 1, 0},
    {0xE1, {0x10, 0x0E}, 2, 0},
    {0xDF, {0x21, 0x0C, 0x02}, 3, 0},
    {0xF0, {0x45, 0x09, 0x08, 0x08, 0x26, 0x2A}, 6, 0},  /* gamma */
    {0xF1, {0x43, 0x70, 0x72, 0x36, 0x37, 0x6F}, 6, 0},
    {0xF2, {0x45, 0x09, 0x08, 0x08, 0x26, 0x2A}, 6, 0},
    {0xF3, {0x43, 0x70, 0x72, 0x36, 0x37, 0x6F}, 6, 0},
    {0xED, {0x1B, 0x0B}, 2, 0},
    {0xAE, {0x77}, 1, 0},
    {0xCD, {0x63}, 1, 0},
    {0x70, {0x07, 0x07, 0x04, 0x0E, 0x0F, 0x09, 0x07, 0x08, 0x03}, 9, 0},
    {0xE8, {0x34}, 1, 0},
    {0x62, {0x18, 0x0D, 0x71, 0xED, 0x70, 0x70, 0x18, 0x0F, 0x71, 0xEF, 0x70, 0x70}, 12, 0},
    {0x63, {0x18, 0x11, 0x71, 0xF1, 0x70, 0x70, 0x18, 0x13, 0x71, 0xF3, 0x70, 0x70}, 12, 0},
    {0x64, {0x28, 0x29, 0xF1, 0x01, 0xF1, 0x00, 0x07}, 7, 0},
    {0x66, {0x3C, 0x00, 0xCD, 0x67, 0x45, 0x45, 0x10, 0x00, 0x00, 0x00}, 10, 0},
    {0x67, {0x00, 0x3C, 0x00, 0x00, 0x00, 0x01, 0x54, 0x10, 0x32, 0x98}, 10, 0},
    {0x74, {0x10, 0x85, 0x80, 0x00, 0x00, 0x4E, 0x00}, 7, 0},
    {0x98, {0x3E, 0x07}, 2, 0},
    {0x35, {0}, 0, 0},
    {0x21, {0}, 0, 0},     /* display inversion ON */
    {0x11, {0}, 0, 120},   /* sleep out */
    {0x29, {0}, 0, 20},    /* display on */
};

static void gc9a01_apply_vendor_init(void)
{
    for (size_t i = 0; i < sizeof(k_gc9a01_init) / sizeof(k_gc9a01_init[0]); i++) {
        const gc_init_cmd_t *c = &k_gc9a01_init[i];
        esp_lcd_panel_io_tx_param(s_io, c->cmd, c->len ? c->data : NULL, c->len);
        if (c->delay_ms) {
            vTaskDelay(pdMS_TO_TICKS(c->delay_ms));
        }
    }
}

esp_err_t eye_display_init(void)
{
    spi_bus_config_t buscfg = {
        .sclk_io_num     = LCD_PIN_SCLK,
        .mosi_io_num     = LCD_PIN_MOSI,
        .miso_io_num     = -1,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = EYE_FB_W * EYE_FB_H * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num       = LCD_PIN_CS,
        .dc_gpio_num       = LCD_PIN_DC,
        .spi_mode          = 0,
        .pclk_hz           = LCD_PCLK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits      = 8,
        .lcd_param_bits    = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST,
                                             &io_cfg, &s_io));

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_PIN_RST,
        /* BGR + byte-swap + invert == Waveshare's stock driver (MADCTL 0x08,
         * INVON 0x21, big-endian pixels). The byte swap now happens in the
         * LVGL port (.flags.swap_bytes) instead of a manual loop. */
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(s_io, &panel_cfg, &s_panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    gc9a01_apply_vendor_init();   /* Waveshare gamma/power -> deep, vivid color */
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    /* ---- LVGL on top (same stack as the AMOLED's BSP uses) ---- */
    const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle     = s_io,
        .panel_handle  = s_panel,
        .buffer_size   = EYE_FB_W * 60,   /* 1/4-screen partial buffers */
        .double_buffer = true,
        .hres          = EYE_FB_W,
        .vres          = EYE_FB_H,
        .monochrome    = false,
        .color_format  = LV_COLOR_FORMAT_RGB565,
        .rotation      = { .swap_xy = false, .mirror_x = false, .mirror_y = false },
        .flags = {
            .buff_dma   = true,
            .swap_bytes = true,   /* LE engine pixels -> BE GC9A01 wire */
        },
    };
    s_lvdisp = lvgl_port_add_disp(&disp_cfg);
    if (s_lvdisp == NULL) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        return ESP_FAIL;
    }

    /* The eye: one full-screen LVGL image backed by our own RGB565 buffer. */
    s_out = (uint16_t *)heap_caps_malloc(EYE_FB_PX * sizeof(uint16_t),
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_out == NULL) {
        s_out = (uint16_t *)heap_caps_malloc(EYE_FB_PX * sizeof(uint16_t),
                                             MALLOC_CAP_8BIT);
    }
    if (s_out == NULL) {
        ESP_LOGE(TAG, "eye buffer alloc failed");
        return ESP_ERR_NO_MEM;
    }
    memset(s_out, 0, EYE_FB_PX * sizeof(uint16_t));

    lvgl_port_lock(0);
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    memset(&s_dsc, 0, sizeof(s_dsc));
    s_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    s_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
    s_dsc.header.flags  = LV_IMAGE_FLAGS_MODIFIABLE;
    s_dsc.header.w      = EYE_FB_W;
    s_dsc.header.h      = EYE_FB_H;
    s_dsc.header.stride = (uint16_t)LV_DRAW_BUF_STRIDE(EYE_FB_W, LV_COLOR_FORMAT_RGB565);
    s_dsc.data_size     = EYE_FB_PX * sizeof(uint16_t);
    s_dsc.data          = (const uint8_t *)s_out;

    s_img = lv_image_create(scr);
    lv_image_set_antialias(s_img, false);
    lv_display_set_antialiasing(s_lvdisp, false);
    lv_image_set_src(s_img, &s_dsc);
    lv_obj_set_size(s_img, EYE_FB_W, EYE_FB_H);
    lv_obj_center(s_img);
    lvgl_port_unlock();

    eye_display_set_brightness(100);   /* eye_net restores the saved level */
    update_scale(100);

    ESP_LOGI(TAG, "GC9A01 + LVGL ready (%dx%d, PWM backlight, vendor gamma)",
             EYE_FB_W, EYE_FB_H);
    return ESP_OK;
}

/* CST816S touch on the shared I2C bus -> LVGL input (info-screen widgets). */
esp_err_t eye_display_add_touch(i2c_master_bus_handle_t bus)
{
    if (bus == NULL || s_lvdisp == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
    io_cfg.scl_speed_hz = 400000;
    esp_lcd_panel_io_handle_t tp_io = NULL;
    esp_err_t err = esp_lcd_new_panel_io_i2c(bus, &io_cfg, &tp_io);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "touch io failed: %s", esp_err_to_name(err));
        return err;
    }

    const esp_lcd_touch_config_t tp_cfg = {
        .x_max        = EYE_FB_W,
        .y_max        = EYE_FB_H,
        .rst_gpio_num = TP_PIN_RST,
        .int_gpio_num = TP_PIN_INT,
        .levels = { .reset = 0, .interrupt = 0 },
    };
    esp_lcd_touch_handle_t tp = NULL;
    err = esp_lcd_touch_new_i2c_cst816s(tp_io, &tp_cfg, &tp);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CST816S not detected: %s (touch disabled)",
                 esp_err_to_name(err));
        return err;
    }

    const lvgl_port_touch_cfg_t tcfg = { .disp = s_lvdisp, .handle = tp };
    if (lvgl_port_add_touch(&tcfg) == NULL) {
        ESP_LOGW(TAG, "lvgl_port_add_touch failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "CST816S touch ready");
    return ESP_OK;
}

/* Show/hide the eye image so the native LVGL info screen (eye_ui_lvgl) can
 * take over the panel. Safe from any task -- takes the LVGL lock. */
void eye_display_show_eye(bool show);

void eye_display_show_eye(bool show)
{
    if (s_img == NULL) {
        return;
    }
    lvgl_port_lock(0);
    if (show) {
        lv_obj_remove_flag(s_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_img);
    } else {
        lv_obj_add_flag(s_img, LV_OBJ_FLAG_HIDDEN);
    }
    lvgl_port_unlock();
}

void eye_display_push_frame(const uint16_t *fb240, int scale_pct)
{
    if (s_out == NULL || s_img == NULL || fb240 == NULL) {
        return;
    }

    if (scale_pct != s_eye_scale_pct) {
        update_scale(scale_pct);
    }

    memset(s_out, 0, EYE_FB_PX * sizeof(uint16_t));

    if (s_rot_deg != 0) {
        /* Combined scale + rotate + offset in one pass using Q12 fixed-point. */
        if (s_rot_deg != s_last_rot) {
            float rad = (float)s_rot_deg * (3.14159265f / 180.0f);
            s_rot_cos = (int32_t)(cosf(rad) * 4096.0f + 0.5f);
            s_rot_sin = (int32_t)(sinf(rad) * 4096.0f + 0.5f);
            s_last_rot = s_rot_deg;
        }
        const int32_t cx   = EYE_FB_W / 2 + s_off_x;
        const int32_t cy   = EYE_FB_H / 2 + s_off_y;
        const int32_t half = s_eye_dim / 2;
        for (int oy = 0; oy < EYE_FB_H; oy++) {
            const int32_t dy0 = oy - cy;
            uint16_t *drow = s_out + (size_t)oy * EYE_FB_W;
            for (int ox = 0; ox < EYE_FB_W; ox++) {
                const int32_t dx0 = ox - cx;
                int32_t ex = ((s_rot_cos * dx0 + s_rot_sin * dy0) >> 12) + half;
                int32_t ey = ((-s_rot_sin * dx0 + s_rot_cos * dy0) >> 12) + half;
                if ((uint32_t)ex < (uint32_t)s_eye_dim && (uint32_t)ey < (uint32_t)s_eye_dim) {
                    drow[ox] = fb240[(size_t)s_map[ey] * EYE_FB_W + s_map[ex]];
                }
            }
        }
    } else {
        /* Fast path: scale + offset, no rotation. */
        const int half = s_eye_dim / 2;
        const int x0   = EYE_FB_W / 2 + s_off_x - half;
        const int y0   = EYE_FB_H / 2 + s_off_y - half;
        for (int dy = 0; dy < s_eye_dim; dy++) {
            int oy = y0 + dy;
            if ((unsigned)oy >= (unsigned)EYE_FB_H) continue;
            const uint16_t *srow = fb240 + (size_t)s_map[dy] * EYE_FB_W;
            uint16_t       *drow = s_out + (size_t)oy * EYE_FB_W;
            for (int dx = 0; dx < s_eye_dim; dx++) {
                int ox = x0 + dx;
                if ((unsigned)ox >= (unsigned)EYE_FB_W) continue;
                drow[ox] = srow[s_map[dx]];
            }
        }
    }

    lvgl_port_lock(0);
    lv_image_set_src(s_img, &s_dsc);
    lv_obj_invalidate(s_img);
    lvgl_port_unlock();
}

/* Backlight brightness 5..100% -- the eye_net brightness callback. */
void eye_display_set_brightness(int pct)
{
    static bool s_ledc_ready;
    if (!s_ledc_ready) {
        const ledc_timer_config_t tcfg = {
            .speed_mode      = LEDC_LOW_SPEED_MODE,
            .timer_num       = LEDC_TIMER_0,
            .duty_resolution = LEDC_TIMER_8_BIT,
            .freq_hz         = 5000,
            .clk_cfg         = LEDC_AUTO_CLK,
        };
        const ledc_channel_config_t ccfg = {
            .gpio_num   = LCD_PIN_BL,
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel    = LEDC_CHANNEL_0,
            .timer_sel  = LEDC_TIMER_0,
            .duty       = 255,
        };
        if (ledc_timer_config(&tcfg) != ESP_OK ||
            ledc_channel_config(&ccfg) != ESP_OK) {
            ESP_LOGW(TAG, "LEDC init failed; backlight pinned on");
            gpio_set_direction(LCD_PIN_BL, GPIO_MODE_OUTPUT);
            gpio_set_level(LCD_PIN_BL, 1);
            return;
        }
        s_ledc_ready = true;
    }
    if (pct < 5) pct = 5;
    if (pct > 100) pct = 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, (uint32_t)(pct * 255) / 100);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}
