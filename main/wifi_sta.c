/* BUILD_NUMBER: 261003.113745 */
/**
 * @file wifi_sta.c
 * @brief STA Wi-Fi bring-up with static IPv4 and link LED.
 *
 * Sequence: netif + event loop → stop DHCP → set IP/DNS → wifi_init → connect →
 * wait for GOT_IP (30 s). After association, LED_PIN toggles every 500 ms
 * (active-low); on disconnect the timer stops and the LED stays off.
 */

#include "wifi_sta.h"

#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/event_groups.h"
#include "freertos/timers.h"
#include "driver/gpio.h"
#include "pins.h"

static const char *TAG = "wifi";
static EventGroupHandle_t s_wifi_events;
static esp_netif_t *s_netif;
static TimerHandle_t s_led_timer;
static int s_led_level = 1; // GPIO level; 1 = OFF (active-low)

/** Set when IP_EVENT_STA_GOT_IP arrives. */
#define WIFI_CONNECTED_BIT BIT0

/** Active-low status LED: flip level each 500 ms while associated. */
static void led_blink_cb(TimerHandle_t timer)
{
    (void)timer;
    s_led_level ^= 1;
    gpio_set_level(LED_PIN, s_led_level);
}

static void led_blink_start(void)
{
    if (!s_led_timer) {
        s_led_timer = xTimerCreate("wifi_led", pdMS_TO_TICKS(500), pdTRUE, NULL, led_blink_cb);
        if (!s_led_timer) {
            ESP_LOGE(TAG, "LED blink timer create failed");
            return;
        }
    }
    s_led_level = 0; // active-low ON for first half-period
    gpio_set_level(LED_PIN, s_led_level);
    xTimerStart(s_led_timer, 0);
}

static void led_blink_stop(void)
{
    if (s_led_timer) {
        xTimerStop(s_led_timer, 0);
    }
    s_led_level = 1;
    gpio_set_level(LED_PIN, s_led_level); // active-low OFF
}

/**
 * @brief WIFI / IP event handler: connect, reconnect, and signal GOT_IP.
 */
static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Disconnected, retrying...");
        led_blink_stop();
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        led_blink_start();
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

esp_netif_t *wifi_sta_netif(void)
{
    return s_netif;
}

esp_err_t wifi_sta_start(const char *ssid,
                         const char *password,
                         const char *local_ip,
                         const char *gateway,
                         const char *subnet,
                         const char *dns)
{
    s_wifi_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();

    // Static IP — credentials.h supplies the strings used by the caller.
    esp_netif_dhcpc_stop(s_netif);
    esp_netif_ip_info_t ip_info;
    memset(&ip_info, 0, sizeof(ip_info));
    ESP_ERROR_CHECK(esp_netif_str_to_ip4(local_ip, &ip_info.ip));
    ESP_ERROR_CHECK(esp_netif_str_to_ip4(gateway, &ip_info.gw));
    ESP_ERROR_CHECK(esp_netif_str_to_ip4(subnet, &ip_info.netmask));
    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_netif, &ip_info));

    esp_netif_dns_info_t dns_info = {0};
    ESP_ERROR_CHECK(esp_netif_str_to_ip4(dns, &dns_info.ip.u_addr.ip4));
    dns_info.ip.type = ESP_IPADDR_TYPE_V4;
    ESP_ERROR_CHECK(esp_netif_set_dns_info(s_netif, ESP_NETIF_DNS_MAIN, &dns_info));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi_event, NULL));

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to SSID:%s ...", ssid);
    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(30000));
    if (!(bits & WIFI_CONNECTED_BIT)) {
        ESP_LOGE(TAG, "Failed to connect within 30s");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}
