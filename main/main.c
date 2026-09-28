#include "board_audio.h"
#include "board_touch.h"
#include "display.h"

#include <stdint.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";

#define RECORD_SECONDS 3
#define RECORD_SAMPLES (AUDIO_SAMPLE_RATE_HZ * RECORD_SECONDS)

static int isqrt_u64(uint64_t x)
{
    uint64_t op = x;
    uint64_t res = 0;
    uint64_t one = 1ULL << 62;
    while (one > op) {
        one >>= 2;
    }
    while (one != 0) {
        if (op >= res + one) {
            op -= res + one;
            res = (res >> 1) + one;
        } else {
            res >>= 1;
        }
        one >>= 2;
    }
    return (int)res;
}

static void log_pcm(const char *what, const int16_t *pcm, int samples)
{
    int32_t peak = 0;
    uint64_t sumsq = 0;
    for (int i = 0; i < samples; ++i) {
        int32_t sample = pcm[i];
        int32_t magnitude = sample < 0 ? -sample : sample;
        if (magnitude > peak) {
            peak = magnitude;
        }
        sumsq += (uint64_t)(sample * sample);
    }
    int rms = samples > 0 ? isqrt_u64(sumsq / (uint64_t)samples) : 0;
    ESP_LOGI(TAG, "%s: %d samples, %d ms, peak %ld, rms %d", what, samples,
             samples * 1000 / AUDIO_SAMPLE_RATE_HZ, (long)peak, rms);
}

static void logo_task(void *arg)
{
    (void)arg;
    display_run_logo();
}

static void loopback_task(void *arg)
{
    int16_t *buf = arg;
    static int16_t silence[AUDIO_FRAME_SAMPLES];
    int filled = 0;
    enum { IDLE, RECORD, HOLD_FULL, PLAY } state = IDLE;

    board_audio_set_speaker_enabled(false);
    if (board_audio_set_recording(true) == ESP_OK) {
        int probe = 0;
        for (int i = 0; i < 10 && probe + AUDIO_FRAME_SAMPLES <= RECORD_SAMPLES; ++i) {
            int n = board_audio_read_mono(buf + probe, AUDIO_FRAME_SAMPLES);
            if (n <= 0) {
                break;
            }
            probe += n;
        }
        board_audio_set_recording(false);
        log_pcm("mic floor", buf, probe);
    } else {
        ESP_LOGE(TAG, "microphone probe failed");
    }

    ESP_LOGI(TAG, "hold BOOT to record %d s, release to play", RECORD_SECONDS);

    for (;;) {
        board_touch_poll();
        const bool held = board_touch_boot_held();

        if (state == IDLE) {
            if (!held) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            board_audio_set_speaker_enabled(false);
            if (board_audio_set_recording(true) != ESP_OK) {
                ESP_LOGE(TAG, "microphone start failed");
                vTaskDelay(pdMS_TO_TICKS(200));
                continue;
            }
            filled = 0;
            state = RECORD;
            ESP_LOGI(TAG, "recording");
            continue;
        }

        if (state == RECORD) {
            if (held && filled + AUDIO_FRAME_SAMPLES <= RECORD_SAMPLES) {
                int n = board_audio_read_mono(buf + filled, AUDIO_FRAME_SAMPLES);
                if (n <= 0) {
                    ESP_LOGE(TAG, "microphone read failed");
                    board_audio_set_recording(false);
                    filled = 0;
                    state = IDLE;
                    continue;
                }
                filled += n;
                continue;
            }
            board_audio_set_recording(false);
            log_pcm("recorded", buf, filled);
            state = held ? HOLD_FULL : PLAY;
            continue;
        }

        if (state == HOLD_FULL) {
            if (held) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            state = PLAY;
        }

        if (filled > 0) {
            ESP_LOGI(TAG, "playing");
            if (board_audio_play_pcm16_mono(buf, filled) != ESP_OK) {
                ESP_LOGE(TAG, "playback failed");
            }
            board_audio_play_pcm16_mono(silence, AUDIO_FRAME_SAMPLES);
        }
        board_audio_set_speaker_enabled(false);
        state = IDLE;
    }
}

void app_main(void)
{
    const size_t psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "PSRAM %u bytes", (unsigned)psram);

    const esp_err_t audio = board_audio_init();
    if (audio != ESP_OK) {
        ESP_LOGE(TAG, "audio init failed: %s", esp_err_to_name(audio));
    }
    board_touch_init();
    ESP_ERROR_CHECK(display_init());

    int16_t *record = heap_caps_malloc(RECORD_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!record) {
        ESP_LOGE(TAG, "record buffer alloc failed");
    } else if (xTaskCreate(loopback_task, "loopback", 8192, record, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "loopback task create failed");
    }
    if (xTaskCreate(logo_task, "logo", 8192, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "logo task create failed");
        display_run_logo();
    }
}
