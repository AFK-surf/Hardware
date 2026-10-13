// SPDX-License-Identifier: Apache-2.0
// LilyGO T-Circle-S3 V1.1. PDM mic on I2S0, MAX98357A on I2S1.
// The speaker runs at 24 kHz for comma.voice.v1 pcm16_24k. The mic runs at MIC_SAMPLE_RATE_HZ:
// 16 kHz with the voice_clean noise suppressor (it upsamples to 24 kHz after), else 24 kHz.
// Filter corners match the 16 kHz T-Circle driver at either rate:
// mic DC block 12.8 Hz, mic low-pass 4.83 kHz, speaker DC block 120 Hz.
// The speaker fade-in stays 6 ms.

#include "board_audio.h"

#include <limits.h>
#include <stddef.h>

#include "driver/gpio.h"
#include "driver/i2s_pdm.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define T_CIRCLE_MIC_CLK      GPIO_NUM_9
#define T_CIRCLE_MIC_DATA     GPIO_NUM_8

#define T_CIRCLE_SPEAKER_BCLK GPIO_NUM_5
#define T_CIRCLE_SPEAKER_WS   GPIO_NUM_4
#define T_CIRCLE_SPEAKER_DATA GPIO_NUM_6
#define T_CIRCLE_SPEAKER_SD   GPIO_NUM_45

#define T_CIRCLE_AUDIO_CHUNK_FRAMES 256
#if MIC_SAMPLE_RATE_HZ == 16000
#define T_CIRCLE_MIC_DC_BLOCK_Q15 32603
#define T_CIRCLE_MIC_LOWPASS_Q15 27850
#else
#define T_CIRCLE_MIC_DC_BLOCK_Q15 32659
#define T_CIRCLE_MIC_LOWPASS_Q15 23517
#endif
#define T_CIRCLE_MIC_LIMIT_START 24576
#define T_CIRCLE_MIC_GAIN_NUMERATOR 5
#define T_CIRCLE_MIC_GAIN_DENOMINATOR 2
#define T_CIRCLE_SPEAKER_DC_BLOCK_Q15 31755
#define T_CIRCLE_SPEAKER_FADE_IN_SAMPLES 144
// 3.75x. The previous 0.375 left a speech peak of ~7500 near 9% of full scale.
#define T_CIRCLE_SPEAKER_GAIN_Q15 122880
#define T_CIRCLE_SPEAKER_LIMIT_START 24576

static const char *TAG = "audio";

static i2s_chan_handle_t s_microphone_rx = NULL;
static i2s_chan_handle_t s_speaker_tx = NULL;
static bool s_microphone_enabled = false;
static bool s_speaker_enabled = false;
static int16_t s_stereo_output[T_CIRCLE_AUDIO_CHUNK_FRAMES * 2];
static int16_t s_prime_buffer[MIC_FRAME_SAMPLES];
static int32_t s_mic_previous_input = 0;
static int32_t s_mic_highpass_state = 0;
static int32_t s_mic_lowpass_state = 0;
static int32_t s_speaker_previous_input = 0;
static int32_t s_speaker_highpass_state = 0;
static uint32_t s_speaker_fade_samples = 0;

static int16_t clamp_pcm16(int32_t value)
{
    if (value > INT16_MAX) {
        return INT16_MAX;
    }
    if (value < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)value;
}

static int16_t soft_limit_pcm16(int32_t value, int32_t knee)
{
    int32_t magnitude = value < 0 ? -value : value;
    if (magnitude > knee) {
        magnitude = knee + (magnitude - knee) / 4;
    }
    return clamp_pcm16(value < 0 ? -magnitude : magnitude);
}

