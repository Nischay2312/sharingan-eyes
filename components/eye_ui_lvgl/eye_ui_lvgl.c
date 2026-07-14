/*
 * Native (LVGL) info screen, shared by both boards.
 *
 * Formerly boards/amoled_2_06/main/eye_ui_amoled.c; generalized by locking
 * through esp_lvgl_port (lvgl_port_lock) instead of the Waveshare BSP, so the
 * same widgets run on the AMOLED (whose bsp_display_lock is a thin wrapper
 * around the same lock) and on the LCD 1.28's raw esp_lcd + lvgl_port backend.
 *
 * eye_core drives this through the eye_ui.h contract: when the info screen is
 * the active screen it calls eye_ui_info_show(true) (which hides the eye image
 * and reveals these widgets) and eye_ui_info_update() ~1 Hz; leaving the screen
 * calls eye_ui_info_show(false) to bring the eye back. The WiFi switches and
 * the brightness slider only set eye_net's DESIRED state (non-blocking).
 */

#include "eye_ui.h"

#include <stdio.h>

#include "lvgl.h"
#include "esp_lvgl_port.h"

#include "eye_net.h"
#include "eye_webserver.h"

/* Provided by each board's display backend -- hides/shows the eye image. */
void eye_display_show_eye(bool show);

/* Sharingan palette. */
#define CLR_BG      0x000000
#define CLR_CARD    0x140306
#define CLR_ACCENT  0xE0141B   /* dojutsu red      */
#define CLR_TEXT    0xE8E6EA
#define CLR_MUTE    0x8A8490
#define CLR_TRACK   0x2A2030   /* arc background   */
#define CLR_OK      0x46DC5A
#define CLR_WARN    0xFFAA28
#define CLR_BAD     0xFF3C32

static bool       s_built;
static lv_obj_t  *s_root;
static lv_obj_t  *s_arc;
static lv_obj_t  *s_pct;      /* big battery %      */
static lv_obj_t  *s_volt;     /* voltage / status   */
static lv_obj_t  *s_details;  /* fw / build / board */
static lv_obj_t  *s_footer;   /* counts + uptime    */
static lv_obj_t  *s_sw_wifi;  /* WiFi on/off        */
static lv_obj_t  *s_sw_mode;  /* off=AP, on=STA     */
static lv_obj_t  *s_wifi_lbl; /* wifi status text   */
static lv_obj_t  *s_bright;   /* brightness slider  */
static lv_obj_t  *s_load;       /* progress overlay (upload + OTA)  */
static lv_obj_t  *s_load_title; /* overlay title text (dynamic)     */
static lv_obj_t  *s_load_bar;
static lv_obj_t  *s_load_pct;
static bool       s_small;    /* <=280px panel (the round 240 LCD) */

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font,
                            uint32_t color, lv_text_align_t align)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, align, 0);
    return l;
}

/* ---- control callbacks: only set DESIRED state; eye_net's task acts ---- */

static void on_wifi_switch(lv_event_t *e)
{
    (void)e;
    eye_net_set_enabled(lv_obj_has_state(s_sw_wifi, LV_STATE_CHECKED));
}

static void on_mode_switch(lv_event_t *e)
{
    (void)e;
    eye_net_set_mode(lv_obj_has_state(s_sw_mode, LV_STATE_CHECKED)
                     ? EYE_NET_STA : EYE_NET_AP);
}

static void on_brightness(lv_event_t *e)
{
    (void)e;
    eye_net_set_brightness((int)lv_slider_get_value(s_bright));
}

/* A titled row with a switch on the right. */
static lv_obj_t *make_switch_row(lv_obj_t *parent, const char *text,
                                 lv_event_cb_t cb)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 4, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *l = make_label(row, &lv_font_montserrat_14, CLR_TEXT, LV_TEXT_ALIGN_LEFT);
    lv_label_set_text(l, text);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 6, 0);

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 52, 28);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -6, 0);
    lv_obj_set_style_bg_color(sw, lv_color_hex(CLR_ACCENT),
                              LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return sw;
}

