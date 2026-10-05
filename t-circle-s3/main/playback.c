#include "playback.h"

#include "board_audio.h"

#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define PLAYBACK_SECONDS 2
#define PLAYBACK_BYTES (AUDIO_SAMPLE_RATE_HZ * PLAYBACK_SECONDS * (int)sizeof(int16_t))
#define MARK_MAX 32
#define PLAYED_MAX 32
#define MARK_NAME_LEN 80
// A sample above this counts as speech. The downlink may carry silence between replies.
#define SPEECH_PEAK 300
// Agent audio arrives in bursts (gaps up to ~1.5 s on office Wi-Fi). After the queue runs dry,
// playback waits for this much audio, or for a pause in arrivals, before it starts again.
#define PREBUFFER_BYTES (AUDIO_SAMPLE_RATE_HZ * (int)sizeof(int16_t) / 5)
#define PREBUFFER_WAIT_US (200 * 1000)
// Ringback while dialing, as in the Android app's ringback.wav: 440 + 480 Hz for 1 s, then 3 s off.
#define RING_ON_SAMPLES AUDIO_SAMPLE_RATE_HZ
#define RING_CYCLE_SAMPLES (AUDIO_SAMPLE_RATE_HZ * 4)
#define RING_FADE_SAMPLES (AUDIO_SAMPLE_RATE_HZ / 100)
#define RING_AMPLITUDE 2500.0f

static const char *TAG = "play";

typedef struct {
    char name[MARK_NAME_LEN];
    int64_t at;
} mark_t;

static uint8_t *s_buf;
static int64_t s_write_at;
static int64_t s_read_at;
static int s_filled;
static mark_t s_marks[MARK_MAX];
static int s_mark_count;
static char s_played[PLAYED_MAX][MARK_NAME_LEN];
static int s_played_count;
static int64_t s_dropped;
static SemaphoreHandle_t s_lock;
static int64_t s_last_audio_us;
static uint8_t s_carry;
static bool s_has_carry;
// Write position just past the last audio that held speech; the speaker is "speaking" until read passes it.
static int64_t s_speech_until;
static bool s_buffering = true;
static atomic_bool s_ringing;
static int s_ring_pos;
static int64_t s_last_push_us;
static int64_t s_rx_bytes;
static int s_rx_peak;

static void release_locked(void)
{
    int kept = 0;
    for (int i = 0; i < s_mark_count; ++i) {
        if (s_marks[i].at <= s_read_at) {
            if (s_played_count < PLAYED_MAX) {
                memcpy(s_played[s_played_count], s_marks[i].name, MARK_NAME_LEN);
                s_played_count++;
            } else {
                ESP_LOGW(TAG, "played-name queue full, dropped a mark");
            }
        } else {
            s_marks[kept++] = s_marks[i];
        }
    }
    s_mark_count = kept;
}

static int copy_out(int16_t *dst, int samples)
{
    int bytes = samples * (int)sizeof(int16_t);
    if (bytes > s_filled) {
        bytes = s_filled;
    }
    bytes -= bytes % (int)sizeof(int16_t);
    int offset = (int)(s_read_at % PLAYBACK_BYTES);
    int first = PLAYBACK_BYTES - offset;
    if (first > bytes) {
        first = bytes;
    }
    memcpy(dst, s_buf + offset, (size_t)first);
    if (bytes > first) {
        memcpy((uint8_t *)dst + first, s_buf, (size_t)(bytes - first));
    }
    s_read_at += bytes;
    s_filled -= bytes;
    if (bytes > 0) {
        s_last_audio_us = esp_timer_get_time();
    }
    release_locked();
    return bytes / (int)sizeof(int16_t);
}

static void fill_ringback(int16_t *out, int samples)
{
    for (int i = 0; i < samples; ++i) {
        int pos = s_ring_pos;
        s_ring_pos = (s_ring_pos + 1) % RING_CYCLE_SAMPLES;
        if (pos >= RING_ON_SAMPLES) {
            out[i] = 0;
            continue;
        }
        float t = (float)pos / AUDIO_SAMPLE_RATE_HZ;
        float fade = 1.0f;
        if (pos < RING_FADE_SAMPLES) {
            fade = (float)pos / RING_FADE_SAMPLES;
        } else if (pos > RING_ON_SAMPLES - RING_FADE_SAMPLES) {
            fade = (float)(RING_ON_SAMPLES - pos) / RING_FADE_SAMPLES;
        }
        float v = RING_AMPLITUDE * fade * (sinf(2.0f * (float)M_PI * 440.0f * t) + sinf(2.0f * (float)M_PI * 480.0f * t));
        out[i] = (int16_t)v;
    }
}

