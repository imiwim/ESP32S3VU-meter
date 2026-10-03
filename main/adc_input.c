/* BUILD_NUMBER: 261003.113745 */
/**
 * @file adc_input.c
 * @brief Stereo audio + brightness sampling with settled dual-reads.
 *
 * Audio path: raw − ADC_midpoint → abs → sum over average_time_ms.
 * Brightness path: extra mux settles + EMA, then 0…4095 → brightness 0…255
 * (reduces crosstalk from the shared ADC when audio is loud).
 */

#include "adc_input.h"

#include <stdlib.h>
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "freertos/FreeRTOS.h" // IWYU pragma: keep
#include "freertos/task.h"
#include "pins.h"

extern int brightness;
extern int ADC_midpoint;

static const char *TAG = "adc";
static adc_oneshot_unit_handle_t s_adc = NULL;
static adc_channel_t s_ch_left;
static adc_channel_t s_ch_right;
static adc_channel_t s_ch_bright;
static adc_unit_t s_unit;

/**
 * @brief Read a channel twice; keep the second sample after mux settle.
 * @param channel Configured oneshot channel.
 * @return Raw 12-bit sample, or ADC_midpoint on read failure.
 */
static int read_channel_settled(adc_channel_t channel)
{
    int raw = 0;
    (void)adc_oneshot_read(s_adc, channel, &raw); // discard after mux switch
    if (adc_oneshot_read(s_adc, channel, &raw) != ESP_OK) {
        return ADC_midpoint;
    }
    return raw;
}

esp_err_t adc_input_init(void)
{
    adc_unit_t unit_l, unit_r, unit_b;
    ESP_ERROR_CHECK(adc_oneshot_io_to_channel(Left_In, &unit_l, &s_ch_left));
    ESP_ERROR_CHECK(adc_oneshot_io_to_channel(Right_In, &unit_r, &s_ch_right));
    ESP_ERROR_CHECK(adc_oneshot_io_to_channel(BrightnessPin, &unit_b, &s_ch_bright));

    if (unit_l != unit_r || unit_l != unit_b) {
        ESP_LOGE(TAG, "All ADC pins must be on the same ADC unit");
        return ESP_ERR_INVALID_ARG;
    }
    s_unit = unit_l;

    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = s_unit,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_cfg, &s_adc));

    /* 12-bit, 12 dB atten ≈ 0–3.3 V full scale on ESP32-S3. */
    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH_12,
        .atten = ADC_ATTEN_DB_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, s_ch_left, &chan_cfg));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, s_ch_right, &chan_cfg));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, s_ch_bright, &chan_cfg));

    ESP_LOGI(TAG, "ADC oneshot ready (L=%d R=%d B=%d), settled dual-read", Left_In, Right_In, BrightnessPin);
    return ESP_OK;
}

int adc_input_read_raw(int gpio_num)
{
    adc_unit_t unit;
    adc_channel_t channel;
    if (adc_oneshot_io_to_channel(gpio_num, &unit, &channel) != ESP_OK || !s_adc) {
        return 0;
    }
    return read_channel_settled(channel);
}

void adc_input_read_brightness(void)
{
    /*
     * Extra settles + EMA: brightness shares the ADC mux with audio, so loud
     * input can chatter the pot reading and make classic zone edges shimmer.
     */
    int discard = 0;
    (void)adc_oneshot_read(s_adc, s_ch_bright, &discard);
    (void)read_channel_settled(s_ch_bright);
    int raw = read_channel_settled(s_ch_bright);
    static int s_filt = -1;
    if (s_filt < 0) {
        s_filt = raw;
    } else {
        s_filt = (s_filt * 7 + raw) / 8;
    }
    brightness = (s_filt * 255) / 4095;
    if (brightness < 0) {
        brightness = 0;
    }
    if (brightness > 255) {
        brightness = 255;
    }
}

void adc_input_average_channels(int *sum_left, int *sum_right, int *count, int average_time_ms)
{
    *sum_left = 0;
    *sum_right = 0;
    *count = 0;

    TickType_t start = xTaskGetTickCount();
    TickType_t window = pdMS_TO_TICKS(average_time_ms);
    if (window < 1) {
        window = 1;
    }

    static TickType_t s_last_extreme_warn;
    bool extreme = false;

    // Alternate start channel each window so any residual mux bias cancels over time.
    static bool start_with_right = false;
    const bool right_first = start_with_right;
    start_with_right = !start_with_right;

    while ((xTaskGetTickCount() - start) < window) {
        int left_raw;
        int right_raw;
        if (right_first) {
            right_raw = read_channel_settled(s_ch_right);
            left_raw = read_channel_settled(s_ch_left);
        } else {
            left_raw = read_channel_settled(s_ch_left);
            right_raw = read_channel_settled(s_ch_right);
        }

        int left = left_raw - ADC_midpoint;
        int right = right_raw - ADC_midpoint;
        if (left <= -2047 || left >= 2047 || right <= -2047 || right >= 2047) {
            extreme = true;
        }
        *sum_left += abs(left);
        *sum_right += abs(right);
        (*count)++;
    }

    if (extreme) {
        TickType_t now = xTaskGetTickCount();
        if ((now - s_last_extreme_warn) >= pdMS_TO_TICKS(2000)) {
            s_last_extreme_warn = now;
            ESP_LOGW(TAG, "Signal at extreme level (ADC rail)");
        }
    }
    if (*count == 0) {
        *count = 1;
    }
}
