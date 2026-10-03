/* BUILD_NUMBER: 261003.113745 */
/**
 * @file vu_leds.h
 * @brief Thin wrapper around Espressif `led_strip` (RMT WS2812).
 *
 * Owns the three strip handles used by the VU meter:
 *  - @c stripLeft / @c stripRight — full LED_COUNT bars
 *  - @c rgbLed — single onboard status pixel (may be NULL if RMT create fails)
 *
 * Brightness from the pot is applied only to the VU strips, not the onboard RGB.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "led_strip.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Left-channel WS2812 strip handle (LED_COUNT pixels). */
extern led_strip_handle_t stripLeft;

/** Right-channel WS2812 strip handle (LED_COUNT pixels). */
extern led_strip_handle_t stripRight;

/** Onboard single-pixel RGB handle, or NULL if unavailable. */
extern led_strip_handle_t rgbLed;

/**
 * @brief Create RMT-backed WS2812 devices for left, right, and onboard RGB.
 * @return ESP_OK if both VU strips were created (RGB may still be NULL).
 */
esp_err_t vu_leds_init(void);

/**
 * @brief Set software brightness scale for VU strips (0–255).
 * @param brightness 255 = full intensity; applied as (channel * brightness) >> 8.
 */
void vu_leds_set_brightness(uint8_t brightness);

/**
 * @brief Write one GRB pixel (brightness-scaled for VU strips).
 * @param strip Strip handle (no-op if NULL).
 * @param index Pixel index; out-of-range is ignored (blue peak-hold safety).
 * @param r Red 0–255 (pre-scale).
 * @param g Green 0–255 (pre-scale).
 * @param b Blue 0–255 (pre-scale).
 */
void vu_leds_set_pixel(led_strip_handle_t strip, int index, uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief Write one pixel without pot brightness scaling.
 * Used by classic modes with a pre-scaled latched palette.
 */
void vu_leds_set_pixel_raw(led_strip_handle_t strip, int index, uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief Push the strip framebuffer to the LEDs (RMT refresh).
 * @param strip Strip handle (no-op if NULL).
 */
void vu_leds_show(led_strip_handle_t strip);

/**
 * @brief Clear all pixels on a strip and refresh.
 * @param strip Strip handle (no-op if NULL).
 */
void vu_leds_clear(led_strip_handle_t strip);

/**
 * @brief Arduino-compatible linear map (integer).
 * @return Mapped value in [out_min, out_max] range (not hard-clamped).
 */
long map(long x, long in_min, long in_max, long out_min, long out_max);

#ifdef __cplusplus
}
#endif
