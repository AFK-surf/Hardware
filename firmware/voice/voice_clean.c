// SPDX-License-Identifier: Apache-2.0
#include "memory_caps.h"
#include "voice_clean.h"

#include <math.h>
#include <string.h>

#include "esp_afe_config.h"
#include "esp_afe_sr_models.h"
#include "esp_ns.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "model_path.h"

// VAD timing inside the AFE. Speech must last 128 ms before vadnet reports it (fewer clicks and
// keyboard hits get through); silence is reported after 96 ms, the shortest it allows, because
// cue_voice adds its own hangover on top.
#define VAD_MIN_SPEECH_MS 128
#define VAD_MIN_NOISE_MS 96
// Denoised audio waiting to be taken, at 16 kHz: two AFE chunks plus a frame is plenty.
#define OUT_FIFO_SAMPLES 2048
// 16 -> 24 kHz: up 3, low-pass at 48 kHz, down 2. 16 taps per phase, cutoff just under 8 kHz.
#define UP 3
#define TAPS_PER_PHASE 16
#define TAPS (UP * TAPS_PER_PHASE)
#define CUTOFF_HZ 7600.0f
// malloc() threshold for internal RAM while the AFE is built (see voice_clean_init).
#define VOICE_CLEAN_INTERNAL_LIMIT 64

static const char *TAG = "clean";

static const esp_afe_sr_iface_t *s_afe;
static esp_afe_sr_data_t *s_data;
static int s_feed_chunk;
static int16_t *s_in;   // feed_chunk samples
static int s_in_len;
static int16_t *s_out;  // OUT_FIFO_SAMPLES denoised samples ...
static uint8_t *s_out_speech;  // ... and the VAD verdict for each
static int s_out_len;
static float s_taps[UP][TAPS_PER_PHASE];
static float s_hist[TAPS_PER_PHASE - 1 + VOICE_CLEAN_FRAME_SAMPLES];
static int64_t s_busy_us;

static esp_err_t create_afe(void);

static void design_upsampler(void)
{
    const float fs = (float)VOICE_CLEAN_RATE_HZ * UP;
    const float fc = CUTOFF_HZ / fs;
    const float mid = (TAPS - 1) / 2.0f;
    for (int n = 0; n < TAPS; ++n) {
        const float x = (float)n - mid;
        const float sinc = x == 0.0f ? 2.0f * fc : sinf(2.0f * (float)M_PI * fc * x) / ((float)M_PI * x);
        // Blackman window: ~-58 dB stopband is enough for images of speech.
        const float w = 0.42f - 0.5f * cosf(2.0f * (float)M_PI * n / (TAPS - 1)) +
                        0.08f * cosf(4.0f * (float)M_PI * n / (TAPS - 1));
        s_taps[n % UP][n / UP] = sinc * w * UP;
    }
}

// One 20 ms frame: 320 samples at 16 kHz in, 480 at 24 kHz out. 320 x 3 = 480 x 2, so every
// frame starts on the same phase and only the last TAPS_PER_PHASE - 1 inputs carry over.
static void upsample(const int16_t *in, int16_t *out)
{
    const int keep = TAPS_PER_PHASE - 1;
    for (int i = 0; i < VOICE_CLEAN_FRAME_SAMPLES; ++i) {
        s_hist[keep + i] = in[i];
    }
    for (int m = 0; m < VOICE_CLEAN_FRAME_SAMPLES * UP / 2; ++m) {
        const int k = 2 * m;  // index at 48 kHz
        const int phase = k % UP;
        const int newest = keep + k / UP;
        float acc = 0.0f;
        for (int t = 0; t < TAPS_PER_PHASE; ++t) {
            acc += s_taps[phase][t] * s_hist[newest - t];
        }
        const int v = (int)lrintf(acc);
        out[m] = (int16_t)(v > INT16_MAX ? INT16_MAX : v < INT16_MIN ? INT16_MIN : v);
    }
    memmove(s_hist, s_hist + VOICE_CLEAN_FRAME_SAMPLES, keep * sizeof(float));
}

