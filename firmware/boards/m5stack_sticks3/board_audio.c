/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * M5Stack StickS3: ES8311 codec, one I2S port, full duplex; speaker amp enabled through the M5PM1.
 * The duplex stereo I2S setup, the mic on the left slot and the codec configuration (MCLK, the amp
 * through the PMIC) follow muse-gadget-sdk's board_m5stack_sticks3.c and muse_audio.c. The rest is
 * H3's Waveshare driver (ours): channels owned here, the codec through esp_codec_dev's codec
 * interface (registers only), filters and the resampler.
 *
 * Mic and speaker share the port's clock, so it runs at the speaker's 24 kHz (comma.voice.v1
 * pcm16_24k, played as received). Slot mode is stereo in both directions: the mic is one slot of
 * each frame (mic_path.c), playback writes the same sample to both slots. The mic is resampled to
 * MIC_SAMPLE_RATE_HZ (16 kHz for V2's AFE); with COMMA_VOICE_CLEAN off it stays at 24 kHz.
 *
 * On the S3, disabling the RX channel also stops TX on the same port, so RX stays enabled.
 *
 * A small reader task ("mic_rx", above the voice tasks) drains the RX DMA every 10 ms, takes the mic
 * slot, resamples, and keeps up to MIC_RING_MS of mic audio in a ring the voice task reads. The DMA
 * itself holds only 60 ms, and the voice task (AFE, sends) can be late by more than that, for example
 * when a call starts during the Connector's TLS handshake: the first board call lost 30 DMA blocks
 * that way (3 % of the audio). A deeper DMA would also delay the speaker (one setting for both
 * directions in duplex), so the ring is on the mic side only. The ring keeps only what arrives while
 * recording; starting a recording empties it.
 */

#include "board_audio.h"
#include "sdkconfig.h"
#include "board_pins.h"
#include "mic_path.h"
#include "pmic.h"

#include <limits.h>
#include <stddef.h>

#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "memory_caps.h"

#define STICK_I2S_RATE_HZ AUDIO_SAMPLE_RATE_HZ
#define STICK_CHANNELS 2
#define STICK_AUDIO_CHUNK_FRAMES 256
// 10 ms of DMA per descriptor at 24 kHz (240 stereo frames, 960 B).
#define STICK_DMA_FRAMES 240
#define STICK_DMA_DESCS 6
// ES8311 mic PGA. The reference runs this board at 30 dB; start 6 dB lower and tune against the
// T-Circle's mic_rms (~400 speaking at arm's length).
#define STICK_MIC_GAIN_DB 24.0f
// DAC volume in dB on esp_codec_dev's scale (after the hardware gain below). The reference's
// default volume 70 is about -15 dB on esp_codec_dev's default curve. At -15 dB the first board
// calls were too quiet by ear and echo_rms peaked at 1-2k (T-Circle median ~10k), so +9 dB.
#define STICK_SPEAKER_DB -6.0f
// The mic rate check: one log line this long after the mic opens.
#define STICK_RATE_CHECK_US (10LL * 1000000)
// Mic audio the reader task can hold for a late voice task (the T-Circle's DMA holds 160 ms).
#define MIC_RING_MS 320
#define MIC_RX_PRIORITY 7  // above the play/sender (6) and uplink (5) tasks
#define MIC_RX_STACK 3072

#if MIC_SAMPLE_RATE_HZ == 16000
#define STICK_RESAMPLE 1
#define STICK_MIC_DC_BLOCK_Q15 32603  // 12.8 Hz at 16 kHz, as on the T-Circle
#elif MIC_SAMPLE_RATE_HZ == STICK_I2S_RATE_HZ
#define STICK_RESAMPLE 0
#define STICK_MIC_DC_BLOCK_Q15 32659  // 12.8 Hz at 24 kHz
#else
#error "unsupported mic rate"
#endif
#define STICK_SPEAKER_DC_BLOCK_Q15 31755  // 120 Hz at 24 kHz, as on the T-Circle
#define STICK_SPEAKER_FADE_IN_SAMPLES 144
#define STICK_LIMIT_START 24576

_Static_assert(MIC_FRAME_SAMPLES <= MIC_PATH_MAX_FRAMES * 2 / 3 || !STICK_RESAMPLE,
               "one mic frame fits one resampler call");
_Static_assert(MIC_FRAME_SAMPLES <= MIC_PATH_MAX_FRAMES, "one mic frame fits the stereo buffer");

static const char *TAG = "audio";

static i2s_chan_handle_t s_tx = NULL;
static i2s_chan_handle_t s_rx = NULL;
static const audio_codec_if_t *s_codec = NULL;
static bool s_recording = false;
static bool s_speaker_enabled = false;
static int16_t s_out[STICK_AUDIO_CHUNK_FRAMES * STICK_CHANNELS];
static StreamBufferHandle_t s_ring;  // mono PCM16 at MIC_SAMPLE_RATE_HZ
static TaskHandle_t s_mic_rx_task;
static volatile bool s_ring_on = false;  // the reader task feeds the ring
static volatile uint32_t s_ring_lost;   // samples the ring had no room for
static int32_t s_mic_previous_input = 0;
static int32_t s_mic_highpass_state = 0;
static int32_t s_speaker_previous_input = 0;
static int32_t s_speaker_highpass_state = 0;
static uint32_t s_speaker_fade_samples = 0;
#if STICK_RESAMPLE
static mic_path_t s_mic_path;  // used by the reader task only (and the boot benchmark before it)
#endif
// Mic samples delivered since the recording started, for the one rate line per call; and from the
// RX DMA interrupt, blocks the hardware filled and blocks lost because nobody read them in time.
static int64_t s_rate_start_us;
static uint32_t s_rate_samples;
static bool s_rate_logged;
static volatile uint32_t s_rx_blocks;
static volatile uint32_t s_rx_overflows;
static uint32_t s_rate_blocks0;
static uint32_t s_rate_overflows0;
static uint32_t s_rate_ring_lost0;

static bool on_rx_block(i2s_chan_handle_t handle, i2s_event_data_t *event, void *ctx)
{
    (void)handle;
    (void)event;
    (void)ctx;
    ++s_rx_blocks;
    return false;
}

static bool on_rx_overflow(i2s_chan_handle_t handle, i2s_event_data_t *event, void *ctx)
{
    (void)handle;
    (void)event;
    (void)ctx;
    ++s_rx_overflows;
    return false;
}

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

static esp_err_t i2s_init(void)
{
    i2s_chan_config_t channel_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel_cfg.dma_desc_num = STICK_DMA_DESCS;
    channel_cfg.dma_frame_num = STICK_DMA_FRAMES;
    channel_cfg.auto_clear_after_cb = true;  // silence, not the last chunk again, when playback pauses
    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel_cfg, &s_tx, &s_rx), TAG, "new duplex channel");

    // Stereo, both directions: two samples per frame. (Mono slot mode on the S3's RX with both
    // slots active delivers both slots as mono samples: twice the rate.)
    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(STICK_I2S_RATE_HZ),  // MCLK 256 x 24 kHz = 6.144 MHz
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = STICK_I2S_MCLK,
            .bclk = STICK_I2S_BCLK,
            .ws = STICK_I2S_WS,
            .dout = STICK_I2S_DOUT,
            .din = STICK_I2S_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std_cfg), TAG, "init TX");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx, &std_cfg), TAG, "init RX");
    const i2s_event_callbacks_t rx_events = {
        .on_recv = on_rx_block,
        .on_recv_q_ovf = on_rx_overflow,
    };
    ESP_RETURN_ON_ERROR(i2s_channel_register_event_callback(s_rx, &rx_events, NULL), TAG, "RX events");
    // MCLK runs before the codec is configured.
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "enable TX");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx), TAG, "enable RX");
    return ESP_OK;
}

