/* BUILD_NUMBER: 261003.113745 */
/**
 * @file web_ui.h
 * @brief HTTP mode UI + OTA (port 80) and WebSerial console (port 81).
 *
 * Port 80:
 *  - GET /           mode dropdown page (WebSocket /ws; names via vu_mode_name)
 *  - GET /update     OTA upload form
 *  - POST /upload    firmware binary (multipart)
 *
 * Port 81:
 *  - GET /webserial  simple browser console
 *  - WS /ws          bidirectional text; "mode" cycles display mode
 */
#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start both HTTP servers.
 * @param http_port      Mode UI + OTA (typically 80).
 * @param webserial_port WebSerial UI (typically 81).
 * @return ESP_OK if both servers started.
 */
esp_err_t web_ui_start(int http_port, int webserial_port);

/**
 * @brief Push the current mode name (@c vu_mode_name) to the mode-page WebSocket.
 */
void web_ui_notify_mode(void);

/**
 * @brief Printf-style text to the connected WebSerial WebSocket client.
 * @param fmt printf format string.
 */
void web_ui_printf(const char *fmt, ...);

#ifdef __cplusplus
}
#endif
