#include "eye_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

#include "wifi_ctrl.h"
#include "wifi_portal.h"
#include "eye_webserver.h"
#include "eye_sync.h"

static const char *TAG = "eye_net";

#define NVS_NS        "eyenet"
#define KEY_BRIGHT    "bright"
#define PORTAL_SSID   "SharinganEye-Setup"

static eye_net_config_t        s_cfg;
/* WiFi comes up at boot: on + AUTO (STA if creds, else control SoftAP). */
static volatile bool           s_want_on   = true;
static volatile eye_net_mode_t s_want_mode = EYE_NET_AUTO;
static volatile bool           s_reprovision;   /* creds changed -> reconnect */
static bool                    s_cur_on;
static eye_net_mode_t          s_cur_mode;
static volatile eye_net_state_t s_state = EYE_NET_OFF;
static bool                    s_portal_up;
static char                    s_ap_ssid[33];
static char                    s_ap_pass[17];
static int                     s_bright = 80;
static volatile int64_t        s_last_activity;

void eye_net_note_activity(void)
{
    s_last_activity = esp_timer_get_time();
}

void eye_net_set_enabled(bool on)
{
    s_want_on = on;
    eye_net_note_activity();
}

void eye_net_set_mode(eye_net_mode_t mode)
{
    s_want_mode = mode;
    eye_net_note_activity();
}

const char *eye_net_clip_dir(void)
{
    return s_cfg.clip_dir ? s_cfg.clip_dir : "/eye";
}

bool eye_net_storage_info(uint64_t *total_kb, uint64_t *free_kb)
{
    if (!s_cfg.get_storage_info) return false;
    s_cfg.get_storage_info(total_kb, free_kb);
    return true;
}

void eye_net_get_status(eye_net_status_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->enabled = s_want_on;
    out->mode    = s_want_mode;
    out->state   = s_state;
    snprintf(out->url, sizeof(out->url), "http://sharingan-eye.local");
    switch (s_state) {
    case EYE_NET_AP_UP:
        snprintf(out->ssid, sizeof(out->ssid), "%s", s_ap_ssid);
        snprintf(out->pass, sizeof(out->pass), "%s", s_ap_pass);
        snprintf(out->ip, sizeof(out->ip), "%s", wifi_ctrl_ip());
        break;
    case EYE_NET_PORTAL:
        snprintf(out->ssid, sizeof(out->ssid), "%s", PORTAL_SSID);
        snprintf(out->ip, sizeof(out->ip), "192.168.4.1");
        break;
    case EYE_NET_STA_UP:
    case EYE_NET_STA_CONNECTING:
        snprintf(out->ssid, sizeof(out->ssid), "%s", wifi_ctrl_sta_ssid());
        snprintf(out->ip, sizeof(out->ip), "%s", wifi_ctrl_ip());
        out->rssi = wifi_ctrl_rssi();
        break;
    default:
        break;
    }
}

void eye_net_info_lines(char *l1, size_t n1, char *l2, size_t n2)
{
    if (l1 == NULL || l2 == NULL) {
        return;
    }
    l1[0] = l2[0] = '\0';
    switch (s_state) {
    case EYE_NET_AP_UP:
        snprintf(l1, n1, "AP %s", s_ap_ssid);
        snprintf(l2, n2, "pw %s", s_ap_pass);
        break;
    case EYE_NET_STA_UP:
        snprintf(l1, n1, "%s", wifi_ctrl_sta_ssid());
        snprintf(l2, n2, "%s", wifi_ctrl_ip());
        break;
    case EYE_NET_STA_CONNECTING:
        snprintf(l1, n1, "WiFi connecting...");
        break;
    case EYE_NET_PORTAL:
        snprintf(l1, n1, "setup: join AP");
        snprintf(l2, n2, "%s", PORTAL_SSID);
        break;
    case EYE_NET_STARTING:
        snprintf(l1, n1, "WiFi starting...");
        break;
    case EYE_NET_FAILED:
        snprintf(l1, n1, "WiFi failed");
        break;
    default:
        break;      /* off: leave both empty -> info screen shows its hint */
    }
}

