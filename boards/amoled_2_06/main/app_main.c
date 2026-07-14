/*
 * Firmware entry point for the Waveshare ESP32-S3-Touch-AMOLED-2.06.
 *
 * This file is the board "personality": it does board-specific setup (panel +
 * I2C bus via the Waveshare BSP), then hands control to the shared, board-
 * agnostic eye engine. The render loop, animation, and tilt logic all live in
 * components/eye_core so the future 1.28" board reuses them unchanged.
 */

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "nvs_flash.h"

#include "bsp/esp-bsp.h"

#include "eye_display.h"   /* eye_display_init / eye_display_push_frame */
#include "eye_demo.h"      /* eye_demo_run                              */
#include "eye_video.h"     /* eye_video_set_dir                         */
#include "eye_battery.h"   /* AXP2101 fuel gauge (eye_battery_axp2101.c) */
#include "eye_info.h"      /* eye_info_set_board                        */
#include "eye_ui.h"        /* native LVGL info screen (eye_ui_amoled.c) */
#include "eye_net.h"       /* WiFi + web control lifecycle (eye_web)    */
#include "eye_console.h"   /* eye_console_set_extra (wifi/bright cmds)  */
#include "eye_webserver.h" /* eye_webserver_log_init (early log capture) */

static const char *TAG = "sharingan";

/* Native info-screen hooks -- this board draws it with LVGL widgets. Passing
 * these to eye_demo_run also pulls eye_ui_amoled.c into the link. */
static const eye_ui_hooks_t s_ui = {
    .available = eye_ui_available,
    .show      = eye_ui_info_show,
    .update    = eye_ui_info_update,
};

#define EYE_CLIP_DIR    BSP_SD_MOUNT_POINT "/eye"

/* eye_net brightness callback: this board's panel dim goes through the BSP. */
static void board_brightness_set(int pct)
{
    bsp_display_brightness_set(pct);
}

static void board_storage_info(uint64_t *total_kb, uint64_t *free_kb)
{
    uint64_t total = 0, free = 0;
    esp_vfs_fat_info(BSP_SD_MOUNT_POINT, &total, &free);
    *total_kb = total / 1024;
    *free_kb  = free  / 1024;
}

void app_main(void)
{
    /* Hook ESP log output into the web console ring buffer as early as possible
     * so boot messages appear on /dev/console once WiFi is up. */
    eye_webserver_log_init();

    /* Some IDF components expect an initialized NVS; do it defensively. */
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_ERROR_CHECK(eye_display_init());

    /* QMI8658 shares the board I2C bus. The BSP's display bring-up may already
     * have initialized I2C; bsp_i2c_init() is safe/idempotent in that case. */
    (void)bsp_i2c_init();
    i2c_master_bus_handle_t imu_bus = bsp_i2c_get_handle();

    /* The AXP2101 PMIC (battery gauge for the info screen) is on the same bus. */
    eye_battery_init(imu_bus);
    eye_info_set_board("AMOLED 2.06 410x502");

    if (bsp_sdcard_mount() == ESP_OK) {
        eye_video_set_dir(EYE_CLIP_DIR);
        ESP_LOGI(TAG, "SD mounted; clip folder %s", EYE_CLIP_DIR);
    } else {
        ESP_LOGW(TAG, "SD mount failed; video disabled (effects only)");
    }

    const eye_net_config_t net_cfg = {
        .clip_dir           = EYE_CLIP_DIR,
        .brightness_set     = board_brightness_set,
        .get_storage_info   = board_storage_info,
    };
    eye_net_init(&net_cfg);
    eye_console_set_extra(eye_net_register_console);

    ESP_LOGI(TAG, "AMOLED 2.06 eye demo starting");
    eye_demo_run(eye_display_push_frame, imu_bus, &s_ui); /* never returns */
}
