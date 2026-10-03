/* BUILD_NUMBER: 261003.113745 */
/**
 * @file app_main.cpp
 * @brief Firmware entry: init peripherals, then forever loop stereo VU processing.
 *
 * Boot path:
 *  1. Log PROJECT_BUILD_NUMBER / esp_app_desc.version
 *  2. NVS flash
 *  3. Status LED + mode button GPIO
 *  4. ADC, LED strips, brightness
 *  5. Wi-Fi (optional) + HTTP/OTA/WebSerial
 *
 * Main loop (cooperative on the main task):
 *  - Refresh brightness pot + mode button (button notifies web UI)
 *  - Average stereo ADC → subtract noise_floor_adc → dB
 *  - vu_ballistics() → vu_display() (classic zones or rainbow FX)
 *
 * ESP-IDF port of the Arduino/PlatformIO project under ESP32VU-meter_241230/.
 */

#include <math.h>

#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "credentials.h"
#include "pins.h"
#include "vu_config.h"
#include "adc_input.h"
#include "wifi_sta.h"
#include "web_ui.h"
#include "project_build_info.h"

static const char *TAG = "main";

/**
 * @brief ESP-IDF application entry (runs on the main FreeRTOS task).
 *
 * Does not return; the stereo VU meter loop runs indefinitely.
 */
extern "C" void app_main(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    ESP_LOGI(TAG, "ESP32VU-meter build %s (app.version=%s)",
             PROJECT_BUILD_NUMBER, app ? app->version : "?");

    /* NVS is required by Wi-Fi; erase+retry if the partition layout changed. */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* Brief LED on to show life, then off until Wi-Fi IP starts 500 ms blink. */
    gpio_config_t io = {};
    io.mode = GPIO_MODE_OUTPUT;
    io.pin_bit_mask = (1ULL << LED_PIN);
    gpio_config(&io);
    gpio_set_level(static_cast<gpio_num_t>(LED_PIN), 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(static_cast<gpio_num_t>(LED_PIN), 1);

    /* Mode button: active-low with pull-up. */
    io = {};
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    io.pin_bit_mask = (1ULL << MODE_PIN);
    gpio_config(&io);

    ESP_ERROR_CHECK(adc_input_init());
    vu_init_strips();
    adc_input_read_brightness();
    vu_apply_brightness();

    /* Network features are optional — meter still runs if STA fails. */
    ret = wifi_sta_start(WIFI_SSID, WIFI_PASSWORD, LOCAL_IP, GATEWAY, SUBNET, PRIMARY_DNS);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi not ready; continuing without network features");
    } else {
        ESP_ERROR_CHECK(web_ui_start(HTTP_PORT, WEBSERIAL_PORT));
    }

    while (true) {
        adc_input_read_brightness();
        vu_apply_brightness();
        vu_handle_mode_button();

        int sum_left = 0, sum_right = 0, count = 0;
        adc_input_average_channels(&sum_left, &sum_right, &count, average_time);

        /* Subtract silent-input noise floor in linear ADC domain, then convert to dB. */
        float mean_left = sum_left / (float)count;
        float mean_right = sum_right / (float)count;
        mean_left = fmaxf(0.0f, mean_left - noise_floor_adc);
        mean_right = fmaxf(0.0f, mean_right - noise_floor_adc);

        float average_left = (mean_left > 0.0f)
                                 ? (20.0f * log10f(VoltageFactor * mean_left))
                                 : dBmin;
        float average_right = (mean_right > 0.0f)
                                  ? (20.0f * log10f(VoltageFactor * mean_right))
                                  : dBmin;
        if (!isfinite(average_left) || average_left < dBmin) {
            average_left = dBmin;
        }
        if (!isfinite(average_right) || average_right < dBmin) {
            average_right = dBmin;
        }

        float smooth_left = dBmin;
        float smooth_right = dBmin;
        vu_ballistics(average_left, average_right, &smooth_left, &smooth_right);
        vu_display(smooth_left, smooth_right);

        /*
         * Optional per-frame telemetry (UART + WebSerial). Left commented to
         * keep the loop quiet at 921600 baud during normal use.
         *
        ESP_LOGI(TAG, "Mode: %d, L: %.0f, R: %.0f, count: %d, brightness: %d",
                 mode, average_left, average_right, count, brightness);
        web_ui_printf("-> Mode: %d, Average Left: %.0f, Average Right: %.0f, Count: %d, brightness: %d\n",
                      mode, average_left, average_right, count, brightness);
        */
    }
}
