idf_component_register(
    SRCS
        "app_main.c"
        "eye_display_gc9a01.c"      # board-specific 1:1 frame presenter + PWM BL
        "eye_battery_adc.c"         # battery voltage via GPIO1 ADC (/3 divider)
    INCLUDE_DIRS
        "."
    REQUIRES
        eye_core      # shared, board-agnostic engine
        eye_web       # shared WiFi + web control (clip dir/brightness injected)
        eye_ui_lvgl   # shared native LVGL info screen (eye_ui contract)
        nvs_flash
        driver        # i2c_master + spi_master + ledc backlight
        esp_adc       # battery voltage sampling + calibration
        fatfs         # FATFS on the 'anim' flash partition (clip storage)
        wear_levelling
        esp_lcd)      # GC9A01 panel ops (driver added via idf_component.yml)
