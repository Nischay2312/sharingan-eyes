#pragma once

/* Captive portal for first-time STA provisioning: open AP + DNS hijack +
 * a scan/password form. When credentials are received, eye_net restarts
 * WiFi in STA mode. */

#include <stdbool.h>
#include "esp_err.h"

esp_err_t wifi_portal_start(const char *ap_ssid);
esp_err_t wifi_portal_stop(void);
bool      wifi_portal_credentials_received(void);
