#include "cue_voice.h"

#include "board_audio.h"
#include "board_touch.h"
#include "playback.h"
#include "secrets.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#define WS_OP_CONT 0x0
#define WS_OP_TEXT 0x1
#define WS_OP_BIN 0x2
#define WS_OP_CLOSE 0x8
#define WS_OP_PING 0x9
#define WS_OP_PONG 0xA

#define RX_MAX (16 * 1024)
#define FRAME_BYTES (AUDIO_FRAME_SAMPLES * (int)sizeof(int16_t))
#define IDLE_HANGUP_US (60LL * 1000000)
// No echo canceller (GPT-Live does none either): the mic sends silence while agent speech plays
// and for this long after.
#define ECHO_TAIL_US (300LL * 1000)
// Wire format. pcm16_24k is the default. pcmu_8k (G.711 mu-law, 8 kB/s each way instead of 48)
// connected on staging but the agent never spoke or heard the caller (2026-10-05), so it stays off.
// With pcmu the board still runs at 24 kHz: mic decimated 3:1 up, agent audio interpolated 1:3 down.
#define CUE_VOICE_PCMU 0
#if CUE_VOICE_PCMU
#define WIRE_FORMAT "pcmu_8k"
#define WIRE_RATE_HZ 8000
#define WIRE_SAMPLE_BYTES 1
#else
#define WIRE_FORMAT "pcm16_24k"
#define WIRE_RATE_HZ AUDIO_SAMPLE_RATE_HZ
#define WIRE_SAMPLE_BYTES 2
#endif
#define WIRE_DECIMATION (AUDIO_SAMPLE_RATE_HZ / WIRE_RATE_HZ)
#define WIRE_FRAME_BYTES (AUDIO_FRAME_SAMPLES / WIRE_DECIMATION * WIRE_SAMPLE_BYTES)
#define WIRE_BYTES_PER_MS (WIRE_RATE_HZ * WIRE_SAMPLE_BYTES / 1000)
// Mic audio waits here for the sender, which batches it into 100 ms frames.
#define MIC_QUEUE_BYTES (WIRE_BYTES_PER_MS * 1000)
// The server closes with 4400 when caller audio arrives faster than 1.25x real time over 5 s, i.e.
// more than 1.25 s ahead. After a network stall, the audio stuck in the TCP send buffer arrives in
// one burst, so the sender keeps at most this much backlog and drops the oldest audio beyond it.
#define MIC_BACKLOG_MAX_BYTES (WIRE_BYTES_PER_MS * 300)
#define SEND_CHUNK_BYTES (WIRE_FRAME_BYTES * 5)
// A send that times out makes the client drop the connection, so this only covers a dead link.
#define SEND_TIMEOUT_MS 3000

static const char *TAG = "cue";

static esp_websocket_client_handle_t s_client;
static atomic_int s_state = CUE_VOICE_IDLE;
static atomic_bool s_stop;
static atomic_bool s_key_rejected;
static atomic_bool s_ws_up;
static atomic_bool s_uplink_running;
static atomic_bool s_sender_running;
static StreamBufferHandle_t s_mic_queue;
static StaticStreamBuffer_t s_mic_queue_struct;
static uint8_t *s_mic_queue_storage;
static atomic_int s_mic_dropped;
static _Atomic int64_t s_send_us;
static _Atomic int64_t s_send_max_us;
static atomic_int s_close_code;
static _Atomic int64_t s_idle_us;
static bool s_start_sent;

static void note_activity(void)
{
    s_idle_us = esp_timer_get_time();
}

static char s_uri[256];
static char s_headers[512];
static uint8_t *s_rx;
static size_t s_frame_got;
static size_t s_frame_len;
static uint8_t s_opcode;
static bool s_assembling;