int eye_net_brightness(void)
{
    return s_bright;
}

void eye_net_set_brightness(int pct)
{
    if (pct < 5) pct = 5;
    if (pct > 100) pct = 100;
    s_bright = pct;
    if (s_cfg.brightness_set) {
        s_cfg.brightness_set(pct);
    }
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_i32(nvs, KEY_BRIGHT, pct);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}

esp_err_t eye_net_forget_wifi(void)
{
    return wifi_ctrl_forget_credentials();
}

esp_err_t eye_net_provision(const char *ssid, const char *pass)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = wifi_ctrl_save_credentials(ssid, pass ? pass : "");
    if (err == ESP_OK) {
        s_want_mode   = EYE_NET_AUTO;
        s_want_on     = true;
        s_reprovision = true;   /* net_task tears down + reconnects as STA */
        eye_net_note_activity();
    }
    return err;
}

/* ---------------------------------------------------------- lifecycle -- */

static void teardown(void)
{
    eye_sync_deinit();
    eye_webserver_stop();
    if (s_portal_up) {
        wifi_portal_stop();
        s_portal_up = false;
    }
    wifi_ctrl_stop();
    s_state = EYE_NET_OFF;
}

/* WPA2 control SoftAP named SharinganEye-<mac4> (unique per board, so two eyes
 * never clash) + web UI + ESP-NOW sync. */
static void start_control_ap(void)
{
    char sfx[8];
    wifi_ctrl_mac_suffix(sfx, sizeof(sfx));
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "SharinganEye-%s", sfx);
    snprintf(s_ap_pass, sizeof(s_ap_pass), "uchiha%s", sfx);   /* >= 8 chars */
    if (wifi_ctrl_start_ap(s_ap_ssid, s_ap_pass, false) == ESP_OK) {
        eye_webserver_start();
        eye_sync_init();
        s_state = EYE_NET_AP_UP;
    } else {
        s_state = EYE_NET_FAILED;
    }
}

static void bring_up(void)
{
    s_state = EYE_NET_STARTING;
    if (wifi_ctrl_init() != ESP_OK) {
        s_state = EYE_NET_FAILED;
        return;
    }

    /* STA first in both STA and AUTO when we have creds. */
    if ((s_cur_mode == EYE_NET_STA || s_cur_mode == EYE_NET_AUTO) &&
        wifi_ctrl_has_credentials()) {
        s_state = EYE_NET_STA_CONNECTING;
        if (wifi_ctrl_start_sta() == ESP_OK) {
            eye_webserver_start();
            eye_sync_init();
            s_state = EYE_NET_STA_UP;
            return;
        }
        ESP_LOGW(TAG, "STA connect failed -> fallback");
        wifi_ctrl_stop();
        vTaskDelay(pdMS_TO_TICKS(150));
    }

    /* No working STA. Explicit STA mode opens the captive portal to capture
     * creds; AUTO/AP open the full control SoftAP (which can also provision a
     * network from its System tab). */
    if (s_cur_mode == EYE_NET_STA) {
        if (wifi_portal_start(PORTAL_SSID) == ESP_OK) {
            s_portal_up = true;
            s_state = EYE_NET_PORTAL;
        } else {
            s_state = EYE_NET_FAILED;
        }
        return;
    }

    start_control_ap();
}

