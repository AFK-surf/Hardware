// Waveshare ESP32-S3-Touch-AMOLED-1.32: ES8311 codec, one I2S port, full duplex, PA on GPIO46.
//
// Mic and speaker share the port's clock, so it runs at the speaker's 24 kHz (comma.voice.v1
// pcm16_24k, played as received). The mic is read at 24 kHz and resampled to MIC_SAMPLE_RATE_HZ
// (16 kHz for V2's AFE) here; with COMMA_VOICE_CLEAN off the mic rate is 24 kHz and nothing is
// resampled.
//
// The ES8311 is set up through esp_codec_dev's codec interface (registers only); this file owns the
// I2S channels and the PA pin. On the S3, disabling the RX channel also stops TX on the same port,
// so RX stays enabled; starting a recording drops the samples that piled up meanwhile.
//
// Codec setup sequence and gains follow our voice-box-agent project, which ran this board.

#include "board_audio.h"
#include "board_pins.h"
#include "resample.h"

#include <limits.h>
#include <stddef.h>

#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define WS_I2S_RATE_HZ AUDIO_SAMPLE_RATE_HZ
#define WS_AUDIO_CHUNK_FRAMES 256
// 10 ms of DMA per descriptor at 24 kHz.
#define WS_DMA_FRAMES 240
#define WS_DMA_DESCS 6
// ES8311 microphone PGA. 24 dB is what voice-box-agent used for speech recognition at arm's length.
#define WS_MIC_GAIN_DB 24.0f
// DAC volume in dB (esp_codec_dev scale, after the hardware gain below). voice-box-agent used
// volume 95 on its 0..100 curve, -2.5 dB. Start ~8 dB quieter; tune against echo_rms in the H3 test.
#define WS_SPEAKER_DB -10.0f

#if MIC_SAMPLE_RATE_HZ == 16000
#define WS_RESAMPLE 1
#define WS_MIC_DC_BLOCK_Q15 32603  // 12.8 Hz at 16 kHz, as on the T-Circle
#elif MIC_SAMPLE_RATE_HZ == WS_I2S_RATE_HZ
#define WS_RESAMPLE 0
#define WS_MIC_DC_BLOCK_Q15 32659  // 12.8 Hz at 24 kHz
#else
#error "unsupported mic rate"
#endif
#define WS_SPEAKER_DC_BLOCK_Q15 31755  // 120 Hz at 24 kHz, as on the T-Circle
#define WS_SPEAKER_FADE_IN_SAMPLES 144
#define WS_LIMIT_START 24576

static const char *TAG = "audio";

static i2s_chan_handle_t s_tx = NULL;
static i2s_chan_handle_t s_rx = NULL;
static const audio_codec_if_t *s_codec = NULL;
static bool s_recording = false;
static bool s_speaker_enabled = false;
static int16_t s_out[WS_AUDIO_CHUNK_FRAMES];
static int32_t s_mic_previous_input = 0;
static int32_t s_mic_highpass_state = 0;
static int32_t s_speaker_previous_input = 0;
static int32_t s_speaker_highpass_state = 0;
static uint32_t s_speaker_fade_samples = 0;
#if WS_RESAMPLE
static resample_3to2_t s_resample;
static int16_t s_in24[RESAMPLE_MAX_IN];
#endif

_Static_assert(MIC_FRAME_SAMPLES <= RESAMPLE_MAX_IN * 2 / 3 || !WS_RESAMPLE, "one mic frame fits one resampler call");

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

static esp_err_t pa_init(void)
{
    const gpio_config_t pa_cfg = {
        .pin_bit_mask = 1ULL << WS_PA,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_set_level(WS_PA, 0), TAG, "PA low");
    ESP_RETURN_ON_ERROR(gpio_config(&pa_cfg), TAG, "configure PA");
    return gpio_set_level(WS_PA, 0);
}

static esp_err_t i2s_init(void)
{
    i2s_chan_config_t channel_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    channel_cfg.dma_desc_num = WS_DMA_DESCS;
    channel_cfg.dma_frame_num = WS_DMA_FRAMES;
    channel_cfg.auto_clear_after_cb = true;  // silence, not the last chunk again, when playback pauses
    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel_cfg, &s_tx, &s_rx), TAG, "new duplex channel");

    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(WS_I2S_RATE_HZ),  // MCLK 256 x 24 kHz = 6.144 MHz
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = WS_I2S_MCLK,
            .bclk = WS_I2S_SCLK,
            .ws = WS_I2S_WS,
            .dout = WS_I2S_DOUT,
            .din = WS_I2S_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx, &std_cfg), TAG, "init TX");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx, &std_cfg), TAG, "init RX");
    // MCLK runs before the codec is configured.
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx), TAG, "enable TX");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx), TAG, "enable RX");
    return ESP_OK;
}