static const char *close_meaning(int code)
{
    switch (code) {
    case 1000: return "normal";
    case 4400: return "bad frame or uplink too fast (firmware bug)";
    case 4401: return "key rejected";
    case 4408: return "timeout: no start, no audio, or no pong (firmware bug)";
    case 4409: return "group busy";
    case 4410: return "playback behind (firmware bug)";
    case 4503: return "voice unavailable";
    default: return "unspecified";
    }
}

static bool session_url(char *out, size_t out_len)
{
    const char *server = CUE_VOICE_SERVER;
    const char *scheme = "wss";
    if (strncmp(server, "https://", 8) == 0) {
        server += 8;
    } else if (strncmp(server, "wss://", 6) == 0) {
        server += 6;
    } else if (strncmp(server, "http://", 7) == 0) {
        server += 7;
        scheme = "ws";
    } else if (strncmp(server, "ws://", 5) == 0) {
        server += 5;
        scheme = "ws";
    } else {
        return false;
    }
    int n = snprintf(out, out_len, "%s://%s/v1/agent-groups/%s/voice/sessions", scheme, server, CUE_VOICE_GROUP);
    return n > 0 && (size_t)n < out_len;
}

static bool name_is_safe(const char *name)
{
    if (!name || name[0] == '\0' || strlen(name) >= 80) {
        return false;
    }
    for (const char *p = name; *p; ++p) {
        if (*p == '"' || *p == '\\' || (unsigned char)*p < 0x20) {
            return false;
        }
    }
    return true;
}

static int send_text(const char *json)
{
    if (!s_client || !esp_websocket_client_is_connected(s_client)) {
        return -1;
    }
    return esp_websocket_client_send_text(s_client, json, (int)strlen(json), pdMS_TO_TICKS(2000));
}

static void note_close(int code)
{
    s_close_code = code;
    if (code == 4401) {
        s_key_rejected = true;
    }
    if (code == 4400 || code == 4408 || code == 4410) {
        ESP_LOGE(TAG, "close %d: %s", code, close_meaning(code));
    } else {
        ESP_LOGW(TAG, "close %d: %s", code, close_meaning(code));
    }
}

static void handle_text(char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGW(TAG, "ignored malformed text frame");
        return;
    }
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type) || !type->valuestring) {
        cJSON_Delete(root);
        return;
    }
    const char *kind = type->valuestring;
    if (strcmp(kind, "session.started") == 0) {
        const cJSON *call_id = cJSON_GetObjectItemCaseSensitive(root, "call_id");
        const cJSON *format = cJSON_GetObjectItemCaseSensitive(root, "audio_format");
        const cJSON *max_s = cJSON_GetObjectItemCaseSensitive(root, "max_duration_s");
        int64_t now = esp_timer_get_time();
        s_idle_us = now;
        s_state = CUE_VOICE_UP;
        ESP_LOGI(TAG, "session.started call_id=%s format=%s max_duration_s=%d",
                 cJSON_IsString(call_id) ? call_id->valuestring : "",
                 cJSON_IsString(format) ? format->valuestring : "",
                 cJSON_IsNumber(max_s) ? max_s->valueint : -1);
    } else if (strcmp(kind, "output.clear") == 0) {
        playback_clear();
        ESP_LOGI(TAG, "output.clear");
    } else if (strcmp(kind, "output.mark") == 0) {
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "name");
        if (cJSON_IsString(name) && name->valuestring) {
            playback_mark(name->valuestring);
        }
    } else if (strcmp(kind, "transcript") == 0) {
        const cJSON *role = cJSON_GetObjectItemCaseSensitive(root, "role");
        const cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
        const cJSON *final = cJSON_GetObjectItemCaseSensitive(root, "final");
        ESP_LOGI(TAG, "transcript role=%s final=%s text=%s",
                 cJSON_IsString(role) ? role->valuestring : "",
                 cJSON_IsTrue(final) ? "true" : "false",
                 cJSON_IsString(text) ? text->valuestring : "");
        if (cJSON_IsString(role) && strcmp(role->valuestring, "caller") == 0 && cJSON_IsTrue(final)) {
            note_activity();
        }
    } else if (strcmp(kind, "session.ended") == 0) {
        const cJSON *reason = cJSON_GetObjectItemCaseSensitive(root, "reason");
        const cJSON *duration = cJSON_GetObjectItemCaseSensitive(root, "duration_s");
        ESP_LOGI(TAG, "session.ended reason=%s duration_s=%.3f",
                 cJSON_IsString(reason) ? reason->valuestring : "",
                 cJSON_IsNumber(duration) ? duration->valuedouble : 0.0);
        s_state = CUE_VOICE_ENDED;
    } else if (strcmp(kind, "error") == 0) {
        const cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
        const cJSON *message = cJSON_GetObjectItemCaseSensitive(root, "message");
        int number = 0;
        if (cJSON_IsNumber(code)) {
            number = code->valueint;
        } else if (cJSON_IsString(code) && code->valuestring) {
            number = atoi(code->valuestring);
        }
        note_close(number);
        ESP_LOGW(TAG, "error %d: %s", number, cJSON_IsString(message) ? message->valuestring : "");
    }
    cJSON_Delete(root);
}

