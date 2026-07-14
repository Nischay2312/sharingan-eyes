#include "eye_demo.h"

#include <stdio.h>
#include <stdlib.h>

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "nvs.h"

#include "eye_config.h"
#include "eye_params.h"
#include "eye_scene.h"
#include "eye_info.h"
#include "eye_ui.h"
#include "eye_imu.h"
#include "eye_console.h"
#include "eye_video.h"
#include "eye_clip_params.h"
#include "eye_display.h"

static const char *TAG = "eye_demo";

#define EYE_TARGET_FPS   30
#define INFO_FPS         10    /* the info screen is mostly static           */
#define GESTURE_HZ       100   /* poll the gyro well above the frame rate    */
#define EYE_RENDER_CORE   1    /* keep heavy rendering off core 0            */

/* Shared, live-tunable parameters (console + gesture write, renderer reads). */
static eye_params_t         s_params;
static uint16_t            *s_fb;    /* 240x240 RGB565 working framebuffer */
static eye_push_frame_fn    s_push;  /* board frame presenter              */
static const eye_ui_hooks_t *s_ui;   /* native info UI, or NULL (raster)   */

/* Per-clip display-transform override (loaded from NVS on each clip switch). */
static bool                 s_has_clip_override;
static eye_clip_override_t  s_clip_override;

/* Weak default no-op: boards that don't support transform compile without it. */
__attribute__((weak)) void eye_display_set_transform(int off_x, int off_y, int rot_deg)
{
    (void)off_x; (void)off_y; (void)rot_deg;
}

/* ------------------------------------------------------------- screens --
 * One flat list: [0] = info, [1 .. nclips] = video clips, then the scenes.
 * s_screen is only written through eye_demo_set_screen (any task) and read
 * by the render task; an int write is atomic on the ESP32-S3.
 */
static int s_nclips;
static volatile int s_screen;   /* boot on the info screen (0) */

typedef enum { SCR_INFO, SCR_CLIP, SCR_SCENE } scr_kind_t;

static scr_kind_t screen_kind(int idx, int *arg)
{
    if (idx <= 0) {
        if (arg) *arg = 0;
        return SCR_INFO;
    }
    if (idx - 1 < s_nclips) {
        if (arg) *arg = idx - 1;                 /* clip index  */
        return SCR_CLIP;
    }
    if (arg) *arg = idx - 1 - s_nclips;          /* scene id    */
    return SCR_SCENE;
}

int eye_demo_screen_count(void)
{
    return 1 + s_nclips + EYE_SCENE_COUNT;
}

int eye_demo_screen(void)
{
    return s_screen;
}

void eye_demo_describe_screen(int idx, char *buf, size_t n)
{
    int arg;
    switch (screen_kind(idx, &arg)) {
    case SCR_INFO:
        snprintf(buf, n, "info");
        break;
    case SCR_CLIP:
        snprintf(buf, n, "clip: %s", eye_video_clip_name(arg));
        break;
    default:
        snprintf(buf, n, "scene: %s", eye_scene_name(arg));
        break;
    }
}

int eye_demo_screen_for_scene(int scene_id)
{
    if (scene_id < 0 || scene_id >= EYE_SCENE_COUNT) {
        return -1;
    }
    return 1 + s_nclips + scene_id;
}

int eye_demo_first_clip_screen(void)
{
    return s_nclips > 0 ? 1 : -1;
}

void eye_demo_set_screen(int idx)
{
    int n = eye_demo_screen_count();
    idx = ((idx % n) + n) % n;

    int prev = s_screen;
    int prev_arg, arg;
    scr_kind_t prev_kind = screen_kind(prev, &prev_arg);
    scr_kind_t kind      = screen_kind(idx, &arg);

    switch (kind) {
    case SCR_CLIP:
        eye_video_select(arg);
        s_has_clip_override = eye_clip_override_load(eye_video_clip_name(arg),
                                                     &s_clip_override);
        break;
    case SCR_SCENE:
        s_params.scene = arg;
        if (prev_kind == SCR_SCENE && prev != idx) {
            eye_scene_select(arg);   /* scene -> scene: blink + morph      */
        } else {
            eye_scene_enter(arg);    /* arriving fresh: dramatic eye-open  */
        }
        break;
    default:
        break;
    }
    s_screen = idx;

    char desc[48];
    eye_demo_describe_screen(idx, desc, sizeof(desc));
    ESP_LOGI(TAG, "screen %d/%d: %s", idx + 1, n, desc);
}

void eye_demo_step_screen(int delta)
{
    eye_demo_set_screen(s_screen + delta);
}

eye_params_t *eye_demo_params(void)
{
    return &s_params;
}

void eye_demo_refresh_clips(void)
{
    s_nclips = eye_video_count();
    if (s_screen >= eye_demo_screen_count()) {
        s_screen = 0;
    }
}

