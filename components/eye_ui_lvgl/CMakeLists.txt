# Native LVGL info screen, shared by every board that runs esp_lvgl_port
# (AMOLED 2.06 via the Waveshare BSP, LCD 1.28 via its raw esp_lcd backend).
# Locks through lvgl_port_lock -- no BSP dependency. The board's display
# backend must provide eye_display_show_eye(bool).
idf_component_register(
    SRCS
        "eye_ui_lvgl.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        eye_core     # eye_ui.h contract + eye_info status struct
        eye_web)     # WiFi switches / brightness slider drive eye_net