#if CUE_VOICE_PCMU
// G.711 mu-law, as in ITU-T G.711 / the classic Sun reference implementation.
static uint8_t linear_to_ulaw(int16_t pcm)
{
    const int bias = 0x84;
    const int clip = 32635;
    int sample = pcm;
    uint8_t sign = 0;
    if (sample < 0) {
        sample = -sample;
        sign = 0x80;
    }
    if (sample > clip) {
        sample = clip;
    }
    sample += bias;
    int exponent = 7;
    for (int mask = 0x4000; (sample & mask) == 0 && exponent > 0; mask >>= 1) {
        exponent--;
    }
    int mantissa = (sample >> (exponent + 3)) & 0x0F;
    return (uint8_t)~(sign | (exponent << 4) | mantissa);
}

static int16_t ulaw_to_linear(uint8_t ulaw)
{
    ulaw = (uint8_t)~ulaw;
    int sign = ulaw & 0x80;
    int exponent = (ulaw >> 4) & 0x07;
    int mantissa = ulaw & 0x0F;
    int sample = (((mantissa << 3) + 0x84) << exponent) - 0x84;
    return (int16_t)(sign ? -sample : sample);
}

#endif

// Agent audio. With pcmu: mu-law at 8 kHz, played at 24 kHz by linear interpolation.
static int16_t s_down_last;

static void push_agent_audio(const uint8_t *audio, size_t len)
{
#if !CUE_VOICE_PCMU
    playback_push(audio, len);
#else
    static int16_t pcm[256 * WIRE_DECIMATION];
    while (len > 0) {
        size_t n = len > 256 ? 256 : len;
        for (size_t i = 0; i < n; ++i) {
            int16_t next = ulaw_to_linear(audio[i]);
            for (int k = 1; k <= WIRE_DECIMATION; ++k) {
                pcm[i * WIRE_DECIMATION + k - 1] =
                    (int16_t)(s_down_last + (next - s_down_last) * k / WIRE_DECIMATION);
            }
            s_down_last = next;
        }
        playback_push((const uint8_t *)pcm, n * WIRE_DECIMATION * sizeof(int16_t));
        audio += n;
        len -= n;
    }
#endif
}

// One 20 ms mic frame in the wire format. With pcmu: 160 mu-law bytes at 8 kHz. Averaging three samples (plus the
// mic's 4.8 kHz low-pass) keeps most aliasing out of the 4 kHz band.
static void encode_mic_frame(const int16_t *pcm, uint8_t *out)
{
#if !CUE_VOICE_PCMU
    memcpy(out, pcm, WIRE_FRAME_BYTES);
#else
    for (int j = 0; j < WIRE_FRAME_BYTES; ++j) {
        const int16_t *x = pcm + j * WIRE_DECIMATION;
        out[j] = linear_to_ulaw((int16_t)(((int)x[0] + x[1] + x[2]) / 3));
    }
#endif
}