static esp_err_t init_speaker(void)
{
    gpio_config_t amplifier_cfg = {
        .pin_bit_mask = 1ULL << T_CIRCLE_SPEAKER_SD,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&amplifier_cfg), TAG, "configure amplifier SD");
    ESP_RETURN_ON_ERROR(gpio_set_level(T_CIRCLE_SPEAKER_SD, 0), TAG, "disable amplifier");

    i2s_chan_config_t channel_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
    channel_cfg.dma_desc_num = 6;
    channel_cfg.auto_clear_after_cb = true;
    channel_cfg.dma_frame_num = T_CIRCLE_AUDIO_CHUNK_FRAMES;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel_cfg, &s_speaker_tx, NULL), TAG, "new speaker channel");

    i2s_std_config_t speaker_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = T_CIRCLE_SPEAKER_BCLK,
            .ws = T_CIRCLE_SPEAKER_WS,
            .dout = T_CIRCLE_SPEAKER_DATA,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_speaker_tx, &speaker_cfg), TAG, "init speaker I2S");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_speaker_tx), TAG, "enable speaker I2S");
    return ESP_OK;
}

static esp_err_t init_microphone(void)
{
    i2s_chan_config_t channel_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel_cfg.dma_desc_num = 8;
    channel_cfg.dma_frame_num = MIC_FRAME_SAMPLES;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel_cfg, NULL, &s_microphone_rx), TAG, "new PDM microphone channel");

    i2s_pdm_rx_config_t microphone_cfg = {
        .clk_cfg = I2S_PDM_RX_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_PDM_RX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .dins = {
                T_CIRCLE_MIC_DATA,
                I2S_GPIO_UNUSED,
                I2S_GPIO_UNUSED,
                I2S_GPIO_UNUSED,
            },
            .clk = T_CIRCLE_MIC_CLK,
            .invert_flags = {
                .clk_inv = false,
            },
        },
    };
    return i2s_channel_init_pdm_rx_mode(s_microphone_rx, &microphone_cfg);
}

esp_err_t board_audio_init(void)
{
    ESP_RETURN_ON_ERROR(init_speaker(), TAG, "MAX98357A");
    ESP_RETURN_ON_ERROR(init_microphone(), TAG, "PDM microphone");
    s_microphone_enabled = false;
    s_speaker_enabled = false;
    ESP_LOGI(TAG, "T-Circle audio ready: PDM RX I2S0 at %d Hz, MAX98357A TX I2S1 at %d Hz, speaker gain 3.75",
             MIC_SAMPLE_RATE_HZ, AUDIO_SAMPLE_RATE_HZ);
    return ESP_OK;
}

esp_err_t board_audio_set_speaker_enabled(bool enabled)
{
    if (!s_speaker_tx) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_speaker_enabled == enabled) {
        return ESP_OK;
    }

    esp_err_t err = gpio_set_level(T_CIRCLE_SPEAKER_SD, enabled ? 1 : 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set amplifier GPIO failed: %s", esp_err_to_name(err));
        return err;
    }
    s_speaker_enabled = enabled;
    if (enabled) {
        s_speaker_previous_input = 0;
        s_speaker_highpass_state = 0;
        s_speaker_fade_samples = 0;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    ESP_LOGI(TAG, "MAX98357A amplifier %s via GPIO%d", enabled ? "enabled" : "disabled", T_CIRCLE_SPEAKER_SD);
    return ESP_OK;
}

esp_err_t board_audio_set_recording(bool enabled)
{
    if (!s_microphone_rx) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_microphone_enabled == enabled) {
        return ESP_OK;
    }

    if (!enabled) {
        ESP_RETURN_ON_ERROR(i2s_channel_disable(s_microphone_rx), TAG, "disable PDM microphone");
        s_microphone_enabled = false;
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_microphone_rx), TAG, "enable PDM microphone");
    s_microphone_enabled = true;
    s_mic_previous_input = 0;
    s_mic_highpass_state = 0;
    s_mic_lowpass_state = 0;

    size_t bytes_read = 0;
    esp_err_t err = i2s_channel_read(s_microphone_rx, s_prime_buffer, sizeof(s_prime_buffer), &bytes_read, 1000);
    if (err != ESP_OK || bytes_read == 0) {
        i2s_channel_disable(s_microphone_rx);
        s_microphone_enabled = false;
        return err == ESP_OK ? ESP_FAIL : err;
    }
    ESP_LOGI(TAG, "PDM microphone capture started; discarded %u startup bytes", (unsigned)bytes_read);
    return ESP_OK;
}