void eye_demo_reload_clip_override(void)
{
    int arg;
    if (screen_kind(s_screen, &arg) == SCR_CLIP) {
        s_has_clip_override = eye_clip_override_load(eye_video_clip_name(arg),
                                                     &s_clip_override);
    }
}

/* Startup clip: stored by name in NVS ("eye"/"boot_clip"), resolved to a clip
 * index at boot. Kept as a name (not index) so it survives playlist reorders. */
#define BOOT_NVS_NS   "eye"
#define BOOT_NVS_KEY  "boot_clip"

void eye_demo_set_startup_clip(const char *name)
{
    nvs_handle_t h;
    if (nvs_open(BOOT_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (name == NULL || name[0] == '\0') {
        nvs_erase_key(h, BOOT_NVS_KEY);   /* clear -> boot on info screen */
    } else {
        nvs_set_str(h, BOOT_NVS_KEY, name);
    }
    nvs_commit(h);
    nvs_close(h);
}

void eye_demo_get_startup_clip(char *buf, size_t n)
{
    if (buf == NULL || n == 0) {
        return;
    }
    buf[0] = '\0';
    nvs_handle_t h;
    if (nvs_open(BOOT_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = n;
        if (nvs_get_str(h, BOOT_NVS_KEY, buf, &len) != ESP_OK) {
            buf[0] = '\0';
        }
        nvs_close(h);
    }
}

/* Mirror the framebuffer in place per the fliph/flipv params (the lens or the
 * mask mounting may invert the optical path). Cheap: one pass of swaps. */
static void apply_flip_vals(uint16_t *fb, int flip_h, int flip_v)
{
    if (flip_h) {
        for (int y = 0; y < EYE_FB_H; y++) {
            uint16_t *row = fb + y * EYE_FB_W;
            for (int x = 0; x < EYE_FB_W / 2; x++) {
                uint16_t t = row[x];
                row[x] = row[EYE_FB_W - 1 - x];
                row[EYE_FB_W - 1 - x] = t;
            }
        }
    }
    if (flip_v) {
        for (int y = 0; y < EYE_FB_H / 2; y++) {
            uint16_t *a = fb + y * EYE_FB_W;
            uint16_t *b = fb + (EYE_FB_H - 1 - y) * EYE_FB_W;
            for (int x = 0; x < EYE_FB_W; x++) {
                uint16_t t = a[x];
                a[x] = b[x];
                b[x] = t;
            }
        }
    }
}

static void apply_flip(uint16_t *fb)
{
    apply_flip_vals(fb, s_params.flip_h, s_params.flip_v);
}

/* -------------------------------------------------------------- tasks -- */

/*
 * Dedicated gesture task: sampling the gyro at 100 Hz (decoupled from the
 * frame rate) makes a quick flick reliable to catch. The flick's rotation
 * direction picks forward vs backward through the screen list.
 */
static void gesture_task(void *arg)
{
    eye_params_t *p = (eye_params_t *)arg;
    const TickType_t period = pdMS_TO_TICKS(1000 / GESTURE_HZ);
    TickType_t last = xTaskGetTickCount();

    for (;;) {
        if (p->flick_enabled && eye_imu_present()) {
            int dir = eye_imu_poll_flick(p->flick_hi_dps, p->flick_lo_dps);
            if (dir != 0) {
                eye_demo_step_screen(dir);
            }
        }
        vTaskDelayUntil(&last, period);
    }
}

/*
 * Render task. Pinned to core 1 so the PSRAM upscale + panel refresh never
 * starve core 0 (console REPL, idle task). It ALWAYS yields each frame --
 * pacing to the target FPS when frames are cheap, and at least one tick when
 * a frame overruns -- so the task watchdog stays fed.
 */
static void render_task(void *arg)
{
    (void)arg;
    const uint32_t t0   = (uint32_t)(esp_timer_get_time() / 1000);
    const bool     have_ui = s_ui && s_ui->available && s_ui->available();
    bool           ui_shown = false;   /* native info UI currently on screen */
    TickType_t     last = xTaskGetTickCount();

    for (;;) {
        uint32_t t_ms = (uint32_t)(esp_timer_get_time() / 1000) - t0;
        uint32_t target_ms = 1000 / EYE_TARGET_FPS;

        int arg_;
        scr_kind_t kind = screen_kind(s_screen, &arg_);

        /* Any time we're not on the info screen, make sure the native UI is
         * torn down so the eye image is visible again. */
        if (have_ui && ui_shown && kind != SCR_INFO) {
            s_ui->show(false);
            ui_shown = false;
        }

        switch (kind) {
        case SCR_INFO:
            if (have_ui) {
                if (!ui_shown) {
                    s_ui->show(true);         /* hides the eye, shows widgets */
                    ui_shown = true;
                }
                eye_info_status_t st;
                eye_info_gather(&st, t_ms);
                s_ui->update(&st);
            } else {
                eye_info_render(s_fb, t_ms);
                apply_flip(s_fb);
                s_push(s_fb, 100);           /* info text: never overscanned */
            }
            target_ms = 1000 / INFO_FPS;
            break;
        case SCR_CLIP:
            if (eye_video_available() && eye_video_next_frame(s_fb) == ESP_OK) {
                if (s_has_clip_override) {
                    apply_flip_vals(s_fb, s_clip_override.flip_h, s_clip_override.flip_v);
                } else {
                    apply_flip(s_fb);
                }
                int cx = s_has_clip_override ? s_clip_override.off_x   : s_params.clip_off_x;
                int cy = s_has_clip_override ? s_clip_override.off_y   : s_params.clip_off_y;
                int cr = s_has_clip_override ? s_clip_override.rot_deg : s_params.clip_rot_deg;
                int cs = (s_has_clip_override && s_clip_override.scale_pct != 0)
                         ? s_clip_override.scale_pct : s_params.scale_pct;
                eye_display_set_transform(cx, cy, cr);
                s_push(s_fb, cs);
            }
            {
                /* speed_pct scales playback: 200% = 2× faster (half the delay).
                   frame_ms from the .eyv header is the un-scaled base timing. */
                uint32_t raw_ms = eye_video_frame_ms() ? eye_video_frame_ms() : 50;
                uint32_t sp = (uint32_t)(s_params.speed_pct > 0 ? s_params.speed_pct : 100);
                target_ms = raw_ms * 100 / sp;
                if (target_ms < 1) target_ms = 1;
            }
            break;
        default:
            eye_scene_render(s_fb, t_ms, &s_params);
            apply_flip(s_fb);
            eye_display_set_transform(0, 0, 0);   /* scenes: no clip transform */
            s_push(s_fb, s_params.scale_pct);
            break;
        }

        TickType_t period = pdMS_TO_TICKS(target_ms);
        if (period == 0) {
            period = 1;
        }
        TickType_t used = xTaskGetTickCount() - last;
        vTaskDelay(used < period ? (period - used) : 1); /* always yields */
        last = xTaskGetTickCount();
    }
}

void eye_demo_run(eye_push_frame_fn push_frame, i2c_master_bus_handle_t imu_bus,
                  const eye_ui_hooks_t *ui)
{
    if (push_frame == NULL) {
        ESP_LOGE(TAG, "no push_frame provided");
        abort();
    }
    s_push = push_frame;
    s_ui   = ui;

    if (eye_scene_init() != ESP_OK) {
        ESP_LOGE(TAG, "scene engine init failed");
        abort();
    }

    eye_params_init(&s_params);
    eye_params_load(&s_params);     /* restore saved tweaks if any */

    eye_imu_init(imu_bus);          /* tolerant: flick just stays off if absent */
    eye_video_init();               /* tolerant: playlist empty if no clips     */
    s_nclips = eye_video_count();

    /* Boot on the configured startup clip (by name) if it exists; else info. */
    char boot_clip[128];
    eye_demo_get_startup_clip(boot_clip, sizeof(boot_clip));
    if (boot_clip[0]) {
        int idx = eye_video_index_for_name(boot_clip);
        int scr = eye_demo_first_clip_screen();
        if (idx >= 0 && scr >= 0) {
            eye_demo_set_screen(scr + idx);
            ESP_LOGI(TAG, "startup clip: %s (screen %d)", boot_clip, s_screen);
        } else {
            ESP_LOGW(TAG, "startup clip '%s' not found; booting on info", boot_clip);
        }
    }

    eye_console_start(&s_params);   /* serial UI (non-blocking REPL task) */

    /* 16-byte aligned: the JPEG decoder writes its RGB565 output here. */
    s_fb = (uint16_t *)heap_caps_aligned_alloc(16, EYE_FB_PX * sizeof(uint16_t),
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_fb == NULL) {
        s_fb = (uint16_t *)heap_caps_aligned_alloc(16, EYE_FB_PX * sizeof(uint16_t),
                                                   MALLOC_CAP_8BIT);
    }
    if (s_fb == NULL) {
        ESP_LOGE(TAG, "framebuffer alloc failed");
        abort();
    }

    if (eye_imu_present()) {
        xTaskCreate(gesture_task, "eye_gesture", 3072, &s_params, 4, NULL);
    }
    xTaskCreatePinnedToCore(render_task, "eye_render", 4096, NULL, 5, NULL,
                            EYE_RENDER_CORE);

    ESP_LOGI(TAG, "running: %d screens (info + %d clips + %d scenes), flick=%s",
             eye_demo_screen_count(), s_nclips, EYE_SCENE_COUNT,
             eye_imu_present() ? "on (QMI8658)" : "off (no IMU)");

    /* Hand core 0 back to the console / idle; this task just parks. */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