static void dispatch_frame(uint8_t opcode, uint8_t *data, size_t len)
{
    if (opcode == WS_OP_CLOSE) {
        int code = 0;
        if (len >= 2) {
            code = (data[0] << 8) | data[1];
        }
        note_close(code);
        return;
    }
    if (opcode == WS_OP_TEXT) {
        if (len >= RX_MAX) {
            return;
        }
        data[len] = 0;
        handle_text((char *)data);
        return;
    }
    if (opcode == WS_OP_BIN && len > 0) {
        note_activity();
        push_agent_audio(data, len);
    }
}

static void consume_chunk(const esp_websocket_event_data_t *event)
{
    if (!event || !event->data_ptr || event->data_len <= 0) {
        return;
    }
    if (event->op_code == WS_OP_PING || event->op_code == WS_OP_PONG) {
        return;
    }
    const size_t chunk = (size_t)event->data_len;
    const size_t offset = event->payload_offset > 0 ? (size_t)event->payload_offset : 0;
    const size_t total = event->payload_len > 0 ? (size_t)event->payload_len : chunk;
    if (offset == 0) {
        s_assembling = true;
        s_opcode = event->op_code;
        s_frame_len = total;
        s_frame_got = 0;
        if (total > RX_MAX) {
            s_assembling = false;
            ESP_LOGW(TAG, "drop oversized frame: %u bytes", (unsigned)total);
            return;
        }
    }
    if (!s_assembling || offset != s_frame_got || chunk > s_frame_len - s_frame_got) {
        s_assembling = false;
        return;
    }
    memcpy(s_rx + offset, event->data_ptr, chunk);
    s_frame_got += chunk;
    if (s_frame_got < s_frame_len || !event->fin) {
        return;
    }
    dispatch_frame(s_opcode, s_rx, s_frame_len);
    s_assembling = false;
}

static void on_ws(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == WEBSOCKET_EVENT_CONNECTED) {
        s_ws_up = true;
        ESP_LOGI(TAG, "websocket connected");
    } else if (id == WEBSOCKET_EVENT_DATA) {
        consume_chunk(data);
    } else if (id == WEBSOCKET_EVENT_DISCONNECTED || id == WEBSOCKET_EVENT_CLOSED) {
        const esp_websocket_event_data_t *event = data;
        int http = event ? event->error_handle.esp_ws_handshake_status_code : 0;
        if (http != 0) {
            ESP_LOGW(TAG, "handshake HTTP %d", http);
        }
        s_ws_up = false;
        if (s_state == CUE_VOICE_UP || s_state == CUE_VOICE_CONNECTING) {
            s_state = s_close_code == 4401 || s_close_code == 4409 || s_close_code == 4503
                          ? CUE_VOICE_FAILED
                          : CUE_VOICE_ENDED;
        }
        ESP_LOGI(TAG, "websocket closed");
        s_stop = true;
    } else if (id == WEBSOCKET_EVENT_ERROR) {
        const esp_websocket_event_data_t *event = data;
        int http = event ? event->error_handle.esp_ws_handshake_status_code : 0;
        ESP_LOGE(TAG, "websocket error, handshake HTTP %d", http);
        s_state = CUE_VOICE_FAILED;
        s_stop = true;
    }
}

static void send_played_marks(void)
{
    char played[8][80];
    int n = playback_take_played(played, 8);
    for (int i = 0; i < n; ++i) {
        if (!name_is_safe(played[i])) {
            ESP_LOGW(TAG, "skipped unsafe mark name");
            continue;
        }
        char json[128];
        snprintf(json, sizeof(json), "{\"type\":\"output.played\",\"name\":\"%s\"}", played[i]);
        send_text(json);
    }
}

