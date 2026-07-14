idf_component_register(
    SRCS
        "app_main.c"
        "eye_display_amoled.c"      # board-specific frame presenter
        "eye_battery_axp2101.c"     # board's AXP2101 PMIC fuel gauge
    INCLUDE_DIRS
        "."
    REQUIRES
        eye_core      # shared, board-agnostic engine (scenes + imu + loop)
        eye_web       # shared WiFi + web control (clip dir/brightness injected)
        eye_ui_lvgl   # shared native LVGL info screen (eye_ui contract)
        nvs_flash
        driver        # i2c_master for bsp_i2c_get_handle()
        fatfs)        # esp_vfs_fat_info for storage stats callback
# Note: the Waveshare BSP and lvgl are added automatically from idf_component.yml.
