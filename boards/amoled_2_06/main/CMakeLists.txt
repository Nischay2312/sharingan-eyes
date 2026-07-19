idf_component_register(
    SRCS
        "app_main.c"
        "eye_display_amoled.c"      # board-specific frame presenter
        "eye_battery_axp2101.c"     # board's AXP2101 PMIC fuel gauge
    INCLUDE_DIRS
        "."
    REQUIRES
        eye_core      # shared, board-agnostic engine (scenes + imu + console + loop)
        eye_web       # WiFi + web control (clip dir/brightness/storage injected)
        nvs_flash
        driver        # i2c_master for bsp_i2c_get_handle()
        esp_lcd       # direct panel draw (esp_lcd_panel_draw_bitmap / io callbacks)
        fatfs)        # esp_vfs_fat_info for storage stats callback
# Note: the Waveshare BSP is added automatically from idf_component.yml.
