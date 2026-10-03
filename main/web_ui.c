/* BUILD_NUMBER: 261003.113745 */
/**
 * @file web_ui.c
 * @brief Dual httpd servers: mode/OTA UI (port 80) and WebSerial (port 81).
 *
 * Mode page: dropdown of human-readable names; WS /ws pushes names and accepts
 * "set:N" (or legacy "toggle"). Button / WebSerial path uses web_ui_notify_mode().
 * OTA accepts multipart firmware on POST /upload (naive boundary trim).
 * WebSerial echoes text; command "mode" cycles display mode.
 * Pages embed PROJECT_BUILD_NUMBER from project_build_info.h.
 */

#include "web_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/semphr.h"
#include "project_build_info.h"

extern int mode;
extern int MaxMode;
void vu_cycle_mode(void);
void vu_set_mode(int m);
const char *vu_mode_name(int m);

static const char *TAG = "web";
static httpd_handle_t s_http;
static httpd_handle_t s_webserial;
static int s_ws_fd = -1;
static int s_webserial_fd = -1;
static SemaphoreHandle_t s_ws_mtx;

/** Mode UI HTML template; "%s" is replaced with PROJECT_BUILD_NUMBER. */
static const char INDEX_HTML[] =
"<!DOCTYPE HTML><html><head><title>VU Meter Web Server</title>"
"<meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
"<style>"
"html{font-family:Arial,Helvetica,sans-serif;text-align:center}"
"h1{font-size:1.8rem;color:white}"
"h2{font-size:1.5rem;font-weight:bold;color:#143642}"
".topnav{overflow:hidden;background-color:#143642}"
"body{margin:0}"
".content{padding:30px;max-width:600px;margin:0 auto}"
".card{background-color:#F8F7F9;box-shadow:2px 2px 12px 1px rgba(140,140,140,.5);padding-top:10px;padding-bottom:20px}"
".state{font-size:1.5rem;color:#8c8c8c;font-weight:bold}"
".mode-select{font-size:1.25rem;padding:12px 16px;margin:12px 0 20px;min-width:260px;"
" border:2px solid #143642;border-radius:5px;background:#fff;color:#143642;font-weight:bold}"
"</style></head><body>"
"<div class=\"topnav\"><h1>VU Meter Web Server</h1></div>"
"<div class=\"content\"><div class=\"card\">"
"<h2>Display Mode</h2>"
"<select id=\"modeSelect\" class=\"mode-select\">"
"<option value=\"0\">Linear Rainbow</option>"
"<option value=\"1\">Inside-Out Rainbow</option>"
"<option value=\"2\">Outside-In Rainbow</option>"
"<option value=\"3\">Linear Classic</option>"
"<option value=\"4\">Inside-Out Classic</option>"
"<option value=\"5\">Outside-In Classic</option>"
"<option value=\"6\">Off</option>"
"</select>"
"<p><a href=\"/update\">OTA Update</a> | <a href=\"http://\" id=\"wsLink\">WebSerial :81</a></p>"
"<p class=\"state\">build: %s</p>"
"</div></div>"
"<script>"
"var websocket;"
"var ignoreChange=false;"
"function setMode(v){"
" const sel=document.getElementById('modeSelect');"
" const t=String(v).trim();"
" ignoreChange=true;"
" if(/^[0-9]+$/.test(t)){sel.value=t;}"
" else{for(const o of sel.options){if(o.text===t){sel.value=o.value;break;}}}"
" ignoreChange=false;"
"}"
"function initWebSocket(){"
" websocket=new WebSocket(`ws://${location.host}/ws`);"
" websocket.onclose=()=>setTimeout(initWebSocket,2000);"
" websocket.onmessage=(e)=>setMode(e.data);"
"}"
"window.addEventListener('load',()=>{"
" initWebSocket();"
" const sel=document.getElementById('modeSelect');"
" sel.onchange=()=>{"
"  if(ignoreChange)return;"
"  if(websocket&&websocket.readyState===1)websocket.send('set:'+sel.value);"
" };"
" document.getElementById('wsLink').href='http://'+location.hostname+':81/webserial';"
"});"
"</script></body></html>";