static esp_err_t codec_init(void)
{
    ESP_RETURN_ON_FALSE(stick_i2c_bus(), ESP_ERR_INVALID_STATE, TAG, "no I2C bus");
    audio_codec_i2c_cfg_t control_cfg = {
        .port = I2C_NUM_0,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = stick_i2c_bus(),
    };
    const audio_codec_ctrl_if_t *control_if = audio_codec_new_i2c_ctrl(&control_cfg);
    ESP_RETURN_ON_FALSE(control_if, ESP_ERR_NO_MEM, TAG, "codec control interface");
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(gpio_if, ESP_ERR_NO_MEM, TAG, "codec GPIO interface");

    es8311_codec_cfg_t codec_cfg = {
        .ctrl_if = control_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        // The amp is the PMIC's GPIO3, switched here (board_audio_set_speaker_enabled), not by the
        // codec driver, so a recording never turns it on.
        .pa_pin = GPIO_NUM_NC,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = {
            .pa_voltage = 5.0,
            .codec_dac_voltage = 3.3,
        },
    };
    // Opens the codec: soft reset and default registers. The amp is already off (pmic_init).
    s_codec = es8311_codec_new(&codec_cfg);
    ESP_RETURN_ON_FALSE(s_codec, ESP_FAIL, TAG, "ES8311 not found (is the PMIC's L3B rail on?)");

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = STICK_CHANNELS,
        .sample_rate = STICK_I2S_RATE_HZ,
    };
    ESP_RETURN_ON_FALSE(s_codec->set_fs(s_codec, &fs) == ESP_CODEC_DEV_OK, ESP_FAIL, TAG, "ES8311 rate");
    ESP_RETURN_ON_FALSE(s_codec->enable(s_codec, true) == ESP_CODEC_DEV_OK, ESP_FAIL, TAG, "ES8311 enable");
    ESP_RETURN_ON_FALSE(s_codec->set_mic_gain(s_codec, STICK_MIC_GAIN_DB) == ESP_CODEC_DEV_OK, ESP_FAIL, TAG,
                        "mic gain");
    ESP_RETURN_ON_FALSE(s_codec->set_vol(s_codec, STICK_SPEAKER_DB) == ESP_CODEC_DEV_OK, ESP_FAIL, TAG, "volume");
    ESP_RETURN_ON_FALSE(s_codec->mute(s_codec, true) == ESP_CODEC_DEV_OK, ESP_FAIL, TAG, "mute DAC");
    return ESP_OK;
}

