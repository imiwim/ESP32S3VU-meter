/* BUILD_NUMBER: 261003.113745 */
/**
 * @file vu_leds.c
 * @brief RMT WS2812 driver glue via espressif/led_strip.
 *
 * RMT channel budget on ESP32-S3 is tight (4 × 48 symbols). Each strip uses one
 * memory block with DMA off so left + right + onboard RGB can coexist.
 * vu_leds_set_pixel applies pot brightness; vu_leds_set_pixel_raw does not
 * (classic latched palette writes pre-scaled RGB).
 */

#include "vu_leds.h"

#include "esp_log.h"
#include "led_strip_rmt.h"
#include "pins.h"
#include "sdkconfig.h"

static const char *TAG = "vu_leds";

/** Software brightness for VU strips only (onboard RGB stays full). */
static uint8_t s_brightness = 255;

led_strip_handle_t stripLeft = NULL;
led_strip_handle_t stripRight = NULL;
led_strip_handle_t rgbLed = NULL;

/**
 * @brief Allocate one RMT WS2812 device and clear it.
 * @param gpio     Data GPIO.
 * @param max_leds Pixel count.
 * @param[out] out Handle on success, NULL on failure.
 */
static esp_err_t create_strip(int gpio, uint32_t max_leds, led_strip_handle_t *out)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = gpio,
        .max_leds = max_leds,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = {
            .invert_out = false,
        },
    };

    // ESP32-S3 RMT pool is 4 × 48 symbols. Requesting >48 makes the driver take
    // two blocks per strip, so only two strips fit and the third fails with
    // "no free tx channels". Use one block per strip (no DMA).
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = CONFIG_SOC_RMT_MEM_WORDS_PER_CHANNEL,
        .flags = {
            .with_dma = false,
        },
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, out);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led_strip_new_rmt_device GPIO%d failed: %s", gpio, esp_err_to_name(err));
        *out = NULL;
        return err;
    }
    return led_strip_clear(*out);
}

esp_err_t vu_leds_init(void)
{
    ESP_ERROR_CHECK(create_strip(LED_PIN_left, LED_COUNT, &stripLeft));
    ESP_ERROR_CHECK(create_strip(LED_PIN_right, LED_COUNT, &stripRight));

    // Status LED is nice-to-have; VU strips must succeed.
    esp_err_t rgb_err = create_strip(RGB_PIN, 1, &rgbLed);
    if (rgb_err != ESP_OK) {
        ESP_LOGW(TAG, "Onboard RGB on GPIO%d unavailable (%s); continuing without it",
                 RGB_PIN, esp_err_to_name(rgb_err));
        rgbLed = NULL;
    }

    ESP_LOGI(TAG, "led_strip ready (L=%d R=%d RGB=%s, %d LEDs/strip)",
             LED_PIN_left, LED_PIN_right, rgbLed ? "yes" : "no", LED_COUNT);
    return ESP_OK;
}

void vu_leds_set_brightness(uint8_t brightness)
{
    s_brightness = brightness;
}

void vu_leds_set_pixel(led_strip_handle_t strip, int index, uint8_t r, uint8_t g, uint8_t b)
{
    if (!strip || index < 0) {
        return;
    }
    // Pure-blue peak markers can compute index == LED_COUNT; Adafruit ignored
    // that, led_strip logs an error — clamp instead.
    const int max_leds = (strip == rgbLed) ? 1 : LED_COUNT;
    if (index >= max_leds) {
        return;
    }
    // Brightness pot applies to VU strips only; onboard RGB stays full intensity.
    if (strip != rgbLed && s_brightness != 255) {
        r = (uint8_t)((r * s_brightness) >> 8);
        g = (uint8_t)((g * s_brightness) >> 8);
        b = (uint8_t)((b * s_brightness) >> 8);
    }
    led_strip_set_pixel(strip, (uint32_t)index, r, g, b);
}

void vu_leds_set_pixel_raw(led_strip_handle_t strip, int index, uint8_t r, uint8_t g, uint8_t b)
{
    if (!strip || index < 0) {
        return;
    }
    const int max_leds = (strip == rgbLed) ? 1 : LED_COUNT;
    if (index >= max_leds) {
        return;
    }
    led_strip_set_pixel(strip, (uint32_t)index, r, g, b);
}

void vu_leds_show(led_strip_handle_t strip)
{
    if (strip) {
        led_strip_refresh(strip);
    }
}

void vu_leds_clear(led_strip_handle_t strip)
{
    if (strip) {
        led_strip_clear(strip);
    }
}

long map(long x, long in_min, long in_max, long out_min, long out_max)
{
    if (in_max == in_min) {
        return out_min;
    }
    return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}