/** OTA form HTML; "%s" is the running build label. */
static const char UPDATE_HTML[] =
"<!DOCTYPE html><html><head><title>OTA Update</title>"
"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
"</head><body style=\"font-family:sans-serif;max-width:480px;margin:2rem auto\">"
"<h1>Firmware OTA</h1>"
"<p>Running build: <strong>%s</strong></p>"
"<form method=\"POST\" action=\"/upload\" enctype=\"multipart/form-data\">"
"<input type=\"file\" name=\"firmware\" accept=\".bin\" required>"
"<button type=\"submit\">Upload & Reboot</button></form>"
"<p><a href=\"/\">Back</a></p></body></html>";

/** Minimal browser console for the WebSerial WebSocket. */
static const char WEBSERIAL_HTML[] =
"<!DOCTYPE html><html><head><title>WebSerial</title>"
"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
"<style>body{font-family:monospace;background:#111;color:#0f0;margin:0;padding:1rem}"
"#log{white-space:pre-wrap;height:70vh;overflow:auto;border:1px solid #333;padding:.5rem}"
"input{width:70%}button{padding:.4rem 1rem}</style></head><body>"
"<h1>WebSerial</h1><div id=\"log\"></div>"
"<input id=\"cmd\" placeholder=\"type 'mode' to cycle\"><button id=\"send\">Send</button>"
"<script>"
"const log=document.getElementById('log');"
"const ws=new WebSocket(`ws://${location.host}/ws`);"
"ws.onmessage=(e)=>{log.textContent+=e.data;log.scrollTop=log.scrollHeight;};"
"function sendCmd(){"
" const v=document.getElementById('cmd').value.trim();"
" if(!v) return;"
" ws.send(v); document.getElementById('cmd').value='';"
"}"
"document.getElementById('send').onclick=sendCmd;"
"document.getElementById('cmd').onkeydown=(e)=>{if(e.key==='Enter')sendCmd();};"
"</script></body></html>";

/** GET / — render mode page with build label. */
static esp_err_t root_get(httpd_req_t *req)
{
    char page[sizeof(INDEX_HTML) + 32];
    int n = snprintf(page, sizeof(page), INDEX_HTML, PROJECT_BUILD_NUMBER);
    if (n < 0 || n >= (int)sizeof(page)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "page too large");
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, page, n);
}

/** GET /update — OTA upload form. */
static esp_err_t update_get(httpd_req_t *req)
{
    char page[sizeof(UPDATE_HTML) + 32];
    int n = snprintf(page, sizeof(page), UPDATE_HTML, PROJECT_BUILD_NUMBER);
    if (n < 0 || n >= (int)sizeof(page)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "page too large");
    }
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, page, n);
}

static esp_err_t upload_post(httpd_req_t *req)
{
    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    if (!update) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
        return ESP_FAIL;
    }

    esp_ota_handle_t ota = 0;
    esp_err_t err = esp_ota_begin(update, OTA_WITH_SEQUENTIAL_WRITES, &ota);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return err;
    }

    char *buf = malloc(4096);
    if (!buf) {
        esp_ota_abort(ota);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_ERR_NO_MEM;
    }

    int remaining = req->content_len;
    bool header_skipped = false;
    while (remaining > 0) {
        int to_read = remaining > 4096 ? 4096 : remaining;
        int got = httpd_req_recv(req, buf, to_read);
        if (got <= 0) {
            if (got == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            free(buf);
            esp_ota_abort(ota);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Recv failed");
            return ESP_FAIL;
        }

        char *payload = buf;
        int payload_len = got;

        // Naive multipart: skip headers until blank line, then write binary.
        if (!header_skipped) {
            char *body = strstr(buf, "\r\n\r\n");
            if (body) {
                body += 4;
                payload = body;
                payload_len = got - (int)(body - buf);
                header_skipped = true;
            } else {
                remaining -= got;
                continue;
            }
        }

        // Trim trailing multipart boundary if present in last chunk
        if (remaining - got <= 0 && payload_len > 4) {
            for (int i = 0; i <= payload_len - 4; ++i) {
                if (payload[i] == '\r' && payload[i + 1] == '\n' &&
                    payload[i + 2] == '-' && payload[i + 3] == '-') {
                    payload_len = i;
                    break;
                }
            }
        }

        if (payload_len > 0) {
            err = esp_ota_write(ota, payload, payload_len);
            if (err != ESP_OK) {
                free(buf);
                esp_ota_abort(ota);
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA write failed");
                return err;
            }
        }
        remaining -= got;
    }
    free(buf);

    err = esp_ota_end(ota);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA end failed");
        return err;
    }
    err = esp_ota_set_boot_partition(update);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Set boot failed");
        return err;
    }

    httpd_resp_sendstr(req, "OK — rebooting");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

