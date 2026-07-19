/*
 * Firmware entry point for the Waveshare ESP32-S3-Touch-AMOLED-2.06.
 *
 * Board "personality": board-specific setup (panel + I2C via the BSP), then
 * hands control to the shared, board-agnostic eye engine (eye_demo_run).
 *
 * POST FREEZE-FIX ARCHITECTURE
 * ----------------------------
 * The AMOLED silent-freeze was the LVGL panel-flush wedge. The display backend
 * now draws straight to the SH8601 (no LVGL) via a pipelined flush task, so the
 * whole feature set is restored on that foundation:
 *   clips + scene effects + gesture flick (IMU) + serial console + battery +
 *   RASTER info screen + WiFi/web control.
 * The info screen is passed as NULL hooks on purpose -> eye_demo_run renders it
 * with eye_info_render (raster), NOT the old LVGL widget screen (LVGL is gone).
 *
 * Brightness note: web/console brightness goes through eye_display_set_brightness
 * (not bsp_display_brightness_set directly) so the SH8601 0x51 command is
 * serialized with the flush task's pixel DMA on the shared QSPI bus.
 */

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "nvs_flash.h"

#include "bsp/esp-bsp.h"

#include "eye_display.h"   /* eye_display_init / push_frame / set_brightness */
#include "eye_demo.h"      /* eye_demo_run                              */
#include "eye_video.h"     /* eye_video_set_dir                        */
#include "eye_battery.h"   /* AXP2101 fuel gauge (eye_battery_axp2101.c) */
#include "eye_info.h"      /* eye_info_set_board                        */
#include "eye_net.h"       /* WiFi + web control lifecycle (eye_web)    */
#include "eye_console.h"   /* eye_console_set_extra (wifi/bright cmds)  */
#include "eye_webserver.h" /* eye_webserver_log_init (early log capture) */

/* Bus-safe brightness, defined in eye_display_amoled.c (serializes the SH8601
 * 0x51 command with the flush task on the shared QSPI bus). */
void eye_display_set_brightness(int pct);

static const char *TAG = "sharingan";

#define EYE_CLIP_DIR    BSP_SD_MOUNT_POINT "/eye"

static void board_brightness_set(int pct)
{
    eye_display_set_brightness(pct);
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
    /* Capture boot logs into the web console ring buffer as early as possible. */
    eye_webserver_log_init();

    /* Some IDF components expect an initialized NVS; do it defensively. */
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_ERROR_CHECK(eye_display_init());

    /* QMI8658 (gesture) shares the board I2C bus. bsp_i2c_init() is idempotent
     * if the BSP display bring-up already initialized it. */
    (void)bsp_i2c_init();
    i2c_master_bus_handle_t imu_bus = bsp_i2c_get_handle();

    /* AXP2101 PMIC (battery gauge for the info screen) is on the same bus. */
    eye_battery_init(imu_bus);
    eye_info_set_board("AMOLED 2.06 410x502");

    if (bsp_sdcard_mount() == ESP_OK) {
        eye_video_set_dir(EYE_CLIP_DIR);
        ESP_LOGI(TAG, "SD mounted; clip folder %s", EYE_CLIP_DIR);
    } else {
        ESP_LOGW(TAG, "SD mount failed; video disabled (effects only)");
    }

    const eye_net_config_t net_cfg = {
        .clip_dir         = EYE_CLIP_DIR,
        .brightness_set   = board_brightness_set,
        .get_storage_info = board_storage_info,
    };
    eye_net_init(&net_cfg);
    eye_console_set_extra(eye_net_register_console);

    ESP_LOGI(TAG, "AMOLED 2.06 eye engine starting (full: web + raster info)");

    /* NULL UI hooks -> raster info screen (no LVGL). Never returns. */
    eye_demo_run(eye_display_push_frame, imu_bus, NULL);
}
