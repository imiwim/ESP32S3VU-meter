/* BUILD_NUMBER: 261003.113745 */
/**
 * @file wifi_sta.h
 * @brief Station-mode Wi-Fi with static IPv4 (credentials from credentials.h).
 *
 * Blocks up to 30 s waiting for IP_EVENT_STA_GOT_IP. On success, LED_PIN
 * (active-low) toggles every 500 ms; on disconnect the blink stops (LED off)
 * and reconnect is attempted.
 */
#pragma once

#include "esp_err.h"
#include "esp_netif.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Bring up STA Wi-Fi with a fixed IP configuration.
 *
 * @param ssid       AP SSID.
 * @param password   WPA2 PSK.
 * @param local_ip   Device IPv4 string (e.g. "172.16.21.4").
 * @param gateway    Gateway IPv4 string.
 * @param subnet     Netmask IPv4 string.
 * @param dns        Primary DNS IPv4 string.
 * @return ESP_OK when associated and IP ready, ESP_ERR_TIMEOUT otherwise.
 */
esp_err_t wifi_sta_start(const char *ssid,
                         const char *password,
                         const char *local_ip,
                         const char *gateway,
                         const char *subnet,
                         const char *dns);

/**
 * @brief Default STA netif created by wifi_sta_start (may be NULL before start).
 */
esp_netif_t *wifi_sta_netif(void);

#ifdef __cplusplus
}
#endif
