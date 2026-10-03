/* BUILD_NUMBER: 261003.113745 */
/**
 * @file adc_input.h
 * @brief Oneshot ADC path for stereo audio + brightness pot.
 *
 * All three GPIOs (Left_In, Right_In, BrightnessPin) must share one ADC unit.
 * Channel reads use a discard+sample pair so mux settling is consistent for L/R.
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize ADC oneshot unit and configure L / R / brightness channels.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if pins span multiple ADC units.
 */
esp_err_t adc_input_init(void);

/**
 * @brief Settled raw 12-bit reading for an arbitrary ADC-capable GPIO.
 * @param gpio_num GPIO number previously mapped with adc_oneshot_io_to_channel.
 * @return Raw sample, or 0 if the GPIO is not an ADC pin / ADC not ready.
 */
int adc_input_read_raw(int gpio_num);

/**
 * @brief Sample brightness pot (settled + EMA) and update @c brightness (0–255).
 */
void adc_input_read_brightness(void);

/**
 * @brief Average |ADC − midpoint| for both channels over a time window.
 *
 * Alternates L-first / R-first each call to cancel residual mux bias.
 * Outputs are linear magnitude sums suitable for later noise-floor subtract + dB.
 *
 * @param[out] sum_left  Sum of abs(left − midpoint) samples.
 * @param[out] sum_right Sum of abs(right − midpoint) samples.
 * @param[out] count     Number of L/R sample pairs collected (≥ 1).
 * @param average_time_ms Averaging window in milliseconds.
 */
void adc_input_average_channels(int *sum_left, int *sum_right, int *count, int average_time_ms);

#ifdef __cplusplus
}
#endif