esp_err_t voice_clean_init(void)
{
    if (s_data) {
        return ESP_OK;
    }
    // The resampler and FIFO come first: without the AFE they still carry the raw mic (pass-through).
    s_out = heap_caps_malloc(OUT_FIFO_SAMPLES * sizeof(int16_t), COMMA_BUFFER_CAPS);
    s_out_speech = heap_caps_malloc(OUT_FIFO_SAMPLES, COMMA_BUFFER_CAPS);
    if (!s_out || !s_out_speech) {
        return ESP_ERR_NO_MEM;
    }
    design_upsampler();
    const size_t internal_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    // ESP-SR and esp-dl allocate with plain malloc, which puts anything under
    // CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL (16 KB) in internal RAM: ~76 KB of it, more than a call can
    // spare. Send their long-lived buffers to PSRAM while the AFE is built.
    heap_caps_malloc_extmem_enable(VOICE_CLEAN_INTERNAL_LIMIT);
    esp_err_t err = create_afe();
    heap_caps_malloc_extmem_enable(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL);
    if (err != ESP_OK) {
        return err;
    }
    const size_t internal_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ESP_LOGI(TAG, "AFE ready: feed %d, fetch %d samples at %d Hz; internal RAM used %d B, free %u", s_feed_chunk,
             s_afe->get_fetch_chunksize(s_data), VOICE_CLEAN_RATE_HZ, (int)(internal_before - internal_after),
             (unsigned)internal_after);
    return ESP_OK;
}