// The only task that writes to the socket after session.start: played marks first, then mic audio
// in 100 ms frames. Mic capture never waits on the network, and a slow audio send cannot hold
// output.played back long enough for the server to close with 4410.
static void sender_task(void *arg)
{
    (void)arg;
    static uint8_t chunk[SEND_CHUNK_BYTES];
    while (!s_stop) {
        send_played_marks();
        size_t backlog = xStreamBufferBytesAvailable(s_mic_queue);
        if (backlog > MIC_BACKLOG_MAX_BYTES) {
            size_t excess = backlog - MIC_BACKLOG_MAX_BYTES;
            while (excess > 0) {
                size_t n = xStreamBufferReceive(s_mic_queue, chunk, excess < sizeof(chunk) ? excess : sizeof(chunk), 0);
                if (n == 0) {
                    break;
                }
                excess -= n;
                s_mic_dropped += (int)(n / WIRE_FRAME_BYTES);
            }
        }
        if (xStreamBufferBytesAvailable(s_mic_queue) < SEND_CHUNK_BYTES) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        size_t n = xStreamBufferReceive(s_mic_queue, chunk, sizeof(chunk), 0);
        if (n == 0 || !s_client || !esp_websocket_client_is_connected(s_client)) {
            continue;
        }
        int64_t start = esp_timer_get_time();
        int sent = esp_websocket_client_send_bin(s_client, (const char *)chunk, (int)n, pdMS_TO_TICKS(SEND_TIMEOUT_MS));
        int64_t took = esp_timer_get_time() - start;
        s_send_us = took;
        if (took > s_send_max_us) {
            s_send_max_us = took;
        }
        if (sent < 0) {
            ESP_LOGW(TAG, "audio send failed");
        }
    }
    s_sender_running = false;
    vTaskDelete(NULL);
}

