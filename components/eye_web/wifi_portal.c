/*
 * Captive portal (adapted from ScreenStreamWatch's wifi_portal.c). Assumes
 * wifi_ctrl already started an OPEN SoftAP in APSTA mode; this file adds the
 * DNS hijack + the provisioning HTTP server on 192.168.4.1.
 */

#include "wifi_portal.h"
#include "wifi_ctrl.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

static const char *TAG = "wifi_portal";

static httpd_handle_t s_httpd;
static TaskHandle_t   s_dns_task;
static volatile bool  s_creds_received;

static const char PORTAL_HTML[] =
    "<!DOCTYPE html><html><head>"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>Sharingan Eye Setup</title>"
    "<style>"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{font-family:-apple-system,sans-serif;background:#0a0a0f;color:#e8e8f0;padding:20px}"
    "h1{color:#e0141b;font-size:22px;margin-bottom:4px}"
    "h2{color:#888;font-size:14px;margin-bottom:16px;font-weight:normal}"
    ".net{padding:12px;margin:6px 0;background:#16161f;border-radius:12px;cursor:pointer;"
    "border:2px solid transparent;display:flex;justify-content:space-between;align-items:center}"
    ".net:active,.net.sel{border-color:#e0141b}"
    ".rssi{color:#666;font-size:13px}"
    "input{width:100%;padding:12px;margin:8px 0;border:1px solid #2a2a38;border-radius:10px;"
    "background:#12121a;color:#eee;font-size:16px}"
    "button{width:100%;padding:14px;background:#e0141b;color:#fff;border:none;border-radius:10px;"
    "font-size:17px;font-weight:bold;cursor:pointer;margin-top:8px}"
    "button:disabled{background:#333;color:#666}"
    ".msg{padding:10px;margin:8px 0;border-radius:10px;text-align:center}"
    ".ok{background:#1b4332;color:#95d5b2}.err{background:#3d0000;color:#ff6b6b}"
    ".spin{text-align:center;padding:24px;color:#666}"
    "</style></head><body>"
    "<h1>Sharingan Eye</h1><h2>Choose your WiFi</h2>"
    "<div id=\"nets\"><p class=\"spin\">Scanning...</p></div>"
    "<div id=\"form\" style=\"display:none\">"
    "<input id=\"ssid\" readonly placeholder=\"Network\">"
    "<input id=\"pw\" type=\"password\" placeholder=\"Password\">"
    "<button id=\"btn\" onclick=\"go()\">Save & connect</button>"
    "</div>"
    "<div id=\"msg\"></div>"
    "<script>"
    "let sel='';"
    "fetch('/scan').then(r=>r.json()).then(d=>{"
    "let h='';"
    "d.forEach(n=>h+='<div class=\"net\" onclick=\"pick(\\''+n.s+'\\',this)\">'"
    "+n.s+'<span class=\"rssi\">'+n.r+' dBm</span></div>');"
    "document.getElementById('nets').innerHTML=h||'<p class=\"spin\">No networks. Reload.</p>';"
    "});"
    "function pick(s,el){"
    "document.querySelectorAll('.net').forEach(e=>e.classList.remove('sel'));"
    "el.classList.add('sel');sel=s;"
    "document.getElementById('ssid').value=s;"
    "document.getElementById('form').style.display='block';"
    "}"
    "function go(){"
    "document.getElementById('btn').disabled=true;"
    "fetch('/connect',{method:'POST',headers:{'Content-Type':'application/json'},"
    "body:JSON.stringify({ssid:sel,password:document.getElementById('pw').value})})"
    ".then(r=>r.json()).then(d=>{"
    "document.getElementById('msg').innerHTML=d.ok"
    "?'<div class=\"msg ok\">Saved. The eye reconnects...</div>'"
    ":'<div class=\"msg err\">'+d.err+'</div>';"
    "document.getElementById('btn').disabled=!d.ok;"
    "});"
    "}"
    "</script></body></html>";

static esp_err_t handler_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PORTAL_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handler_scan(httpd_req_t *req)
{
    wifi_scan_config_t scan_cfg = { 0 };
    esp_wifi_scan_start(&scan_cfg, true);

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 20) {
        n = 20;
    }

    cJSON *arr = cJSON_CreateArray();
    if (n > 0) {
        wifi_ap_record_t *recs = calloc(n, sizeof(*recs));
        if (recs == NULL) {
            cJSON_Delete(arr);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
            return ESP_FAIL;
        }
        esp_wifi_scan_get_ap_records(&n, recs);
        for (int i = 0; i < n; i++) {
            if (recs[i].ssid[0] == '\0') {
                continue;
            }
            bool dup = false;
            for (int j = 0; j < i && !dup; j++) {
                dup = strcmp((char *)recs[j].ssid, (char *)recs[i].ssid) == 0;
            }
            if (dup) {
                continue;
            }
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "s", (char *)recs[i].ssid);
            cJSON_AddNumberToObject(o, "r", recs[i].rssi);
            cJSON_AddItemToArray(arr, o);
        }
        free(recs);
    }

    char *json = cJSON_PrintUnformatted(arr);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    cJSON_Delete(arr);
    return ESP_OK;
}