static esp_err_t create_afe(void)
{
    srmodel_list_t *models = esp_srmodel_init("model");
    if (!models) {
        ESP_LOGE(TAG, "no models: is the \"model\" partition flashed?");
        return ESP_ERR_NOT_FOUND;
    }
    afe_config_t *cfg = afe_config_init("M", models, AFE_TYPE_VC, AFE_MODE_HIGH_PERF);
    if (!cfg) {
        return ESP_FAIL;
    }
    cfg->aec_init = false;  // the echo gate in cue_voice handles the speaker for now
    cfg->se_init = false;   // one mic
    cfg->ns_init = !COMMA_VOICE_NS_OFF;
    cfg->vad_init = true;
    cfg->vad_min_speech_ms = VAD_MIN_SPEECH_MS;
    cfg->vad_min_noise_ms = VAD_MIN_NOISE_MS;
    cfg->wakenet_init = false;
    // No AGC: it would raise the noise floor in pauses, the opposite of the point. The mic's fixed
    // x2.5 gain in board_audio stays.
    cfg->agc_init = false;
    cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    cfg->afe_linear_gain = 1.0f;
    cfg = afe_config_check(cfg);
    afe_config_print(cfg);
    s_afe = esp_afe_handle_from_config(cfg);
    s_data = s_afe ? s_afe->create_from_config(cfg) : NULL;
    afe_config_free(cfg);
    if (!s_data) {
        ESP_LOGE(TAG, "AFE create failed");
        return ESP_FAIL;
    }
    s_feed_chunk = s_afe->get_feed_chunksize(s_data);
    const int fetch_chunk = s_afe->get_fetch_chunksize(s_data);
    if (s_afe->get_samp_rate(s_data) != VOICE_CLEAN_RATE_HZ || s_afe->get_feed_channel_num(s_data) != 1 ||
        fetch_chunk > OUT_FIFO_SAMPLES / 2) {
        ESP_LOGE(TAG, "unexpected AFE shape: rate %d, channels %d, fetch %d", s_afe->get_samp_rate(s_data),
                 s_afe->get_feed_channel_num(s_data), fetch_chunk);
        s_afe->destroy(s_data);
        s_data = NULL;
        return ESP_FAIL;
    }
    s_in = heap_caps_malloc(s_feed_chunk * sizeof(int16_t), COMMA_BUFFER_CAPS);
    if (!s_in) {
        s_afe->destroy(s_data);
        s_data = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_afe->print_pipeline(s_data);
    return ESP_OK;
}

void voice_clean_reset(void)
{
    if (s_data) {
        s_afe->reset_buffer(s_data);
        s_afe->reset_vad(s_data);
    }
    s_in_len = 0;
    s_out_len = 0;
    memset(s_hist, 0, sizeof(s_hist));
    s_busy_us = 0;
}

void voice_clean_feed(const int16_t *mic16k)
{
    if (!s_data) {
        // No AFE (models missing or create failed): the raw mic, all of it called speech.
        if (s_out && s_out_len + VOICE_CLEAN_FRAME_SAMPLES <= OUT_FIFO_SAMPLES) {
            memcpy(s_out + s_out_len, mic16k, VOICE_CLEAN_FRAME_SAMPLES * sizeof(int16_t));
            memset(s_out_speech + s_out_len, 1, VOICE_CLEAN_FRAME_SAMPLES);
            s_out_len += VOICE_CLEAN_FRAME_SAMPLES;
        }
        return;
    }
    const int64_t start = esp_timer_get_time();
    int used = 0;
    while (used < VOICE_CLEAN_FRAME_SAMPLES) {
        int n = s_feed_chunk - s_in_len;
        if (n > VOICE_CLEAN_FRAME_SAMPLES - used) {
            n = VOICE_CLEAN_FRAME_SAMPLES - used;
        }
        memcpy(s_in + s_in_len, mic16k + used, n * sizeof(int16_t));
        s_in_len += n;
        used += n;
        if (s_in_len < s_feed_chunk) {
            break;
        }
        s_in_len = 0;
        s_afe->feed(s_data, s_in);
        // With one mic and no AEC the AFE has no task of its own: fetch runs NS and VAD on the chunk
        // just fed and returns at once.
        afe_fetch_result_t *res = s_afe->fetch_with_delay(s_data, pdMS_TO_TICKS(100));
        if (!res || res->ret_value == ESP_FAIL || res->data_size <= 0) {
            continue;
        }
        int got = res->data_size / (int)sizeof(int16_t);
        if (s_out_len + got > OUT_FIFO_SAMPLES) {
            // Only if nobody pops; never in a call. Keep the newest.
            s_out_len = 0;
        }
        memcpy(s_out + s_out_len, res->data, got * sizeof(int16_t));
        memset(s_out_speech + s_out_len, res->vad_state == VAD_SPEECH ? 1 : 0, got);
        s_out_len += got;
    }
    s_busy_us += esp_timer_get_time() - start;
}

bool voice_clean_pop(int16_t *out24k, bool *speech)
{
    if (!s_out || s_out_len < VOICE_CLEAN_FRAME_SAMPLES) {
        return false;
    }
    const int64_t start = esp_timer_get_time();
    bool any = false;
    for (int i = 0; i < VOICE_CLEAN_FRAME_SAMPLES && !any; ++i) {
        any = s_out_speech[i] != 0;
    }
    upsample(s_out, out24k);
    s_out_len -= VOICE_CLEAN_FRAME_SAMPLES;
    memmove(s_out, s_out + VOICE_CLEAN_FRAME_SAMPLES, s_out_len * sizeof(int16_t));
    memmove(s_out_speech, s_out_speech + VOICE_CLEAN_FRAME_SAMPLES, s_out_len);
    *speech = any;
    s_busy_us += esp_timer_get_time() - start;
    return true;
}

int64_t voice_clean_take_busy_us(void)
{
    int64_t busy = s_busy_us;
    s_busy_us = 0;
    return busy;
}

#ifdef VOICE_CLEAN_BENCH
// Dev only (`idf.py -B build-bench -DCOMMA_VOICE_CLEAN_BENCH=1 build`): times AFE variants on 3 s of
// pseudo-noise at boot and logs ms of CPU per second of audio.
typedef struct {
    const char *name;
    afe_mode_t mode;
    bool ns;
    bool ns_webrtc;
    bool vad;
} bench_variant_t;

void voice_clean_bench(void)
{
    static const bench_variant_t variants[] = {
        {"model ns+vadnet", AFE_MODE_HIGH_PERF, true, false, true},
        {"model ns only", AFE_MODE_HIGH_PERF, true, false, false},
        {"vadnet only", AFE_MODE_HIGH_PERF, false, false, true},
    };
    srmodel_list_t *models = esp_srmodel_init("model");
    uint32_t seed = 1;
    for (size_t v = 0; v < sizeof(variants) / sizeof(variants[0]); ++v) {
        const bench_variant_t *b = &variants[v];
        heap_caps_malloc_extmem_enable(VOICE_CLEAN_INTERNAL_LIMIT);
        afe_config_t *cfg = afe_config_init("M", models, AFE_TYPE_VC, b->mode);
        cfg->aec_init = false;
        cfg->se_init = false;
        cfg->agc_init = false;
        cfg->wakenet_init = false;
        cfg->ns_init = b->ns;
        if (b->ns_webrtc) {
            cfg->afe_ns_mode = AFE_NS_MODE_WEBRTC;
            cfg->ns_model_name = NULL;
        }
        cfg->vad_init = b->vad;
        cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
        cfg = afe_config_check(cfg);
        const esp_afe_sr_iface_t *afe = esp_afe_handle_from_config(cfg);
        esp_afe_sr_data_t *data = afe->create_from_config(cfg);
        afe_config_free(cfg);
        heap_caps_malloc_extmem_enable(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL);
        if (!data) {
            ESP_LOGW(TAG, "bench %s: create failed", b->name);
            continue;
        }
        const int chunk = afe->get_feed_chunksize(data);
        int16_t *in = heap_caps_malloc(chunk * sizeof(int16_t), COMMA_BUFFER_CAPS);
        const int chunks = 3 * VOICE_CLEAN_RATE_HZ / chunk;
        int64_t busy = 0;
        for (int c = 0; c < chunks; ++c) {
            for (int i = 0; i < chunk; ++i) {
                seed = seed * 1664525u + 1013904223u;
                in[i] = (int16_t)((int32_t)(seed >> 16) % 800 - 400);
            }
            const int64_t t0 = esp_timer_get_time();
            afe->feed(data, in);
            afe->fetch_with_delay(data, pdMS_TO_TICKS(100));
            busy += esp_timer_get_time() - t0;
        }
        ESP_LOGI(TAG, "bench %s: %lld ms CPU per s of audio", b->name, (long long)(busy / 1000 * VOICE_CLEAN_RATE_HZ / (chunks * chunk)));
        free(in);
        afe->destroy(data);
    }
    // Classic WebRTC NS outside the AFE, 10 ms frames.
    for (int level = 0; level <= 2; ++level) {
        ns_handle_t ns = ns_pro_create(10, level, VOICE_CLEAN_RATE_HZ);
        if (!ns) {
            continue;
        }
        int16_t in[160];
        int16_t out[160];
        int64_t busy = 0;
        for (int c = 0; c < 300; ++c) {
            for (int i = 0; i < 160; ++i) {
                seed = seed * 1664525u + 1013904223u;
                in[i] = (int16_t)((int32_t)(seed >> 16) % 800 - 400);
            }
            const int64_t t0 = esp_timer_get_time();
            ns_process(ns, in, out);
            busy += esp_timer_get_time() - t0;
        }
        ESP_LOGI(TAG, "bench webrtc ns_pro level %d: %lld ms CPU per s of audio", level, (long long)(busy / 3000));
        ns_destroy(ns);
    }
}
#endif
