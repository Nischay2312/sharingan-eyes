/*
 * Firmware entry point for the Waveshare ESP32-S3-Touch-LCD-1.28 (round).
 *
 * Structure mirrors the AMOLED board: board-specific setup, then the same
 * shared eye_demo_run(). Differences vs. the AMOLED:
 *   - eye_display_gc9a01.c instead of eye_display_amoled.c (1:1 push, PWM BL)
 *   - clips live on a FATFS-formatted 'anim' FLASH partition (/anim) instead
 *     of an SD card -- same web upload/list/delete, different storage
 *   - no LVGL: the raster info screen shows the WiFi status lines, and the
 *     `wifi`/`bright` console commands stand in for the touch sliders
 */

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "nvs_flash.h"
#include "driver/i2c_master.h"
#include "wear_levelling.h"

#include "eye_display.h"
#include "eye_demo.h"
#include "eye_video.h"     /* eye_video_set_dir                        */
#include "eye_battery.h"   /* battery ADC on GPIO1 (eye_battery_adc.c) */
#include "eye_info.h"      /* eye_info_set_board / net provider        */
#include "eye_console.h"   /* eye_console_set_extra                    */
#include "eye_net.h"       /* WiFi + web control lifecycle (eye_web)   */
#include "eye_ui.h"        /* native LVGL info screen (eye_ui_lvgl)    */
#include "eye_webserver.h" /* eye_webserver_log_init (early log capture) */

static const char *TAG = "sharingan_128";

/* The 'anim' flash partition, FATFS-mounted: the clip playlist scans it and
 * the web UI uploads/deletes files in it (like the AMOLED's SD folder). */
#define EYE_CLIP_DIR  "/anim"

/* Provided by eye_display_gc9a01.c (LEDC backlight; CST816S -> LVGL input). */
void      eye_display_set_brightness(int pct);
esp_err_t eye_display_add_touch(i2c_master_bus_handle_t bus);

static void board_storage_info(uint64_t *total_kb, uint64_t *free_kb)
{
    uint64_t total = 0, free = 0;
    esp_vfs_fat_info(EYE_CLIP_DIR, &total, &free);
    *total_kb = total / 1024;
    *free_kb  = free  / 1024;
}

/* Native info-screen hooks -- this board runs LVGL now too (eye_ui_lvgl). */
static const eye_ui_hooks_t s_ui = {
    .available = eye_ui_available,
    .show      = eye_ui_info_show,
    .update    = eye_ui_info_update,
};

/* Shared I2C bus for the QMI8658 (confirmed from Waveshare docs: SDA=6, SCL=7;
 * the CST816 touch sits on the same bus but the eye demo doesn't use touch). */
#define IMU_I2C_PORT  I2C_NUM_0
#define IMU_PIN_SDA   6
#define IMU_PIN_SCL   7

static i2c_master_bus_handle_t imu_bus_init(void)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = IMU_I2C_PORT,
        .sda_io_num = IMU_PIN_SDA,
        .scl_io_num = IMU_PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    if (i2c_new_master_bus(&cfg, &bus) != ESP_OK) {
        ESP_LOGW(TAG, "I2C bus init failed; tilt disabled");
        return NULL;
    }
    return bus;
}

/* Mount (formatting on first use) the wear-levelled FATFS clip partition. */
static void anim_fs_mount(void)
{
    static wl_handle_t s_wl = WL_INVALID_HANDLE;
    const esp_vfs_fat_mount_config_t cfg = {
        .max_files              = 4,
        .format_if_mount_failed = true,     /* first boot: raw -> FATFS */
        .allocation_unit_size   = 4096,
    };
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(EYE_CLIP_DIR, "anim",
                                                     &cfg, &s_wl);
    if (err == ESP_OK) {
        eye_video_set_dir(EYE_CLIP_DIR);
        ESP_LOGI(TAG, "anim FATFS mounted at %s (upload clips via the web UI)",
                 EYE_CLIP_DIR);
    } else {
        ESP_LOGW(TAG, "anim FATFS mount failed (%s); video disabled",
                 esp_err_to_name(err));
    }
}

void app_main(void)
{
    eye_webserver_log_init();

    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_ERROR_CHECK(eye_display_init());
    i2c_master_bus_handle_t imu_bus = imu_bus_init();
    eye_display_add_touch(imu_bus);   /* CST816S shares the IMU I2C bus */

    /* No PMIC on this board: battery % is estimated from the GPIO1 ADC. */
    eye_battery_init(NULL);
    eye_info_set_board("LCD 1.28 round 240");

    anim_fs_mount();

    /* WiFi + web control (same app as the AMOLED): the LVGL info screen has
     * the WiFi/brightness controls, and the console (`wifi ap|sta|off`,
     * `bright <pct>`) works as well. */
    const eye_net_config_t net_cfg = {
        .clip_dir           = EYE_CLIP_DIR,
        .brightness_set     = eye_display_set_brightness,
        .get_storage_info   = board_storage_info,
    };
    eye_net_init(&net_cfg);
    eye_console_set_extra(eye_net_register_console);
    eye_info_set_net_provider(eye_net_info_lines);   /* raster fallback only */

    ESP_LOGI(TAG, "1.28\" round eye demo starting");
    eye_demo_run(eye_display_push_frame, imu_bus, &s_ui); /* never returns */
}