static esp_err_t handler_connect(httpd_req_t *req)
{
    char buf[256] = { 0 };
    int got = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (got <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }
    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad JSON");
        return ESP_FAIL;
    }
    const char *ssid = cJSON_GetStringValue(cJSON_GetObjectItem(root, "ssid"));
    const char *pass = cJSON_GetStringValue(cJSON_GetObjectItem(root, "password"));

    cJSON *resp = cJSON_CreateObject();
    if (ssid && ssid[0]) {
        wifi_ctrl_save_credentials(ssid, pass ? pass : "");
        cJSON_AddBoolToObject(resp, "ok", true);
        s_creds_received = true;
    } else {
        cJSON_AddBoolToObject(resp, "ok", false);
        cJSON_AddStringToObject(resp, "err", "SSID required");
    }
    char *json = cJSON_PrintUnformatted(resp);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    cJSON_Delete(resp);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t handler_redirect(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

/* Minimal DNS: answer every query with 192.168.4.1 (captive portal hijack). */
static void dns_server_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS socket failed");
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = INADDR_ANY,
    };
    bind(sock, (struct sockaddr *)&addr, sizeof(addr));

    uint8_t rx[512], tx[512];
    const uint8_t ap_ip[] = { 192, 168, 4, 1 };
    for (;;) {
        struct sockaddr_in client;
        socklen_t clen = sizeof(client);
        int len = recvfrom(sock, rx, sizeof(rx), 0, (struct sockaddr *)&client, &clen);
        if (len < 12 || len > (int)sizeof(tx) - 16) {
            continue;
        }
        memcpy(tx, rx, len);
        tx[2] = 0x81; tx[3] = 0x80;      /* response, no error   */
        tx[6] = 0x00; tx[7] = 0x01;      /* one answer           */
        int off = len;
        tx[off++] = 0xC0; tx[off++] = 0x0C;             /* name ptr  */
        tx[off++] = 0x00; tx[off++] = 0x01;             /* type A    */
        tx[off++] = 0x00; tx[off++] = 0x01;             /* class IN  */
        tx[off++] = 0x00; tx[off++] = 0x00; tx[off++] = 0x00; tx[off++] = 0x3C;
        tx[off++] = 0x00; tx[off++] = 0x04;
        memcpy(&tx[off], ap_ip, 4);
        off += 4;
        sendto(sock, tx, off, 0, (struct sockaddr *)&client, clen);
    }
}

esp_err_t wifi_portal_start(const char *ap_ssid)
{
    s_creds_received = false;

    esp_err_t err = wifi_ctrl_start_ap(ap_ssid, NULL, true /* open */);
    if (err != ESP_OK) {
        return err;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.max_uri_handlers = 8;
    err = httpd_start(&s_httpd, &cfg);
    if (err != ESP_OK) {
        return err;
    }

    const httpd_uri_t uris[] = {
        { .uri = "/",        .method = HTTP_GET,  .handler = handler_root },
        { .uri = "/scan",    .method = HTTP_GET,  .handler = handler_scan },
        { .uri = "/connect", .method = HTTP_POST, .handler = handler_connect },
        { .uri = "/*",       .method = HTTP_GET,  .handler = handler_redirect },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_httpd, &uris[i]);
    }

    xTaskCreate(dns_server_task, "dns_srv", 4096, NULL, 5, &s_dns_task);
    ESP_LOGI(TAG, "portal up: join '%s' -> http://192.168.4.1", ap_ssid);
    return ESP_OK;
}

esp_err_t wifi_portal_stop(void)
{
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    if (s_dns_task) {
        vTaskDelete(s_dns_task);
        s_dns_task = NULL;
    }
    return ESP_OK;
}

bool wifi_portal_credentials_received(void)
{
    return s_creds_received;
}