/* Full-screen progress overlay: used for both clip uploads and OTA updates. */
static void build_overlay(void)
{
    s_load = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_load, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_load, lv_color_hex(CLR_BG), 0);
    lv_obj_set_style_bg_opa(s_load, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_load, 0, 0);
    lv_obj_set_style_radius(s_load, 0, 0);
    lv_obj_clear_flag(s_load, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_load, LV_OBJ_FLAG_HIDDEN);

    s_load_title = make_label(s_load, &lv_font_montserrat_14, CLR_TEXT,
                              LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(s_load_title, "");
    lv_obj_align(s_load_title, LV_ALIGN_CENTER, 0, -26);

    s_load_bar = lv_bar_create(s_load);
    lv_obj_set_size(s_load_bar, LV_PCT(55), 12);
    lv_obj_align(s_load_bar, LV_ALIGN_CENTER, 0, 4);
    lv_bar_set_range(s_load_bar, 0, 100);
    lv_obj_set_style_bg_color(s_load_bar, lv_color_hex(CLR_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_load_bar, lv_color_hex(CLR_ACCENT), LV_PART_INDICATOR);

    s_load_pct = make_label(s_load, &lv_font_montserrat_20, CLR_ACCENT,
                            LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(s_load_pct, "0%");
    lv_obj_align(s_load_pct, LV_ALIGN_CENTER, 0, 32);
}

/* Show (pct 0..100) or hide (pct < 0) the overlay with a dynamic title. */
static void show_overlay(const char *title, int pct)
{
    lvgl_port_lock(0);
    if (s_load == NULL) build_overlay();
    if (pct < 0) {
        lv_obj_add_flag(s_load, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(s_load_title, title);
        lv_bar_set_value(s_load_bar, pct > 100 ? 100 : pct, LV_ANIM_OFF);
        lv_label_set_text_fmt(s_load_pct, "%d%%", pct > 100 ? 100 : pct);
        lv_obj_remove_flag(s_load, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_load);
    }
    lvgl_port_unlock();
}

/* eye_webserver hooks: 0..100 while active, -1 to hide. Run on the httpd task. */
static void upload_progress(int pct)
{
    show_overlay(LV_SYMBOL_DOWNLOAD "  Loading animation", pct);
}
static void ota_progress(int pct)
{
    show_overlay(LV_SYMBOL_REFRESH "  Updating firmware", pct);
}

static void build(void)
{
    /* The round 240px LCD needs a tighter layout than the 410x502 AMOLED --
     * mainly a smaller battery ring. */
    s_small = lv_display_get_horizontal_resolution(NULL) <= 280 ||
              lv_display_get_vertical_resolution(NULL) <= 280;

    s_root = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_root, lv_color_hex(CLR_BG), 0);
    lv_obj_set_style_bg_opa(s_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_root, 0, 0);
    lv_obj_set_style_pad_all(s_root, 0, 0);
    lv_obj_set_style_radius(s_root, 0, 0);
    /* Scrollable: the WiFi + brightness card lives below the fold. */
    lv_obj_set_scroll_dir(s_root, LV_DIR_VER);
    lv_obj_set_flex_flow(s_root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_root, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_root, s_small ? 8 : 14, 0);
    lv_obj_set_style_pad_ver(s_root, s_small ? 26 : 22, 0);

    /* Title */
    lv_obj_t *title = make_label(s_root, &lv_font_montserrat_28, CLR_ACCENT,
                                 LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(title, "SHARINGAN");
    lv_obj_set_style_text_letter_space(title, 4, 0);

    /* Battery ring: a non-interactive arc with the % + voltage stacked in the
     * middle -- reads like a watch face. */
    const int arc_dim = s_small ? 128 : 210;
    const int arc_w   = s_small ? 10 : 14;
    s_arc = lv_arc_create(s_root);
    lv_obj_set_size(s_arc, arc_dim, arc_dim);
    lv_arc_set_bg_angles(s_arc, 135, 45);
    lv_arc_set_range(s_arc, 0, 100);
    lv_arc_set_value(s_arc, 0);
    lv_obj_remove_style(s_arc, NULL, LV_PART_KNOB);      /* no draggable knob */
    lv_obj_clear_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_arc, arc_w, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(CLR_TRACK), LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(s_arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, arc_w, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(CLR_OK), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_arc, true, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_arc, LV_OPA_TRANSP, LV_PART_MAIN);

    s_pct = make_label(s_arc, s_small ? &lv_font_montserrat_20 : &lv_font_montserrat_28,
                       CLR_TEXT, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(s_pct, "--");
    lv_obj_align(s_pct, LV_ALIGN_CENTER, 0, s_small ? -9 : -12);

    s_volt = make_label(s_arc, &lv_font_montserrat_14, CLR_MUTE, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(s_volt, "");
    lv_obj_align(s_volt, LV_ALIGN_CENTER, 0, s_small ? 14 : 20);

    /* Firmware / build / board */
    s_details = make_label(s_root, &lv_font_montserrat_14, CLR_MUTE, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(s_details, "");
    lv_obj_set_style_text_line_space(s_details, 4, 0);

    /* Counts + uptime + hint */
    s_footer = make_label(s_root, &lv_font_montserrat_14, CLR_TEXT, LV_TEXT_ALIGN_CENTER);
    lv_label_set_text(s_footer, "");
    lv_obj_set_style_text_line_space(s_footer, 4, 0);

    /* ---- WiFi + brightness card (scroll down to reach) ---- */
    lv_obj_t *card = lv_obj_create(s_root);
    lv_obj_set_size(card, LV_PCT(88), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, lv_color_hex(CLR_CARD), 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 18, 0);
    lv_obj_set_style_pad_all(card, 10, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 2, 0);

    lv_obj_t *t = make_label(card, &lv_font_montserrat_14, CLR_ACCENT, LV_TEXT_ALIGN_LEFT);
    lv_label_set_text(t, LV_SYMBOL_WIFI "  Web control");

    s_sw_wifi = make_switch_row(card, "WiFi", on_wifi_switch);
    s_sw_mode = make_switch_row(card, "AP  /  STA", on_mode_switch);

    s_wifi_lbl = make_label(card, &lv_font_montserrat_14, CLR_MUTE, LV_TEXT_ALIGN_LEFT);
    lv_label_set_text(s_wifi_lbl, "off");
    lv_obj_set_width(s_wifi_lbl, LV_PCT(100));
    lv_label_set_long_mode(s_wifi_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(s_wifi_lbl, 3, 0);

    lv_obj_t *bt = make_label(card, &lv_font_montserrat_14, CLR_TEXT, LV_TEXT_ALIGN_LEFT);
    lv_label_set_text(bt, LV_SYMBOL_EYE_OPEN "  Brightness");
    lv_obj_set_style_pad_top(bt, 8, 0);

    s_bright = lv_slider_create(card);
    lv_obj_set_size(s_bright, LV_PCT(94), 14);
    lv_obj_set_style_margin_ver(s_bright, 8, 0);
    lv_slider_set_range(s_bright, 5, 100);
    lv_slider_set_value(s_bright, eye_net_brightness(), LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_bright, lv_color_hex(CLR_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bright, lv_color_hex(CLR_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_bright, lv_color_hex(CLR_TEXT), LV_PART_KNOB);
    lv_obj_add_event_cb(s_bright, on_brightness, LV_EVENT_RELEASED, NULL);

    s_built = true;
}

/* WiFi status -> one short label under the sliders. */
static void wifi_label_refresh(void)
{
    eye_net_status_t n;
    eye_net_get_status(&n);
    switch (n.state) {
    case EYE_NET_AP_UP:
        lv_label_set_text_fmt(s_wifi_lbl, "AP: %s\npass: %s\n%s\n(%s)",
                              n.ssid, n.pass, n.url, n.ip);
        break;
    case EYE_NET_STA_UP:
        lv_label_set_text_fmt(s_wifi_lbl, "%s  (%d dBm)\n%s\n(%s)",
                              n.ssid, n.rssi, n.url, n.ip);
        break;
    case EYE_NET_STA_CONNECTING:
        lv_label_set_text(s_wifi_lbl, "connecting...");
        break;
    case EYE_NET_PORTAL:
        lv_label_set_text_fmt(s_wifi_lbl, "setup: join '%s'\nthen open %s",
                              n.ssid, n.ip);
        break;
    case EYE_NET_STARTING:
        lv_label_set_text(s_wifi_lbl, "starting...");
        break;
    case EYE_NET_FAILED:
        lv_label_set_text(s_wifi_lbl, "failed -- toggle to retry");
        break;
    default:
        lv_label_set_text(s_wifi_lbl, "off");
        break;
    }

    /* Mirror desired state into the switches (no events fire from these). */
    if (n.enabled) {
        lv_obj_add_state(s_sw_wifi, LV_STATE_CHECKED);
    } else {
        lv_obj_clear_state(s_sw_wifi, LV_STATE_CHECKED);
    }
    if (n.mode == EYE_NET_STA) {
        lv_obj_add_state(s_sw_mode, LV_STATE_CHECKED);
    } else {
        lv_obj_clear_state(s_sw_mode, LV_STATE_CHECKED);
    }
    if (!lv_obj_has_state(s_bright, LV_STATE_PRESSED)) {
        lv_slider_set_value(s_bright, eye_net_brightness(), LV_ANIM_OFF);
    }
}

/* ------------------------------------------------------------ contract -- */

bool eye_ui_available(void)
{
    eye_webserver_set_upload_cb(upload_progress);
    eye_webserver_set_ota_cb(ota_progress);
    return true;
}

void eye_ui_info_show(bool show)
{
    lvgl_port_lock(0);
    if (!s_built) {
        build();
    }
    if (show) {
        lv_obj_remove_flag(s_root, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_root);
    } else {
        lv_obj_add_flag(s_root, LV_OBJ_FLAG_HIDDEN);
    }
    lvgl_port_unlock();

    /* Toggle the eye image opposite to the info screen. */
    eye_display_show_eye(!show);
}

void eye_ui_info_update(const eye_info_status_t *st)
{
    if (st == NULL) {
        return;
    }

    lvgl_port_lock(0);
    if (!s_built) {
        build();
    }

    if (st->bat_valid && st->bat.present) {
        int pct = st->bat.pct;
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        uint32_t c = pct > 40 ? CLR_OK : (pct > 15 ? CLR_WARN : CLR_BAD);
        lv_arc_set_value(s_arc, pct);
        lv_obj_set_style_arc_color(s_arc, lv_color_hex(c), LV_PART_INDICATOR);
        lv_label_set_text_fmt(s_pct, "%d%%", pct);
        lv_obj_set_style_text_color(s_pct, lv_color_hex(c), 0);
        lv_label_set_text_fmt(s_volt, "%d.%02d V%s",
                              st->bat.mv / 1000, (st->bat.mv % 1000) / 10,
                              st->bat.charging ? "  " LV_SYMBOL_CHARGE
                              : (st->bat.vbus ? "  " LV_SYMBOL_USB : ""));
    } else {
        lv_arc_set_value(s_arc, 0);
        lv_obj_set_style_arc_color(s_arc, lv_color_hex(CLR_TRACK), LV_PART_INDICATOR);
        lv_label_set_text(s_pct, LV_SYMBOL_USB);
        lv_obj_set_style_text_color(s_pct, lv_color_hex(CLR_MUTE), 0);
        lv_label_set_text(s_volt, (st->bat_valid && st->bat.vbus) ? "USB power" : "no battery");
    }

    lv_label_set_text_fmt(s_details, "FW %s\n%s  %s\nIDF %s\n%s",
                          st->fw_version, st->build_date, st->build_time,
                          st->idf_version, st->board);

    uint32_t s = st->uptime_s;
    lv_label_set_text_fmt(s_footer, "%d clips  %d scenes\nup %02u:%02u:%02u\n"
                          LV_SYMBOL_LEFT "  flick  " LV_SYMBOL_RIGHT,
                          st->clips, st->scenes,
                          (unsigned)(s / 3600), (unsigned)((s / 60) % 60),
                          (unsigned)(s % 60));

    wifi_label_refresh();

    lvgl_port_unlock();
}
