#pragma once

/* STA + SoftAP control with NVS credential storage (AMOLED board only). */

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** One-time stack init (netif, event loop, esp_wifi_init). Idempotent. */
esp_err_t wifi_ctrl_init(void);

/** Blocking STA connect using saved creds (~15 s timeout). */
esp_err_t wifi_ctrl_start_sta(void);

/** Start a WPA2 SoftAP (pass open=true for the provisioning portal's open AP). */
esp_err_t wifi_ctrl_start_ap(const char *ssid, const char *pass, bool open);

/** Stop WiFi entirely (radio off -> minimum draw). */
esp_err_t wifi_ctrl_stop(void);

bool      wifi_ctrl_has_credentials(void);
esp_err_t wifi_ctrl_save_credentials(const char *ssid, const char *pass);
esp_err_t wifi_ctrl_forget_credentials(void);

bool        wifi_ctrl_sta_connected(void);
const char *wifi_ctrl_ip(void);        /* STA ip, or "192.168.4.1" in AP mode */
const char *wifi_ctrl_sta_ssid(void);  /* saved/connected STA ssid            */
int         wifi_ctrl_rssi(void);      /* STA rssi dBm, 0 if n/a              */

/** Stable per-device suffix from the MAC (e.g. "7C3A"). */
void wifi_ctrl_mac_suffix(char *buf, size_t len);

#ifdef __cplusplus
}
#endif