static void playback_task(void *arg)
{
    (void)arg;
    int16_t chunk[256];
    for (;;) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_buffering && (s_filled >= PREBUFFER_BYTES ||
                            (s_filled > 0 && esp_timer_get_time() - s_last_push_us > PREBUFFER_WAIT_US))) {
            s_buffering = false;
        }
        int samples = (s_buf && !s_buffering) ? copy_out(chunk, 256) : 0;
        if (samples == 0) {
            s_buffering = true;
        }
        int64_t last = s_last_audio_us;
        xSemaphoreGive(s_lock);
        if (s_ringing) {
            // The tone replaces whatever was queued (the server's paced silence), which is still
            // consumed above so its marks keep being answered.
            fill_ringback(chunk, 256);
            if (board_audio_play_pcm16_mono(chunk, 256) != ESP_OK) {
                ESP_LOGE(TAG, "speaker write failed");
            }
            continue;
        }
        if (samples > 0) {
            if (board_audio_play_pcm16_mono(chunk, samples) != ESP_OK) {
                ESP_LOGE(TAG, "speaker write failed");
            }
            continue;
        }
        if (last != 0 && esp_timer_get_time() - last > 150000) {
            board_audio_set_speaker_enabled(false);
            s_last_audio_us = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void playback_init(void)
{
    if (s_buf) {
        return;
    }
    s_buf = heap_caps_malloc(PLAYBACK_BYTES, MALLOC_CAP_SPIRAM);
    s_lock = xSemaphoreCreateMutex();
    if (!s_buf || !s_lock) {
        ESP_LOGE(TAG, "playback buffer alloc failed");
        return;
    }
    if (xTaskCreate(playback_task, "play", 4096, NULL, 6, NULL) != pdPASS) {
        ESP_LOGE(TAG, "playback task create failed");
    }
}

static void write_locked(const uint8_t *pcm, size_t len)
{
    size_t room = (size_t)(PLAYBACK_BYTES - s_filled);
    if (len > room) {
        s_dropped += (int64_t)(len - room);
        ESP_LOGW(TAG, "playback full, dropped %u bytes (total %lld)", (unsigned)(len - room),
                 (long long)s_dropped);
        len = room;
    }
    int offset = (int)(s_write_at % PLAYBACK_BYTES);
    size_t first = (size_t)(PLAYBACK_BYTES - offset);
    if (first > len) {
        first = len;
    }
    memcpy(s_buf + offset, pcm, first);
    if (len > first) {
        memcpy(s_buf, pcm + first, len - first);
    }
    int peak = 0;
    for (size_t i = 0; i + 1 < len; i += 2) {
        int16_t sample = (int16_t)(pcm[i] | (pcm[i + 1] << 8));
        int magnitude = sample < 0 ? -(int)sample : sample;
        if (magnitude > peak) {
            peak = magnitude;
        }
    }
    s_write_at += (int64_t)len;
    s_filled += (int)len;
    s_rx_bytes += (int64_t)len;
    if (peak > s_rx_peak) {
        s_rx_peak = peak;
    }
    if (peak >= SPEECH_PEAK) {
        s_speech_until = s_write_at;
        // The agent's first speech ends the ringback.
        s_ringing = false;
    }
}

// A binary frame may end mid-sample. The odd byte is kept and joined to the next frame.
void playback_push(const uint8_t *pcm, size_t len)
{
    if (!s_buf || !pcm || len == 0) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_has_carry) {
        uint8_t sample[2] = {s_carry, pcm[0]};
        write_locked(sample, 2);
        s_has_carry = false;
        ++pcm;
        --len;
    }
    if (len % 2) {
        s_carry = pcm[len - 1];
        s_has_carry = true;
        --len;
    }
    if (len > 0) {
        write_locked(pcm, len);
    }
    s_last_push_us = esp_timer_get_time();
    xSemaphoreGive(s_lock);
}

void playback_mark(const char *name)
{
    if (!s_lock || !name || name[0] == '\0') {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_mark_count >= MARK_MAX) {
        ESP_LOGW(TAG, "mark queue full");
    } else {
        mark_t *mark = &s_marks[s_mark_count++];
        memset(mark, 0, sizeof(*mark));
        strncpy(mark->name, name, MARK_NAME_LEN - 1);
        mark->at = s_write_at;
        release_locked();
    }
    xSemaphoreGive(s_lock);
}

void playback_clear(void)
{
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_read_at = s_write_at;
    s_filled = 0;
    s_has_carry = false;
    s_speech_until = s_write_at;
    s_buffering = true;
    for (int i = 0; i < s_mark_count; ++i) {
        s_marks[i].at = s_read_at;
    }
    release_locked();
    xSemaphoreGive(s_lock);
}

int playback_take_played(char names[][80], int max_names)
{
    if (!s_lock || max_names <= 0) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = s_played_count;
    if (n > max_names) {
        n = max_names;
    }
    for (int i = 0; i < n; ++i) {
        memcpy(names[i], s_played[i], MARK_NAME_LEN);
    }
    s_played_count -= n;
    if (s_played_count > 0) {
        memmove(s_played, s_played[n], (size_t)s_played_count * MARK_NAME_LEN);
    }
    xSemaphoreGive(s_lock);
    return n;
}

void playback_stats(int *queued_bytes, int *pending_marks)
{
    if (!s_lock) {
        *queued_bytes = 0;
        *pending_marks = 0;
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *queued_bytes = s_filled;
    *pending_marks = s_mark_count;
    xSemaphoreGive(s_lock);
}

void playback_ringback(bool on)
{
    s_ring_pos = 0;
    s_ringing = on;
}

bool playback_speaking(void)
{
    if (s_ringing) {
        return true;
    }
    if (!s_lock) {
        return false;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool speaking = s_read_at < s_speech_until;
    xSemaphoreGive(s_lock);
    return speaking;
}

void playback_take_rx(int *bytes, int *peak)
{
    if (!s_lock) {
        *bytes = 0;
        *peak = 0;
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *bytes = (int)s_rx_bytes;
    *peak = s_rx_peak;
    s_rx_bytes = 0;
    s_rx_peak = 0;
    xSemaphoreGive(s_lock);
}