#if STICK_RESAMPLE
// What the slot pick plus 24 -> 16 kHz costs on this CPU: one second of mic audio, timed at boot.
static void resample_benchmark(void)
{
    static int16_t s_in[MIC_PATH_MAX_FRAMES * STICK_CHANNELS];
    int16_t out[MIC_PATH_MAX_FRAMES * 2 / 3];  // 640 B on the main task's 8 KB stack
    for (int i = 0; i < MIC_PATH_MAX_FRAMES * STICK_CHANNELS; ++i) {
        s_in[i] = (int16_t)((i * 2731) & 0x3fff);
    }
    const int64_t start = esp_timer_get_time();
    for (int frame = 0; frame < STICK_I2S_RATE_HZ / MIC_PATH_MAX_FRAMES; ++frame) {
        mic_path_24k_to_16k(&s_mic_path, s_in, MIC_PATH_MAX_FRAMES, STICK_MIC_SLOT, out);
    }
    const int64_t took = esp_timer_get_time() - start;
    mic_path_reset(&s_mic_path);
    ESP_LOGI(TAG, "mic resampler: 1 s of audio in %lld us (%.2f%% of one core)", (long long)took, (double)took / 1e4);
}
#endif

// Drains the RX DMA one block (10 ms) at a time, whether recording or not, so the DMA never
// overruns; while recording, the mic slot goes into the ring at the pipeline's rate.
static void mic_rx_task(void *arg)
{
    (void)arg;
    static int16_t block[STICK_DMA_FRAMES * STICK_CHANNELS];
    static int16_t mono[STICK_DMA_FRAMES];
    _Static_assert(STICK_DMA_FRAMES % 3 == 0 && STICK_DMA_FRAMES <= MIC_PATH_MAX_FRAMES, "whole 3:2 groups");
    bool was_on = false;
    for (;;) {
        size_t got = 0;
        if (i2s_channel_read(s_rx, block, sizeof(block), &got, portMAX_DELAY) != ESP_OK || got != sizeof(block)) {
            continue;
        }
        const bool on = s_ring_on;
        if (on && !was_on) {
#if STICK_RESAMPLE
            mic_path_reset(&s_mic_path);
#endif
        }
        was_on = on;
        if (!on) {
            continue;
        }
#if STICK_RESAMPLE
        const int n = mic_path_24k_to_16k(&s_mic_path, block, STICK_DMA_FRAMES, STICK_MIC_SLOT, mono);
#else
        const int n = mic_take_slot(block, STICK_DMA_FRAMES, STICK_MIC_SLOT, mono);
#endif
        if (n <= 0) {
            continue;
        }
        const size_t bytes = (size_t)n * sizeof(int16_t);
        const size_t sent = xStreamBufferSend(s_ring, mono, bytes, 0);
        if (sent < bytes) {
            s_ring_lost += (uint32_t)((bytes - sent) / sizeof(int16_t));
        }
    }
}

