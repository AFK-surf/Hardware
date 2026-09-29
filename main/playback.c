#include "playback.h"

#include "board_audio.h"

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

static void playback_task(void *arg)
{
    (void)arg;
    int16_t chunk[256];
    for (;;) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        int samples = s_buf ? copy_out(chunk, 256) : 0;
        int64_t last = s_last_audio_us;
        xSemaphoreGive(s_lock);
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

void playback_push(const uint8_t *pcm, size_t len)
{
    if (!s_buf || !pcm || len < 2) {
        return;
    }
    len -= len % 2;
    xSemaphoreTake(s_lock, portMAX_DELAY);
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
    s_write_at += (int64_t)len;
    s_filled += (int)len;
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
