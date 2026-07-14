#include "eye_info.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_timer.h"

#include "eye_config.h"
#include "eye_font.h"
#include "eye_battery.h"
#include "eye_video.h"
#include "eye_params.h"

static const char *s_board = "unknown board";

/* Battery is polled at most once a second; the cached copy is what we draw. */
static eye_battery_t s_bat;
static bool          s_bat_valid;
static int64_t       s_bat_next_us;

void eye_info_set_board(const char *name)
{
    if (name != NULL) {
        s_board = name;
    }
}

static eye_info_net_fn s_net_fn;

void eye_info_set_net_provider(eye_info_net_fn fn)
{
    s_net_fn = fn;
}

void eye_info_gather(eye_info_status_t *st, uint32_t t_ms)
{
    if (st == NULL) {
        return;
    }

    /* Refresh the cached battery reading at 1 Hz -- cheap to call every frame. */
    int64_t now = esp_timer_get_time();
    if (now >= s_bat_next_us) {
        s_bat_valid   = eye_battery_read(&s_bat);
        s_bat_next_us = now + 1000000;
    }

    const esp_app_desc_t *app = esp_app_get_description();
    st->board       = s_board;
    st->fw_version  = app->version;
    st->build_date  = app->date;
    st->build_time  = app->time;
    st->idf_version = app->idf_ver;
    st->bat         = s_bat;
    st->bat_valid   = s_bat_valid;
    st->clips       = eye_video_count();
    st->scenes      = EYE_SCENE_COUNT;
    st->uptime_s    = t_ms / 1000;
}

/* ---------------------------------------------------------------- drawing */

static inline void px(uint16_t *fb, int x, int y, uint16_t c)
{
    if ((unsigned)x < EYE_FB_W && (unsigned)y < EYE_FB_H) {
        fb[y * EYE_FB_W + x] = c;
    }
}

static void fill_rect(uint16_t *fb, int x, int y, int w, int h, uint16_t c)
{
    for (int j = y; j < y + h; j++) {
        for (int i = x; i < x + w; i++) {
            px(fb, i, j, c);
        }
    }
}

/* Draw one glyph scaled by `s` (1 or 2). Returns the advance in px. */
static int draw_char(uint16_t *fb, int x, int y, char ch, int s, uint16_t c)
{
    unsigned uc = (unsigned char)ch;
    if (uc < EYE_FONT_FIRST || uc > EYE_FONT_LAST) {
        uc = ' ';
    }
    const uint8_t *g = eye_font[uc - EYE_FONT_FIRST];
    for (int row = 0; row < EYE_FONT_H; row++) {
        uint8_t bits = g[row];
        if (bits == 0) {
            continue;
        }
        for (int col = 0; col < EYE_FONT_W; col++) {
            if (bits & (0x80 >> col)) {
                if (s == 1) {
                    px(fb, x + col, y + row, c);
                } else {
                    fill_rect(fb, x + col * s, y + row * s, s, s, c);
                }
            }
        }
    }
    return EYE_FONT_W * s;
}

static void draw_text(uint16_t *fb, int x, int y, const char *t, int s, uint16_t c)
{
    while (*t) {
        x += draw_char(fb, x, y, *t++, s, c);
    }
}

/* Horizontally centred text (the display is a 240 px circle). */
static void draw_text_c(uint16_t *fb, int y, const char *t, int s, uint16_t c)
{
    int w = (int)strlen(t) * EYE_FONT_W * s;
    draw_text(fb, (EYE_FB_W - w) / 2, y, t, s, c);
}

/* ------------------------------------------------------------ the screen */

