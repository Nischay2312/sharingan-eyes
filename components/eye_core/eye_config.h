#pragma once

/*
 * Board-agnostic configuration for the Sharingan/Rinnegan "animated eye".
 *
 * EVERYTHING the animation draws lives in a fixed 240x240 RGB565 framebuffer.
 * No code that touches this buffer knows or cares which physical panel it ends
 * up on. The only board-specific code is behind eye_display.h
 * (eye_display_init / eye_display_push_frame), implemented per board.
 *
 * 240x240 is deliberate: it is the native resolution of the Waveshare
 * ESP32-S3-Touch-LCD-1.28 (GC9A01, round). On that board push_frame is a 1:1
 * copy. On the AMOLED 2.06 (410x502) push_frame scales this buffer up to the
 * panel width and centers it (see boards/amoled_2_06/main/eye_display_amoled.c).
 */

#include <stdint.h>

/* Logical, board-agnostic framebuffer geometry. */
#define EYE_FB_W   240
#define EYE_FB_H   240
#define EYE_FB_PX  (EYE_FB_W * EYE_FB_H)

/*
 * Pack RGB565 as a native little-endian uint16 fed to an LVGL
 * LV_COLOR_FORMAT_RGB565 image (the byte order the Waveshare BSP display path
 * expects). Producing this here means the eye renders with correct colors with
 * zero driver changes on either board.
 */
static inline uint16_t eye_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

#define EYE_RGB565_BLACK ((uint16_t)0x0000)

/* Effect selection + tunable parameters live in eye_params.h. */