int board_audio_read_mono(int16_t *mono_buf, int samples)
{
    if (!s_microphone_rx || !s_microphone_enabled || !mono_buf || samples <= 0 || samples > MIC_FRAME_SAMPLES) {
        return -1;
    }

    size_t bytes_read = 0;
    esp_err_t err = i2s_channel_read(s_microphone_rx, mono_buf, (size_t)samples * sizeof(int16_t), &bytes_read, 1000);
    if (err != ESP_OK || bytes_read == 0) {
        return -1;
    }

    const int samples_read = (int)(bytes_read / sizeof(int16_t));
    for (int i = 0; i < samples_read; ++i) {
        const int32_t input = mono_buf[i];
        const int32_t highpass = input - s_mic_previous_input +
                                 (int32_t)(((int64_t)s_mic_highpass_state * T_CIRCLE_MIC_DC_BLOCK_Q15) >> 15);
        s_mic_previous_input = input;
        s_mic_highpass_state = highpass;

        s_mic_lowpass_state += (int32_t)(((int64_t)(highpass - s_mic_lowpass_state) * T_CIRCLE_MIC_LOWPASS_Q15) >> 15);
        const int32_t gained = s_mic_lowpass_state * T_CIRCLE_MIC_GAIN_NUMERATOR / T_CIRCLE_MIC_GAIN_DENOMINATOR;
        mono_buf[i] = soft_limit_pcm16(gained, T_CIRCLE_MIC_LIMIT_START);
    }
    return samples_read;
}

esp_err_t board_audio_play_pcm16_mono(const int16_t *pcm, int samples)
{
    if (!s_speaker_tx || !pcm || samples <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(board_audio_set_speaker_enabled(true), TAG, "enable amplifier");

    int offset = 0;
    while (offset < samples) {
        int frames = samples - offset;
        if (frames > T_CIRCLE_AUDIO_CHUNK_FRAMES) {
            frames = T_CIRCLE_AUDIO_CHUNK_FRAMES;
        }

        for (int i = 0; i < frames; ++i) {
            const int32_t input = pcm[offset + i];
            const int32_t highpass = input - s_speaker_previous_input +
                                     (int32_t)(((int64_t)s_speaker_highpass_state * T_CIRCLE_SPEAKER_DC_BLOCK_Q15) >> 15);
            s_speaker_previous_input = input;
            s_speaker_highpass_state = highpass;

            int32_t shaped = (int32_t)(((int64_t)highpass * T_CIRCLE_SPEAKER_GAIN_Q15) >> 15);
            if (s_speaker_fade_samples < T_CIRCLE_SPEAKER_FADE_IN_SAMPLES) {
                shaped = shaped * (int32_t)s_speaker_fade_samples / T_CIRCLE_SPEAKER_FADE_IN_SAMPLES;
                ++s_speaker_fade_samples;
            }
            const int16_t sample = soft_limit_pcm16(shaped, T_CIRCLE_SPEAKER_LIMIT_START);
            s_stereo_output[2 * i] = sample;
            s_stereo_output[2 * i + 1] = sample;
        }

        size_t bytes_written = 0;
        ESP_RETURN_ON_ERROR(i2s_channel_write(s_speaker_tx, s_stereo_output, (size_t)frames * 2 * sizeof(int16_t),
                                              &bytes_written, 1000),
                            TAG, "write speaker PCM");
        if (bytes_written == 0) {
            return ESP_FAIL;
        }
        offset += frames;
    }
    return ESP_OK;
}
