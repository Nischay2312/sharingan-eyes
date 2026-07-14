/*
 * STA + SoftAP control for the Sharingan eye (adapted from ScreenStreamWatch's
 * wifi_manager.c, extended with a WPA2 SoftAP and credential forget).
 */

#include "wifi_ctrl.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs.h"

static const char *TAG = "wifi_ctrl";

#define NVS_NS      "eyewifi"
#define KEY_SSID    "ssid"
#define KEY_PASS    "pass"

#define CONNECTED_BIT BIT0
#define FAIL_BIT      BIT1
#define MAX_RETRY     4
#define CONNECT_WAIT_MS 15000

static EventGroupHandle_t s_events;
static bool  s_inited;
static bool  s_connected;
static bool  s_ap_mode;
static int   s_retry;
static char  s_ip[16];
static char  s_ssid[33];
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;

static void event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        if (s_retry < MAX_RETRY) {
            s_retry++;
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_events, FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
        s_connected = true;
        s_retry = 0;
        xEventGroupSetBits(s_events, CONNECTED_BIT);
        ESP_LOGI(TAG, "got ip %s", s_ip);
    }
}

esp_err_t wifi_ctrl_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif  = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi_init");

    s_events = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_events, ESP_ERR_NO_MEM, TAG, "events");

    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL), TAG, "reg wifi");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL), TAG, "reg ip");

    s_inited = true;
    return ESP_OK;
}

esp_err_t wifi_ctrl_start_sta(void)
{
    nvs_handle_t nvs;
    char pass[65] = { 0 };
    size_t slen = sizeof(s_ssid), plen = sizeof(pass);
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READONLY, &nvs), TAG, "no creds");
    esp_err_t err = nvs_get_str(nvs, KEY_SSID, s_ssid, &slen);
    if (err == ESP_OK && nvs_get_str(nvs, KEY_PASS, pass, &plen) != ESP_OK) {
        pass[0] = '\0';
    }
    nvs_close(nvs);
    ESP_RETURN_ON_ERROR(err, TAG, "no ssid saved");

    wifi_config_t cfg = { 0 };
    strncpy((char *)cfg.sta.ssid, s_ssid, sizeof(cfg.sta.ssid) - 1);
    strncpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    s_retry = 0;
    s_ap_mode = false;
    xEventGroupClearBits(s_events, CONNECTED_BIT | FAIL_BIT);

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &cfg), TAG, "config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");
    ESP_LOGI(TAG, "connecting to '%s'...", s_ssid);

    EventBits_t bits = xEventGroupWaitBits(s_events, CONNECTED_BIT | FAIL_BIT,
                                           pdTRUE, pdFALSE,
                                           pdMS_TO_TICKS(CONNECT_WAIT_MS));
    return (bits & CONNECTED_BIT) ? ESP_OK : ESP_FAIL;
}

esp_err_t wifi_ctrl_start_ap(const char *ssid, const char *pass, bool open)
{
    wifi_config_t cfg = {
        .ap = {
            .max_connection = 4,
            .channel = 1,
            .authmode = open ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK,
        },
    };
    strncpy((char *)cfg.ap.ssid, ssid, sizeof(cfg.ap.ssid) - 1);
    cfg.ap.ssid_len = (uint8_t)strlen((char *)cfg.ap.ssid);
    if (!open && pass) {
        strncpy((char *)cfg.ap.password, pass, sizeof(cfg.ap.password) - 1);
    }

    s_ap_mode = true;
    s_connected = false;
    /* APSTA so the portal can still scan for nearby networks. */
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &cfg), TAG, "config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "start");
    ESP_LOGI(TAG, "SoftAP '%s' up (%s)", ssid, open ? "open" : "wpa2");
    return ESP_OK;
}

esp_err_t wifi_ctrl_stop(void)
{
    s_connected = false;
    s_ap_mode = false;
    s_ip[0] = '\0';
    esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED && err != ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGW(TAG, "wifi_stop: %s", esp_err_to_name(err));
    }
    return ESP_OK;
}

bool wifi_ctrl_has_credentials(void)
{
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS, NVS_READONLY, &nvs) != ESP_OK) {
        return false;
    }
    size_t len = 0;
    bool has = (nvs_get_str(nvs, KEY_SSID, NULL, &len) == ESP_OK && len > 1);
    nvs_close(nvs);
    return has;
}

esp_err_t wifi_ctrl_save_credentials(const char *ssid, const char *pass)
{
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &nvs), TAG, "open");
    esp_err_t err = nvs_set_str(nvs, KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, KEY_PASS, pass ? pass : "");
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    ESP_LOGI(TAG, "saved credentials for '%s'", ssid);
    return err;
}

esp_err_t wifi_ctrl_forget_credentials(void)
{
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &nvs), TAG, "open");
    nvs_erase_all(nvs);
    nvs_commit(nvs);
    nvs_close(nvs);
    ESP_LOGI(TAG, "credentials forgotten");
    return ESP_OK;
}

bool wifi_ctrl_sta_connected(void) { return s_connected; }

const char *wifi_ctrl_ip(void)
{
    return s_ap_mode ? "192.168.4.1" : s_ip;
}

const char *wifi_ctrl_sta_ssid(void) { return s_ssid; }

int wifi_ctrl_rssi(void)
{
    if (!s_connected) {
        return 0;
    }
    wifi_ap_record_t ap;
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
}

void wifi_ctrl_mac_suffix(char *buf, size_t len)
{
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(buf, len, "%02X%02X", mac[4], mac[5]);
}