// The microphone's I2S clock paces the uplink: one 20 ms frame goes out per 480 samples read.
// The mic is open for the whole call, except that frames are zeros while agent audio plays.
static void uplink_task(void *arg)
{
    (void)arg;
    static int16_t frame[AUDIO_FRAME_SAMPLES];
    int filled = 0;
    bool recording = false;
    bool was_gated = false;
    int64_t speaker_quiet_since = 0;
    int64_t stats_at = 0;
    int mic_peak = 0;
    while (!s_stop) {
        if (!s_start_sent && s_ws_up) {
            const char *start =
                "{\"type\":\"session.start\",\"audio_format\":\"" WIRE_FORMAT "\",\"client\":\"t-circle/0.1\"}";
            if (send_text(start) < 0) {
                ESP_LOGE(TAG, "session.start send failed");
                s_stop = true;
                break;
            }
            s_start_sent = true;
            ESP_LOGI(TAG, "sent session.start");
        }
        if (s_state != CUE_VOICE_UP) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (!recording) {
            esp_err_t err = board_audio_set_recording(true);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "microphone start failed: %s", esp_err_to_name(err));
                s_stop = true;
                break;
            }
            recording = true;
            filled = 0;
        }
        int got = board_audio_read_mono(frame + filled, AUDIO_FRAME_SAMPLES - filled);
        if (got < 0) {
            ESP_LOGE(TAG, "microphone read failed");
            s_stop = true;
            break;
        }
        filled += got;
        if (filled < AUDIO_FRAME_SAMPLES) {
            continue;
        }
        filled = 0;

        int64_t now = esp_timer_get_time();
        int64_t idle = s_idle_us;
        int queued_bytes = 0;
        int pending_marks = 0;
        playback_stats(&queued_bytes, &pending_marks);
        if (playback_speaking()) {
            speaker_quiet_since = 0;
        } else if (speaker_quiet_since == 0) {
            speaker_quiet_since = now;
        }
        const bool gated = speaker_quiet_since == 0 || now - speaker_quiet_since < ECHO_TAIL_US;
        if (gated != was_gated) {
            ESP_LOGI(TAG, "mic %s", gated ? "muted while the agent speaks" : "open");
            was_gated = gated;
        }
        if (!gated) {
            for (int i = 0; i < AUDIO_FRAME_SAMPLES; i++) {
                int magnitude = frame[i] < 0 ? -(int)frame[i] : frame[i];
                if (magnitude > mic_peak) {
                    mic_peak = magnitude;
                }
            }
        } else {
            memset(frame, 0, sizeof(frame));
        }
        if (idle != 0 && now - idle >= IDLE_HANGUP_US) {
            ESP_LOGI(TAG, "idle hangup: 60 s without a final caller transcript or agent audio");
            s_stop = true;
            break;
        }
        uint8_t wire[WIRE_FRAME_BYTES];
        encode_mic_frame(frame, wire);
        if (xStreamBufferSend(s_mic_queue, wire, sizeof(wire), 0) != sizeof(wire)) {
            s_mic_dropped++;
        }
        if (now - stats_at >= 1000000) {
            int rx_bytes = 0;
            int rx_peak = 0;
            playback_take_rx(&rx_bytes, &rx_peak);
            ESP_LOGI(TAG,
                     "stats rx_ms=%d rx_peak=%d queue_ms=%d marks=%d send_ms=%lld send_max_ms=%lld mic_backlog_ms=%d mic_dropped=%d mic=%s "
                     "mic_peak=%d",
                     rx_bytes / (AUDIO_SAMPLE_RATE_HZ * (int)sizeof(int16_t) / 1000), rx_peak,
                     queued_bytes / (AUDIO_SAMPLE_RATE_HZ * (int)sizeof(int16_t) / 1000), pending_marks,
                     (long long)(s_send_us / 1000), (long long)(s_send_max_us / 1000),
                     (int)xStreamBufferBytesAvailable(s_mic_queue) / WIRE_BYTES_PER_MS,
                     (int)s_mic_dropped, gated ? "muted" : "open", mic_peak);
            stats_at = now;
            s_send_max_us = 0;
            s_mic_dropped = 0;
            mic_peak = 0;
        }
    }
    if (recording) {
        board_audio_set_recording(false);
    }
    playback_ringback(false);
    // The sender writes to s_client, so it must be gone before the client is destroyed.
    s_stop = true;
    while (s_sender_running) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_client && esp_websocket_client_is_connected(s_client)) {
        // session.end before session.start is a 4400, so just close.
        if (s_start_sent) {
            send_text("{\"type\":\"session.end\"}");
            ESP_LOGI(TAG, "sent session.end");
            int64_t until = esp_timer_get_time() + 1000000;
            while (s_state == CUE_VOICE_UP && esp_timer_get_time() < until) {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
        } else {
            ESP_LOGI(TAG, "closing before session.start");
        }
        esp_websocket_client_close_with_code(s_client, 1000, NULL, 0, pdMS_TO_TICKS(1000));
    }
    esp_websocket_client_handle_t client = s_client;
    s_client = NULL;
    if (client) {
        esp_websocket_client_stop(client);
        esp_websocket_client_destroy(client);
    }
    if (s_state == CUE_VOICE_UP || s_state == CUE_VOICE_CONNECTING) {
        s_state = CUE_VOICE_ENDED;
    }
    s_ws_up = false;
    s_uplink_running = false;
    vTaskDelete(NULL);
}

