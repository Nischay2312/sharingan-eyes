# Board-agnostic eye engine. Depends only on generic IDF components -- NO BSP --
# so it is reused unchanged by every board firmware under ../../boards/.
idf_component_register(
    SRCS
        "eye_scene.c"
        "eye_info.c"
        "eye_params.c"
        "eye_imu.c"
        "eye_console.c"
        "eye_video.c"
        "eye_clip_params.c"
        "eye_demo.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        driver                      # i2c_master (QMI8658, eye_battery contract)
        esp_timer                   # animation clock + flick timing
        esp_app_format              # esp_app_get_description (info screen)
        console                     # serial command REPL
        nvs_flash                   # save/load tuned parameters
        esp_partition               # read the 'anim' clip partition
        espressif__esp_new_jpeg)    # decode MJPEG clip frames
