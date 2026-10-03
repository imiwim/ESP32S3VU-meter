/* BUILD_NUMBER: 261003.113745 */
/**
 * @file pins.h
 * @brief Board GPIO map for the stereo ESP32-S3 VU meter.
 *
 * Defaults target an ESP32-S3-Zero / QT Py ESP32-S3 style board:
 *  - Two WS2812 strips (left / right channel bars)
 *  - One onboard RGB status LED
 *  - Stereo audio into ADC GPIOs
 *  - Brightness pot + mode button
 *
 * @note LED_PIN (status LED) is active-low: drive LOW to turn ON, HIGH for OFF.
 */
#pragma once

/** Onboard WS2812 RGB status LED (RMT). Optional — init may skip if RMT is exhausted. */
#define RGB_PIN 21

/** Board status LED (active-low). Blinks 500 ms when Wi-Fi has an IP. */
#define LED_PIN 10

/** Mode button input (active-low with internal pull-up). Cycles display mode. */
#define MODE_PIN 7

/** WS2812 data out for the left-channel VU bar. */
#define LED_PIN_left 8

/** WS2812 data out for the right-channel VU bar. */
#define LED_PIN_right 9

/** Analog brightness pot (ADC). Mapped to 0–255 strip intensity (EMA in adc_input). */
#define BrightnessPin 6

/** Left audio input (ADC). Centered around ADC_midpoint (~2048 at 12-bit). */
#define Left_In 4

/** Right audio input (ADC). Same scale as Left_In. */
#define Right_In 5

/** Number of addressable LEDs on each VU strip (not including onboard RGB). */
#define LED_COUNT 216