esp_err_t board_audio_init(void)
{
    // The amp is off since pmic_init(); make sure, before the codec reset in codec_init().
    ESP_RETURN_ON_ERROR(pmic_speaker(false), TAG, "amp off");
    ESP_RETURN_ON_ERROR(i2s_init(), TAG, "I2S");
    ESP_RETURN_ON_ERROR(codec_init(), TAG, "ES8311");
#if STICK_RESAMPLE
    mic_path_init(&s_mic_path);
    resample_benchmark();
#endif
    s_recording = false;
    s_speaker_enabled = false;
    s_ring = xStreamBufferCreateWithCaps((size_t)MIC_SAMPLE_RATE_HZ * MIC_RING_MS / 1000 * sizeof(int16_t),
                                         sizeof(int16_t), COMMA_BUFFER_CAPS);
    ESP_RETURN_ON_FALSE(s_ring, ESP_ERR_NO_MEM, TAG, "mic ring");
    ESP_RETURN_ON_FALSE(xTaskCreate(mic_rx_task, "mic_rx", MIC_RX_STACK, NULL, MIC_RX_PRIORITY, &s_mic_rx_task) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "mic reader task");
    ESP_LOGI(TAG,
             "StickS3 audio ready: ES8311 duplex stereo I2S0 at %d Hz, mic slot %d to %d Hz, mic gain %.0f dB, "
             "speaker %.1f dB",
             STICK_I2S_RATE_HZ, STICK_MIC_SLOT, MIC_SAMPLE_RATE_HZ, (double)STICK_MIC_GAIN_DB, (double)STICK_SPEAKER_DB);
    return ESP_OK;
}

esp_err_t board_audio_set_speaker_enabled(bool enabled)
{
    if (!s_codec) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_speaker_enabled == enabled) {
        return ESP_OK;
    }
    if (enabled) {
        ESP_RETURN_ON_FALSE(s_codec->mute(s_codec, false) == ESP_CODEC_DEV_OK, ESP_FAIL, TAG, "unmute DAC");
        vTaskDelay(pdMS_TO_TICKS(2));
        ESP_RETURN_ON_ERROR(pmic_speaker(true), TAG, "amp on");
        s_speaker_previous_input = 0;
        s_speaker_highpass_state = 0;
        s_speaker_fade_samples = 0;
        vTaskDelay(pdMS_TO_TICKS(5));
    } else {
        // Amp first, so the mute never pops through it.
        ESP_RETURN_ON_ERROR(pmic_speaker(false), TAG, "amp off");
        vTaskDelay(pdMS_TO_TICKS(2));
        if (s_codec->mute(s_codec, true) != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "mute DAC failed");
        }
    }
    s_speaker_enabled = enabled;
    ESP_LOGI(TAG, "speaker amp %s via M5PM1", enabled ? "enabled" : "disabled");
    return ESP_OK;
}

// Read exactly `samples` mic samples from the ring, or fail.
static bool read_ring(int16_t *dst, int samples, uint32_t timeout_ms)
{
    size_t want = (size_t)samples * sizeof(int16_t);
    uint8_t *at = (uint8_t *)dst;
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (want > 0) {
        const TickType_t now = xTaskGetTickCount();
        if ((int32_t)(deadline - now) <= 0) {
            return false;
        }
        const size_t got = xStreamBufferReceive(s_ring, at, want, deadline - now);
        at += got;
        want -= got;
    }
    return true;
}

esp_err_t board_audio_set_recording(bool enabled)
{
    if (!s_rx || !s_ring) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_recording == enabled) {
        return ESP_OK;
    }
    if (!enabled) {
        // RX keeps running (it carries TX's clock); the reader task just stops filling the ring.
        s_ring_on = false;
        s_recording = false;
        return ESP_OK;
    }
    // The ring is idle (nobody reads or blocks on it), so it can be emptied; then 10 ms of settling
    // is dropped, as the T-Circle drops its first frame.
    const size_t stale = xStreamBufferBytesAvailable(s_ring);
    xStreamBufferReset(s_ring);
    s_ring_on = true;
    static int16_t settle[MIC_SAMPLE_RATE_HZ / 100];
    if (!read_ring(settle, MIC_SAMPLE_RATE_HZ / 100, 1000)) {
        s_ring_on = false;
        return ESP_FAIL;
    }
    s_mic_previous_input = 0;
    s_mic_highpass_state = 0;
    s_rate_start_us = esp_timer_get_time();
    s_rate_samples = 0;
    s_rate_logged = false;
    s_rate_blocks0 = s_rx_blocks;
    s_rate_overflows0 = s_rx_overflows;
    s_rate_ring_lost0 = s_ring_lost;
    s_recording = true;
    ESP_LOGI(TAG, "ES8311 microphone capture started; discarded %u stale and %u startup bytes", (unsigned)stale,
             (unsigned)sizeof(settle));
    return ESP_OK;
}