void eye_info_render(uint16_t *fb, uint32_t t_ms)
{
    eye_info_status_t st;
    eye_info_gather(&st, t_ms);

    const uint16_t col_bg    = eye_rgb565(0, 0, 0);
    const uint16_t col_title = eye_rgb565(255, 32, 32);
    const uint16_t col_text  = eye_rgb565(200, 200, 210);
    const uint16_t col_dim   = eye_rgb565(120, 120, 132);
    const uint16_t col_ok    = eye_rgb565(70, 220, 90);
    const uint16_t col_warn  = eye_rgb565(255, 170, 40);
    const uint16_t col_bad   = eye_rgb565(255, 60, 50);

    for (int i = 0; i < EYE_FB_PX; i++) {
        fb[i] = col_bg;
    }

    /* Thin dark-red ring at the rim -- a nod to the eye, and it marks the
     * visible circle edge on the round panel. */
    const int cx = EYE_FB_W / 2, cy = EYE_FB_H / 2;
    for (int y = 0; y < EYE_FB_H; y++) {
        for (int x = 0; x < EYE_FB_W; x++) {
            int dx = x - cx, dy = y - cy;
            int r2 = dx * dx + dy * dy;
            if (r2 >= 113 * 113 && r2 <= 118 * 118) {
                px(fb, x, y, eye_rgb565(110, 8, 8));
            }
        }
    }

    char line[40];

    draw_text_c(fb, 26, "SHARINGAN", 2, col_title);

    snprintf(line, sizeof(line), "FW %.20s", st.fw_version);
    draw_text_c(fb, 58, line, 1, col_text);

    snprintf(line, sizeof(line), "%.12s %.8s", st.build_date, st.build_time);
    draw_text_c(fb, 72, line, 1, col_dim);

    snprintf(line, sizeof(line), "IDF %.14s", st.idf_version);
    draw_text_c(fb, 86, line, 1, col_dim);

    draw_text_c(fb, 100, st.board, 1, col_text);

    /* ---- battery ---- */
    if (st.bat_valid && st.bat.present) {
        uint16_t fillc = st.bat.pct > 40 ? col_ok : (st.bat.pct > 15 ? col_warn : col_bad);

        /* Bar: 100x16 body + nub, centred, filled by pct. */
        const int bw = 100, bh = 16;
        const int bx = (EYE_FB_W - bw) / 2, by = 122;
        fill_rect(fb, bx - 1, by - 1, bw + 2, bh + 2, col_dim);       /* frame */
        fill_rect(fb, bx, by, bw, bh, col_bg);                        /* body  */
        fill_rect(fb, bx + bw + 1, by + 4, 4, bh - 8, col_dim);       /* nub   */
        int fw = st.bat.pct * (bw - 4) / 100;
        if (fw > 0) {
            fill_rect(fb, bx + 2, by + 2, fw, bh - 4, fillc);
        }

        snprintf(line, sizeof(line), "%d%%  %d.%02dV%s%s",
                 st.bat.pct, st.bat.mv / 1000, (st.bat.mv % 1000) / 10,
                 st.bat.charging ? "  CHG" : "",
                 (!st.bat.charging && st.bat.vbus) ? "  USB" : "");
        draw_text_c(fb, 146, line, 1, fillc);
    } else {
        draw_text_c(fb, 130, st.bat_valid && st.bat.vbus ? "USB power" : "no battery", 1, col_dim);
    }

    /* ---- content + uptime ---- */
    snprintf(line, sizeof(line), "clips %d  scenes %d", st.clips, st.scenes);
    draw_text_c(fb, 162, line, 1, col_text);

    uint32_t s = st.uptime_s;
    snprintf(line, sizeof(line), "up %02u:%02u:%02u",
             (unsigned)(s / 3600), (unsigned)((s / 60) % 60), (unsigned)(s % 60));
    draw_text_c(fb, 176, line, 1, col_dim);

    /* ---- network status (when the board wired eye_net in) ---- */
    char l1[26] = "", l2[26] = "";
    if (s_net_fn != NULL) {
        s_net_fn(l1, sizeof(l1), l2, sizeof(l2));
    }
    if (l1[0] != '\0') {
        draw_text_c(fb, 192, l1, 1, col_title);
        if (l2[0] != '\0') {
            draw_text_c(fb, 206, l2, 1, col_text);
        }
    } else {
        draw_text_c(fb, 198, "flick > next", 1, col_dim);
    }
}