/** GET /favicon.ico — browsers always ask; avoid noisy 404s. */
static esp_err_t favicon_get(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/**
 * Payload for async WS sends — must outlive httpd_ws_send_frame_async().
 * (Stack buffers were freed before the frame was written → UI never updated.)
 */
static char s_mode_payload[48];

static int format_mode_payload(void)
{
    return snprintf(s_mode_payload, sizeof(s_mode_payload), "%s", vu_mode_name(mode));
}

/** Sync send on an active WS request (handshake / set / toggle handler). */
static esp_err_t ws_send_mode_sync(httpd_req_t *req)
{
    int n = format_mode_payload();
    if (n < 0) {
        return ESP_FAIL;
    }
    httpd_ws_frame_t frame = {
        .final = true,
        .fragmented = false,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)s_mode_payload,
        .len = (size_t)n,
    };
    return httpd_ws_send_frame(req, &frame);
}

/** Async send from another task (mode button). Uses persistent s_mode_payload. */
static void ws_send_mode_async(void)
{
    if (!s_http || s_ws_fd < 0) {
        return;
    }
    int n = format_mode_payload();
    if (n < 0) {
        return;
    }
    httpd_ws_frame_t frame = {
        .final = true,
        .fragmented = false,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)s_mode_payload,
        .len = (size_t)n,
    };
    if (httpd_ws_send_frame_async(s_http, s_ws_fd, &frame) != ESP_OK) {
        s_ws_fd = -1;
    }
}

/** httpd_queue_work callback — run async send on the httpd task. */
static void ws_notify_work(void *arg)
{
    (void)arg;
    if (s_ws_mtx) {
        xSemaphoreTake(s_ws_mtx, portMAX_DELAY);
    }
    ws_send_mode_async();
    if (s_ws_mtx) {
        xSemaphoreGive(s_ws_mtx);
    }
}

/** Mode-page WebSocket: on connect push mode; "set:N" / "toggle" change mode. */
static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        s_ws_fd = httpd_req_to_sockfd(req);
        /* Sync send during handshake so the page shows the real mode immediately. */
        (void)ws_send_mode_sync(req);
        return ESP_OK;
    }

    httpd_ws_frame_t frame = {0};
    frame.type = HTTPD_WS_TYPE_TEXT;
    esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
    if (ret != ESP_OK) {
        s_ws_fd = -1; // client closed / EAGAIN during teardown
        return ESP_OK;
    }
    if (frame.len == 0) {
        return ESP_OK;
    }

    uint8_t *buf = calloc(1, frame.len + 1);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    frame.payload = buf;
    ret = httpd_ws_recv_frame(req, &frame, frame.len);
    if (ret == ESP_OK) {
        if (strncmp((char *)buf, "set:", 4) == 0) {
            vu_set_mode(atoi((char *)buf + 4));
            (void)ws_send_mode_sync(req);
        } else if (strcmp((char *)buf, "toggle") == 0) {
            vu_cycle_mode();
            (void)ws_send_mode_sync(req);
        }
    } else {
        s_ws_fd = -1;
        ret = ESP_OK;
    }
    free(buf);
    return ret;
}

/** GET /webserial (and /) on the WebSerial server. */
static esp_err_t webserial_page(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, WEBSERIAL_HTML, HTTPD_RESP_USE_STRLEN);
}

