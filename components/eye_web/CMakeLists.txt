# WiFi + web control app, shared by BOTH boards. Board specifics are injected
# through eye_net_config_t (clip directory + brightness callback), so this
# component -- like eye_core -- pulls in NO BSP.
idf_component_register(
    SRCS
        "eye_net.c"          # lifecycle state machine + console cmds
        "wifi_ctrl.c"        # STA/AP + NVS credentials
        "wifi_portal.c"      # captive portal for first-time STA setup
        "eye_webserver.c"    # REST API + embedded web app + mDNS
        "eye_sync.c"         # ESP-NOW cross-board settings sync
    INCLUDE_DIRS
        "."
    EMBED_FILES
        "web_ui/index.html"  # the whole web app, served from flash
    REQUIRES
        eye_core
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        esp_http_server
        esp_timer
        lwip                 # portal's raw DNS socket
        console              # wifi/bright commands on the eye> console
        json                 # cJSON (portal + REST)
        espressif__mdns
        app_update)          # esp_ota_ops (OTA firmware update)