// Once per recording, STICK_RATE_CHECK_US after it started: samples delivered against the clock.
// Must be MIC_SAMPLE_RATE_HZ +-1 %; H3's Waveshare delivered ~1.9x.
static void rate_check(int n)
{
    s_rate_samples += (uint32_t)n;
    const int64_t elapsed = esp_timer_get_time() - s_rate_start_us;
    if (s_rate_logged || elapsed < STICK_RATE_CHECK_US) {
        return;
    }
    s_rate_logged = true;
    const uint32_t per_s = (uint32_t)((int64_t)s_rate_samples * 1000000 / elapsed);
    const uint32_t tolerance = MIC_SAMPLE_RATE_HZ / 100;
    const bool ok = per_s + tolerance >= MIC_SAMPLE_RATE_HZ && per_s <= MIC_SAMPLE_RATE_HZ + tolerance;
    // The I2S clock as the DMA saw it, and DMA blocks dropped because the reader was late.
    const uint32_t i2s_per_s =
        (uint32_t)((int64_t)(s_rx_blocks - s_rate_blocks0) * STICK_DMA_FRAMES * 1000000 / elapsed);
    ESP_LOG_LEVEL(ok ? ESP_LOG_INFO : ESP_LOG_ERROR, TAG,
                  "mic rate: %u samples in %lld ms = %u/s, want %d +-1%%: %s (I2S %u frames/s; lost: %u DMA blocks, "
                  "%u ring samples)",
                  (unsigned)s_rate_samples, (long long)(elapsed / 1000), (unsigned)per_s, MIC_SAMPLE_RATE_HZ,
                  ok ? "OK" : "WRONG", (unsigned)i2s_per_s, (unsigned)(s_rx_overflows - s_rate_overflows0),
                  (unsigned)(s_ring_lost - s_rate_ring_lost0));
#if CONFIG_COMMA_HEAP_DIAGNOSTICS
    ESP_LOGI("heap", "stack_free mic_rx=%u", (unsigned)uxTaskGetStackHighWaterMark(s_mic_rx_task));
#endif
}

int board_audio_read_mono(int16_t *mono_buf, int samples)
{
    if (!s_ring || !s_recording || !mono_buf || samples <= 0 || samples > MIC_FRAME_SAMPLES) {
        return -1;
    }
    const int n_out = samples;
    if (!read_ring(mono_buf, n_out, 1000)) {
        return -1;
    }
    for (int i = 0; i < n_out; ++i) {
        const int32_t input = mono_buf[i];
        const int32_t highpass = input - s_mic_previous_input +
                                 (int32_t)(((int64_t)s_mic_highpass_state * STICK_MIC_DC_BLOCK_Q15) >> 15);
        s_mic_previous_input = input;
        s_mic_highpass_state = highpass;
        mono_buf[i] = soft_limit_pcm16(highpass, STICK_LIMIT_START);
    }
    rate_check(n_out);
    return n_out;
}

esp_err_t board_audio_play_pcm16_mono(const int16_t *pcm, int samples)
{
    if (!s_tx || !pcm || samples <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(board_audio_set_speaker_enabled(true), TAG, "enable speaker");

    int offset = 0;
    while (offset < samples) {
        int frames = samples - offset;
        if (frames > STICK_AUDIO_CHUNK_FRAMES) {
            frames = STICK_AUDIO_CHUNK_FRAMES;
        }
        for (int i = 0; i < frames; ++i) {
            const int32_t input = pcm[offset + i];
            const int32_t highpass = input - s_speaker_previous_input +
                                     (int32_t)(((int64_t)s_speaker_highpass_state * STICK_SPEAKER_DC_BLOCK_Q15) >> 15);
            s_speaker_previous_input = input;
            s_speaker_highpass_state = highpass;
            int32_t shaped = highpass;
            if (s_speaker_fade_samples < STICK_SPEAKER_FADE_IN_SAMPLES) {
                shaped = shaped * (int32_t)s_speaker_fade_samples / STICK_SPEAKER_FADE_IN_SAMPLES;
                ++s_speaker_fade_samples;
            }
            // The same sample in both slots, as the reference writes it.
            s_out[2 * i] = s_out[2 * i + 1] = soft_limit_pcm16(shaped, STICK_LIMIT_START);
        }
        const size_t bytes = (size_t)frames * STICK_CHANNELS * sizeof(int16_t);
        size_t bytes_written = 0;
        ESP_RETURN_ON_ERROR(i2s_channel_write(s_tx, s_out, bytes, &bytes_written, 1000), TAG, "write speaker PCM");
        if (bytes_written != bytes) {
            return ESP_FAIL;
        }
        offset += frames;
    }
    return ESP_OK;
}
