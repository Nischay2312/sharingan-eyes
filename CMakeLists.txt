# ESP-IDF project root for the Sharingan animated-eye firmware.
#
# This single project builds ONE board's firmware at a time. To SWITCH BOARDS,
# just edit the one word in `board.txt` and rebuild -- no flags, no re-importing.
# Board-specific code, Kconfig defaults, and partition table live under
# boards/<board>/, so each board's firmware stays cleanly separated.
#
# Selection precedence (first that is set wins):
#   1. -DSHARINGAN_BOARD=<name>   (one-off override on the idf.py/cmake line)
#   2. $ENV{SHARINGAN_BOARD}      (environment variable)
#   3. board.txt                  (the easy way -- edit this file)
#   4. amoled_2_06                (fallback default)
cmake_minimum_required(VERSION 3.16)

# Reconfigure automatically whenever board.txt is edited.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/board.txt")

if(NOT DEFINED SHARINGAN_BOARD)
    if(DEFINED ENV{SHARINGAN_BOARD})
        set(SHARINGAN_BOARD "$ENV{SHARINGAN_BOARD}")
    elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/board.txt")
        # Use the first non-empty, non-comment line of board.txt.
        file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/board.txt" _board_lines)
        foreach(_line IN LISTS _board_lines)
            string(STRIP "${_line}" _line)
            if(_line AND NOT _line MATCHES "^#")
                set(SHARINGAN_BOARD "${_line}")
                break()
            endif()
        endforeach()
    endif()
endif()
if(NOT DEFINED SHARINGAN_BOARD OR SHARINGAN_BOARD STREQUAL "")
    set(SHARINGAN_BOARD "amoled_2_06")
endif()
message(STATUS "Sharingan board: ${SHARINGAN_BOARD}  (edit board.txt to switch)")

set(_board_dir "${CMAKE_CURRENT_LIST_DIR}/boards/${SHARINGAN_BOARD}")
if(NOT EXISTS "${_board_dir}/main/CMakeLists.txt")
    message(FATAL_ERROR
        "Unknown SHARINGAN_BOARD '${SHARINGAN_BOARD}': no ${_board_dir}/main")
endif()

# Shared board-agnostic engine + the selected board's firmware component ("main").
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/components"
    "${_board_dir}/main")

# Per-board Kconfig defaults (PSRAM mode, LVGL, partition-table path).
set(SDKCONFIG_DEFAULTS "${_board_dir}/sdkconfig.defaults")

# Keep each board's generated config separate so switching boards is clean.
set(SDKCONFIG "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.${SHARINGAN_BOARD}")

# Both supported boards are ESP32-S3. Pin the target so a fresh configure from an
# IDE that didn't run `idf.py set-target esp32s3` doesn't default to plain esp32
# (which makes the esp32s3-only Waveshare BSP fail dependency solving).
if(NOT DEFINED IDF_TARGET AND NOT DEFINED ENV{IDF_TARGET})
    set(IDF_TARGET "esp32s3" CACHE STRING "ESP-IDF build target")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(sharingan_eye)