esp_err_t cue_voice_start(void)
{
    if (s_key_rejected) {
        ESP_LOGE(TAG, "not retrying after close 4401");
        return ESP_ERR_INVALID_STATE;
    }
    cue_voice_state_t state = s_state;
    if (s_uplink_running || state == CUE_VOICE_CONNECTING || state == CUE_VOICE_UP) {
        return ESP_ERR_INVALID_STATE;
    }
    if (strncmp(CUE_VOICE_KEY, "salix_vk_", 9) != 0) {
        ESP_LOGE(TAG, "CUE_VOICE_KEY is not a voice agent key");
        return ESP_ERR_INVALID_STATE;
    }
    if (!session_url(s_uri, sizeof(s_uri))) {
        ESP_LOGE(TAG, "bad CUE_VOICE_SERVER or group");
        return ESP_ERR_INVALID_ARG;
    }
    int header_n = snprintf(s_headers, sizeof(s_headers), "Authorization: Bearer %s\r\n", CUE_VOICE_KEY);
    if (header_n < 0 || (size_t)header_n >= sizeof(s_headers)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!s_rx) {
        s_rx = heap_caps_malloc(RX_MAX + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_rx) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s_mic_queue) {
        s_mic_queue_storage = heap_caps_malloc(MIC_QUEUE_BYTES + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_mic_queue_storage) {
            return ESP_ERR_NO_MEM;
        }
        s_mic_queue = xStreamBufferCreateStatic(MIC_QUEUE_BYTES, 1, s_mic_queue_storage, &s_mic_queue_struct);
    }
    xStreamBufferReset(s_mic_queue);
    s_down_last = 0;
    s_mic_dropped = 0;
    s_send_us = 0;
    s_send_max_us = 0;
    playback_clear();
    s_stop = false;
    s_ws_up = false;
    s_start_sent = false;
    s_close_code = 0;
    s_idle_us = 0;
    s_assembling = false;
    s_state = CUE_VOICE_CONNECTING;

    esp_websocket_client_config_t cfg = {
        .uri = s_uri,
        .subprotocol = "comma.voice.v1",
        .headers = s_headers,
        .transport = WEBSOCKET_TRANSPORT_OVER_SSL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_reconnect = true,
        .buffer_size = 4096,
        .task_stack = 6144,
        .task_prio = 5,
        .network_timeout_ms = 10000,
        .ping_interval_sec = 10,
        .pingpong_timeout_sec = 20,
    };
    if (strncmp(s_uri, "ws://", 5) == 0) {
        cfg.transport = WEBSOCKET_TRANSPORT_OVER_TCP;
        cfg.crt_bundle_attach = NULL;
    }
    s_client = esp_websocket_client_init(&cfg);
    if (!s_client) {
        s_state = CUE_VOICE_FAILED;
        return ESP_FAIL;
    }
    ESP_ERROR_CHECK(esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, on_ws, NULL));
    esp_err_t err = esp_websocket_client_start(s_client);
    if (err != ESP_OK) {
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
        s_state = CUE_VOICE_FAILED;
        return err;
    }
    s_sender_running = true;
    if (xTaskCreate(sender_task, "sender", 4096, NULL, 6, NULL) != pdPASS) {
        s_sender_running = false;
        s_stop = true;
        esp_websocket_client_stop(s_client);
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
        s_state = CUE_VOICE_FAILED;
        return ESP_ERR_NO_MEM;
    }
    s_uplink_running = true;
    if (xTaskCreate(uplink_task, "uplink", 6144, NULL, 5, NULL) != pdPASS) {
        s_uplink_running = false;
        s_stop = true;
        while (s_sender_running) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        esp_websocket_client_stop(s_client);
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
        s_state = CUE_VOICE_FAILED;
        return ESP_ERR_NO_MEM;
    }
    playback_ringback(true);
    ESP_LOGI(TAG, "dialing %s", s_uri);
    return ESP_OK;
}

void cue_voice_end(void)
{
    if (s_state == CUE_VOICE_CONNECTING || s_state == CUE_VOICE_UP) {
        ESP_LOGI(TAG, "hangup requested");
        s_stop = true;
    }
}

bool cue_voice_can_start(void)
{
    cue_voice_state_t state = s_state;
    return !s_key_rejected && !s_uplink_running && !s_sender_running && state != CUE_VOICE_CONNECTING && state != CUE_VOICE_UP;
}

cue_voice_state_t cue_voice_state(void)
{
    return s_state;
}

int cue_voice_close_code(void)
{
    return s_close_code;
}