static void net_task(void *arg)
{
    (void)arg;
    for (;;) {
        bool           want_on   = s_want_on;
        eye_net_mode_t want_mode = s_want_mode;

        if (want_on != s_cur_on || (want_on && want_mode != s_cur_mode)) {
            if (s_cur_on) {
                teardown();
            }
            s_cur_on   = want_on;
            s_cur_mode = want_mode;
            if (want_on) {
                eye_net_note_activity();
                bring_up();
            }
        }

        /* Portal finished OR the web UI provisioned a network: creds saved ->
         * restart (AUTO -> STA). */
        if ((s_state == EYE_NET_PORTAL && wifi_portal_credentials_received()) ||
            (s_reprovision && s_cur_on)) {
            s_reprovision = false;
            vTaskDelay(pdMS_TO_TICKS(1500));   /* let the 'saved' page flush */
            teardown();
            vTaskDelay(pdMS_TO_TICKS(200));
            bring_up();
        }

        /* WiFi is intentionally kept on (no idle auto-off): it is the only way
         * to reach the eye once it is in the mask, and the sync mesh needs it. */

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

esp_err_t eye_net_init(const eye_net_config_t *cfg)
{
    if (cfg != NULL) {
        s_cfg = *cfg;
    }

    nvs_handle_t nvs;
    if (nvs_open(NVS_NS, NVS_READONLY, &nvs) == ESP_OK) {
        int32_t v = 0;
        if (nvs_get_i32(nvs, KEY_BRIGHT, &v) == ESP_OK && v >= 5 && v <= 100) {
            s_bright = (int)v;
        }
        nvs_close(nvs);
    }
    if (s_cfg.brightness_set) {
        s_cfg.brightness_set(s_bright);
    }

    s_last_activity = esp_timer_get_time();
    /* Core 0: rendering owns core 1. */
    xTaskCreatePinnedToCore(net_task, "eye_net", 4096, NULL, 3, NULL, 0);
    return ESP_OK;
}

/* ------------------------------------------------ console: wifi/bright -- */

static const char *k_state_names[] = {
    "off", "starting", "AP up", "connecting", "connected", "portal", "failed",
};

static int cmd_wifi(int argc, char **argv)
{
    if (argc < 2) {
        eye_net_status_t n;
        eye_net_get_status(&n);
        const char *mn = n.mode == EYE_NET_STA  ? "sta"
                       : n.mode == EYE_NET_AUTO ? "auto" : "ap";
        printf("wifi: %s, mode %s, state %s\n", n.enabled ? "on" : "off",
               mn, k_state_names[n.state]);
        if (n.ssid[0]) {
            printf("  ssid %s%s%s\n", n.ssid, n.pass[0] ? "  pw " : "", n.pass);
        }
        if (n.ip[0]) {
            printf("  %s  (%s)\n", n.url, n.ip);
        }
        printf("  sync peers: %d\n", eye_sync_peer_count());
        printf("usage: wifi <on|off|auto|ap|sta|forget>\n");
        return 0;
    }
    if (strcmp(argv[1], "on") == 0) {
        eye_net_set_enabled(true);
    } else if (strcmp(argv[1], "off") == 0) {
        eye_net_set_enabled(false);
    } else if (strcmp(argv[1], "auto") == 0) {
        eye_net_set_mode(EYE_NET_AUTO);
        eye_net_set_enabled(true);
    } else if (strcmp(argv[1], "ap") == 0) {
        eye_net_set_mode(EYE_NET_AP);
        eye_net_set_enabled(true);
    } else if (strcmp(argv[1], "sta") == 0) {
        eye_net_set_mode(EYE_NET_STA);
        eye_net_set_enabled(true);
    } else if (strcmp(argv[1], "forget") == 0) {
        eye_net_forget_wifi();
        printf("credentials forgotten\n");
    } else {
        printf("usage: wifi <on|off|auto|ap|sta|forget>\n");
        return 1;
    }
    return 0;
}

static int cmd_bright(int argc, char **argv)
{
    if (argc < 2) {
        printf("brightness = %d%%\n", eye_net_brightness());
        return 0;
    }
    eye_net_set_brightness(atoi(argv[1]));
    printf("brightness -> %d%%\n", eye_net_brightness());
    return 0;
}

void eye_net_register_console(void)
{
    const esp_console_cmd_t cmds[] = {
        { .command = "wifi",   .help = "wifi <on|off|auto|ap|sta|forget>: web control", .func = &cmd_wifi },
        { .command = "bright", .help = "bright <5-100>: panel brightness",         .func = &cmd_bright },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        esp_console_cmd_register(&cmds[i]);
    }
}