static esp_err_t codec_init(void)
{
    ESP_RETURN_ON_FALSE(ws_i2c_bus(), ESP_ERR_INVALID_STATE, TAG, "no I2C bus");
    audio_codec_i2c_cfg_t control_cfg = {
        .port = I2C_NUM_0,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = ws_i2c_bus(),
    };
    const audio_codec_ctrl_if_t *control_if = audio_codec_new_i2c_ctrl(&control_cfg);
    ESP_RETURN_ON_FALSE(control_if, ESP_ERR_NO_MEM, TAG, "codec control interface");
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(gpio_if, ESP_ERR_NO_MEM, TAG, "codec GPIO interface");

    es8311_codec_cfg_t codec_cfg = {
        .ctrl_if = control_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = GPIO_NUM_NC,  // GPIO46 is ours (pa_init), so recording can keep the PA off
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
    // Opens the codec: soft reset and default registers. The PA is already low.
    s_codec = es8311_codec_new(&codec_cfg);
    ESP_RETURN_ON_FALSE(s_codec, ESP_FAIL, TAG, "ES8311 not found");

    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 1,
        .sample_rate = WS_I2S_RATE_HZ,
    };
    ESP_RETURN_ON_FALSE(s_codec->set_fs(s_codec, &fs) == ESP_CODEC_DEV_OK, ESP_FAIL, TAG, "ES8311 rate");
    ESP_RETURN_ON_FALSE(s_codec->enable(s_codec, true) == ESP_CODEC_DEV_OK, ESP_FAIL, TAG, "ES8311 enable");
    ESP_RETURN_ON_FALSE(s_codec->set_mic_gain(s_codec, WS_MIC_GAIN_DB) == ESP_CODEC_DEV_OK, ESP_FAIL, TAG,
                        "mic gain");
    ESP_RETURN_ON_FALSE(s_codec->set_vol(s_codec, WS_SPEAKER_DB) == ESP_CODEC_DEV_OK, ESP_FAIL, TAG, "volume");
    ESP_RETURN_ON_FALSE(s_codec->mute(s_codec, true) == ESP_CODEC_DEV_OK, ESP_FAIL, TAG, "mute DAC");
    return ESP_OK;
}

#if WS_RESAMPLE
// What the 24 -> 16 kHz resampler costs on this CPU: one second of mic audio, timed once at boot.
static void resample_benchmark(void)
{
    int16_t out[RESAMPLE_MAX_IN * 2 / 3];  // 640 B on the main task's 8 KB stack
    for (int i = 0; i < RESAMPLE_MAX_IN; ++i) {
        s_in24[i] = (int16_t)((i * 2731) & 0x3fff);
    }
    const int64_t start = esp_timer_get_time();
    for (int frame = 0; frame < WS_I2S_RATE_HZ / RESAMPLE_MAX_IN; ++frame) {
        resample_3to2(&s_resample, s_in24, RESAMPLE_MAX_IN, out);
    }
    const int64_t took = esp_timer_get_time() - start;
    resample_3to2_reset(&s_resample);
    ESP_LOGI(TAG, "mic resampler: 1 s of audio in %lld us (%.2f%% of one core)", (long long)took, (double)took / 1e4);
}
#endif

esp_err_t board_audio_init(void)
{
    // PA off before anything else, and before the codec reset in codec_init().
    ESP_RETURN_ON_ERROR(pa_init(), TAG, "PA");
    ESP_RETURN_ON_ERROR(i2s_init(), TAG, "I2S");
    ESP_RETURN_ON_ERROR(codec_init(), TAG, "ES8311");
#if WS_RESAMPLE
    resample_3to2_init(&s_resample);
    resample_benchmark();
#endif
    s_recording = false;
    s_speaker_enabled = false;
    ESP_LOGI(TAG, "Waveshare audio ready: ES8311 duplex I2S0 at %d Hz, mic to %d Hz, mic gain %.0f dB, speaker %.1f dB",
             WS_I2S_RATE_HZ, MIC_SAMPLE_RATE_HZ, (double)WS_MIC_GAIN_DB, (double)WS_SPEAKER_DB);
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
        ESP_RETURN_ON_ERROR(gpio_set_level(WS_PA, 1), TAG, "PA on");
        s_speaker_previous_input = 0;
        s_speaker_highpass_state = 0;
        s_speaker_fade_samples = 0;
        vTaskDelay(pdMS_TO_TICKS(5));
    } else {
        // PA first, so the mute never pops through it.
        ESP_RETURN_ON_ERROR(gpio_set_level(WS_PA, 0), TAG, "PA off");
        vTaskDelay(pdMS_TO_TICKS(2));
        if (s_codec->mute(s_codec, true) != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "mute DAC failed");
        }
    }
    s_speaker_enabled = enabled;
    ESP_LOGI(TAG, "speaker PA %s via GPIO%d", enabled ? "enabled" : "disabled", WS_PA);
    return ESP_OK;
}