/** WebSerial WebSocket: log lines, "mode" cycles display mode. */
static esp_err_t webserial_ws(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        s_webserial_fd = httpd_req_to_sockfd(req);
        return ESP_OK;
    }

    httpd_ws_frame_t frame = {0};
    frame.type = HTTPD_WS_TYPE_TEXT;
    esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
    if (ret != ESP_OK) {
        s_webserial_fd = -1; // tab closed / recv EAGAIN — drop stale fd
        return ESP_OK;
    }
    if (frame.len == 0) {
        return ESP_OK;
    }

    uint8_t *buf = calloc(1, frame.len + 1);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    frame.payload = buf;
    ret = httpd_ws_recv_frame(req, &frame, frame.len);
    if (ret == ESP_OK) {
        if (buf[0] != '\0') {
            ESP_LOGI(TAG, "WebSerial: %s", (char *)buf);
            if (strcmp((char *)buf, "mode") == 0) {
                vu_cycle_mode();
                web_ui_notify_mode();
                web_ui_printf("Mode: %s\n", vu_mode_name(mode));
            } else {
                web_ui_printf("Received: %s\n", (char *)buf);
            }
        }
    } else {
        s_webserial_fd = -1;
        ret = ESP_OK;
    }
    free(buf);
    return ret;
}

/**
 * Push current mode name to the mode-page WebSocket (button / WebSerial).
 * Queued onto the httpd task; payload lives in s_mode_payload.
 */
void web_ui_notify_mode(void)
{
    if (!s_http || s_ws_fd < 0) {
        return;
    }
    if (httpd_queue_work(s_http, ws_notify_work, NULL) != ESP_OK) {
        /* Fallback if the queue is full — still use persistent payload. */
        if (s_ws_mtx) {
            xSemaphoreTake(s_ws_mtx, portMAX_DELAY);
        }
        ws_send_mode_async();
        if (s_ws_mtx) {
            xSemaphoreGive(s_ws_mtx);
        }
    }
}

/** Async printf to the WebSerial client (dropped if none connected). */
void web_ui_printf(const char *fmt, ...)
{
    if (!s_webserial || s_webserial_fd < 0) {
        return;
    }
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    if (n <= 0) {
        return;
    }
    httpd_ws_frame_t frame = {
        .final = true,
        .fragmented = false,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)msg,
        .len = (size_t)n,
    };
    if (httpd_ws_send_frame_async(s_webserial, s_webserial_fd, &frame) != ESP_OK) {
        s_webserial_fd = -1;
    }
}

static httpd_handle_t start_server(int port, bool with_ota)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.ctrl_port = 32768 + port;
    config.lru_purge_enable = true;
    config.max_uri_handlers = 12;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed on port %d", port);
        return NULL;
    }

    if (with_ota) {
        const httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = root_get};
        const httpd_uri_t update = {.uri = "/update", .method = HTTP_GET, .handler = update_get};
        const httpd_uri_t upload = {.uri = "/upload", .method = HTTP_POST, .handler = upload_post};
        const httpd_uri_t ws = {
            .uri = "/ws",
            .method = HTTP_GET,
            .handler = ws_handler,
            .is_websocket = true,
        };
        const httpd_uri_t favicon = {.uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_get};
        httpd_register_uri_handler(server, &root);
        httpd_register_uri_handler(server, &update);
        httpd_register_uri_handler(server, &upload);
        httpd_register_uri_handler(server, &ws);
        httpd_register_uri_handler(server, &favicon);
    } else {
        const httpd_uri_t page = {.uri = "/webserial", .method = HTTP_GET, .handler = webserial_page};
        const httpd_uri_t ws = {
            .uri = "/ws",
            .method = HTTP_GET,
            .handler = webserial_ws,
            .is_websocket = true,
        };
        const httpd_uri_t root = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = webserial_page,
        };
        const httpd_uri_t favicon = {.uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_get};
        httpd_register_uri_handler(server, &page);
        httpd_register_uri_handler(server, &ws);
        httpd_register_uri_handler(server, &root);
        httpd_register_uri_handler(server, &favicon);
    }
    return server;
}

/** Create mutex and both servers; log listening ports. */
esp_err_t web_ui_start(int http_port, int webserial_port)
{
    s_ws_mtx = xSemaphoreCreateMutex();
    s_http = start_server(http_port, true);
    s_webserial = start_server(webserial_port, false);
    if (!s_http || !s_webserial) {
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "HTTP UI on :%d  WebSerial on :%d/webserial  OTA on :%d/update",
             http_port, webserial_port, http_port);
    return ESP_OK;
}