// Read exactly `samples` 24 kHz samples, or fail.
static bool read_i2s(int16_t *dst, int samples, uint32_t timeout_ms)
{
    size_t want = (size_t)samples * sizeof(int16_t);
    uint8_t *at = (uint8_t *)dst;
    while (want > 0) {
        size_t got = 0;
        const esp_err_t err = i2s_channel_read(s_rx, at, want, &got, timeout_ms);
        if (got == 0 || (err != ESP_OK && err != ESP_ERR_TIMEOUT)) {
            return false;
        }
        at += got;
        want -= got;
    }
    return true;
}

esp_err_t board_audio_set_recording(bool enabled)
{
    if (!s_rx) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_recording == enabled) {
        return ESP_OK;
    }
    if (!enabled) {
        // RX keeps running (it carries TX's clock); reads just stop.
        s_recording = false;
        return ESP_OK;
    }
    // Drop what the DMA collected while nobody read, then one frame of settling, as on the T-Circle.
    static int16_t scratch[WS_DMA_FRAMES];
    size_t dropped = 0;
    for (int i = 0; i < WS_DMA_DESCS + 2; ++i) {
        size_t got = 0;
        i2s_channel_read(s_rx, scratch, sizeof(scratch), &got, 0);
        if (got == 0) {
            break;
        }
        dropped += got;
    }
    if (!read_i2s(scratch, WS_DMA_FRAMES, 1000)) {
        return ESP_FAIL;
    }
    s_mic_previous_input = 0;
    s_mic_highpass_state = 0;
#if WS_RESAMPLE
    resample_3to2_reset(&s_resample);
#endif
    s_recording = true;
    ESP_LOGI(TAG, "ES8311 microphone capture started; discarded %u stale and %u startup bytes", (unsigned)dropped,
             (unsigned)sizeof(scratch));
    return ESP_OK;
}

int board_audio_read_mono(int16_t *mono_buf, int samples)
{
    if (!s_rx || !s_recording || !mono_buf || samples <= 0 || samples > MIC_FRAME_SAMPLES) {
        return -1;
    }
#if WS_RESAMPLE
    // Whole 3:2 groups: an even count out (the voice task always asks for a whole frame).
    const int n_out = samples & ~1;
    if (n_out == 0) {
        return -1;
    }
    const int n_in = n_out / 2 * 3;
    if (!read_i2s(s_in24, n_in, 1000)) {
        return -1;
    }
    if (resample_3to2(&s_resample, s_in24, n_in, mono_buf) != n_out) {
        return -1;
    }
#else
    const int n_out = samples;
    if (!read_i2s(mono_buf, n_out, 1000)) {
        return -1;
    }
#endif
    for (int i = 0; i < n_out; ++i) {
        const int32_t input = mono_buf[i];
        const int32_t highpass = input - s_mic_previous_input +
                                 (int32_t)(((int64_t)s_mic_highpass_state * WS_MIC_DC_BLOCK_Q15) >> 15);
        s_mic_previous_input = input;
        s_mic_highpass_state = highpass;
        mono_buf[i] = soft_limit_pcm16(highpass, WS_LIMIT_START);
    }
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
        if (frames > WS_AUDIO_CHUNK_FRAMES) {
            frames = WS_AUDIO_CHUNK_FRAMES;
        }
        for (int i = 0; i < frames; ++i) {
            const int32_t input = pcm[offset + i];
            const int32_t highpass = input - s_speaker_previous_input +
                                     (int32_t)(((int64_t)s_speaker_highpass_state * WS_SPEAKER_DC_BLOCK_Q15) >> 15);
            s_speaker_previous_input = input;
            s_speaker_highpass_state = highpass;
            int32_t shaped = highpass;
            if (s_speaker_fade_samples < WS_SPEAKER_FADE_IN_SAMPLES) {
                shaped = shaped * (int32_t)s_speaker_fade_samples / WS_SPEAKER_FADE_IN_SAMPLES;
                ++s_speaker_fade_samples;
            }
            s_out[i] = soft_limit_pcm16(shaped, WS_LIMIT_START);
        }
        size_t bytes_written = 0;
        ESP_RETURN_ON_ERROR(i2s_channel_write(s_tx, s_out, (size_t)frames * sizeof(int16_t), &bytes_written, 1000),
                            TAG, "write speaker PCM");
        if (bytes_written == 0) {
            return ESP_FAIL;
        }
        offset += frames;
    }
    return ESP_OK;
}
