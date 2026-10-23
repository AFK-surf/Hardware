// SPDX-License-Identifier: Apache-2.0
#include "comma_voice.h"

#include "board_audio.h"
#include "device_config.h"
#include "firmware_config.h"
#include "memory_caps.h"
#include "playback.h"
#include "voice_clean.h"
#include "ws_frame.h"

#include <math.h>
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
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/priv/tcpip_priv.h"
#include "lwip/tcp.h"

#define RX_MAX (16 * 1024)
#define FRAME_BYTES (AUDIO_FRAME_SAMPLES * (int)sizeof(int16_t))
#define IDLE_HANGUP_US (60LL * 1000000)
// The ringback plays, with the mic muted, until the agent first speaks. If it hasn't this long
// after session.started (it once sent only silence for minutes, 2026-10-06), stop ringing, play the
// connected tone and open the mic, so the caller can speak first.
#define GREETING_WAIT_US (8LL * 1000000)
// No echo canceller (the server's voice model does none either): the mic sends silence while agent
// speech plays and for this long after.
#define ECHO_TAIL_US (300LL * 1000)
// Wire format: pcm16_24k both ways, the speaker's rate, so frames go out and in as they are.
#define WIRE_FORMAT "pcm16_24k"
#define WIRE_BYTES_PER_MS (AUDIO_SAMPLE_RATE_HZ * (int)sizeof(int16_t) / 1000)
// voice_clean.h: the wire carries mic audio (denoised if nsnet2 is on) only while the VAD hears
// speech, from PREROLL_MS before it fired (it needs 128 ms of speech to decide, and word starts are
// quiet) until HANGOVER_MS after it stopped (soft word ends). Otherwise zeros, still paced by the mic clock, so the
// server keeps its continuous real-time stream. The pre-roll goes out at once when speech starts, so
// the stream runs that much ahead; the next zero frames are skipped to pay it back (preroll debt).
// The AFE runs vadnet (and nsnet2, if on) on this task's stack. A 16 KB stack peaked at 2.2 KB used
// (2026-10-07). Internal RAM: a PSRAM stack was no faster and can't run during flash writes.
#define UPLINK_STACK 8192
#define PREROLL_MS 300
#define HANGOVER_MS 500
#define PREROLL_FRAMES (PREROLL_MS / AUDIO_FRAME_MS)
#define HANGOVER_FRAMES (HANGOVER_MS / AUDIO_FRAME_MS)
// Mic audio waits here for the sender, which batches it into 100 ms frames.
#define MIC_QUEUE_BYTES (WIRE_BYTES_PER_MS * 1000)
// The server closes with 4400 when caller audio arrives faster than 1.25x real time over 5 s, i.e.
// more than 1.25 s ahead. After a network stall, the audio stuck in the TCP send buffer arrives in
// one burst, so the sender keeps at most this much backlog and drops the oldest audio beyond it.
// A pre-roll lands in the queue all at once, so the cap leaves room for it on top; at most ~1 s is
// then ahead after a stall, still inside the server's 1.25 s.
#define MIC_BACKLOG_MAX_BYTES (WIRE_BYTES_PER_MS * (300 + PREROLL_MS))
#define SEND_CHUNK_BYTES (FRAME_BYTES * 5)
// Any esp_websocket_client send that times out tears the connection down, and a send that blocks
// holds the TX lock, which the websocket task needs for PONG: if it can't get it within 2 s
// (CONFIG_ESP_WS_CLIENT_TX_LOCK_TIMEOUT_MS) the read fails and the client aborts too. A 3 s send
// timeout closed 2 of 5 calls on office Wi-Fi (2026-10-06) on uplink stalls the server would have
// ridden out. So the sender only writes when the voice socket's lwIP send buffer can take the whole
// frame without blocking (tcp_room), and waits outside the lock otherwise, trimming the mic backlog.
// UPLINK_STALL_LIMIT_MS is how long no audio frame may fit before the device hangs up itself. The
// server gives up on a client that doesn't read for 8 s (slow_reader); an uplink that hasn't
// drained 100 ms of audio in 10 s almost always means the downlink is dead too, so by then the
// server has closed (and its close code says why). Shorter, and the device hangs up on stalls the
// server tolerates; longer only keeps a dead call on screen. After a stall at most ~0.7 s of audio is
// ahead (the frames in flight plus the 300 ms mic backlog), well inside the server's 5 s catch-up.
#define UPLINK_STALL_LIMIT_MS 10000
// Ceiling for a send that the gate let through (or every send, if the voice socket wasn't found).
#define SEND_TIMEOUT_MS UPLINK_STALL_LIMIT_MS
// A wait for room longer than this is logged as a stall, with the Wi-Fi and TCP state.
#define STALL_LOG_MS 1000
// What one audio frame costs in the send buffer: 2 websocket fragments (the client's 4096-byte
// buffer), each a header and a payload write, each its own TLS record (~30 bytes), plus slack.
#define AUDIO_SEND_OVERHEAD 256
// pbufs one audio frame can take in the TCP send queue (4800 B / MSS plus the small header writes).
#define AUDIO_SEND_SEGMENTS 16

static const char *TAG = "comma";

static esp_websocket_client_handle_t s_client;
static atomic_int s_state = COMMA_VOICE_IDLE;
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
static atomic_int s_rx_ms_last;
static _Atomic int64_t s_wait_since_us;
// How the call ends, for the hang-up tone: the caller tapped, the device hung up on silence, or the
// server ended the session.
static atomic_bool s_user_hangup;
static atomic_bool s_idle_hangup;
static atomic_bool s_session_ended;
static atomic_int s_last_end = COMMA_VOICE_END_NONE;
static _Atomic int64_t s_started_us;

// The voice socket in lwIP, found by local port when the websocket connects (0: not found, and the
// sender falls back to plain blocking sends).
static _Atomic uint16_t s_voice_port;
#define KNOWN_PORTS_MAX 16
static uint16_t s_known_ports[KNOWN_PORTS_MAX];
static int s_known_count;
// Worst TCP state the sender saw since the last stats line.
static atomic_uint s_tcp_unacked_max;
static atomic_uint s_tcp_nrtx_max;

static void tcp_find_voice_socket(void);

static void note_activity(void)
{
    s_idle_us = esp_timer_get_time();
}

static char s_uri[256];
static char s_headers[512];
// Incoming frames, reassembled in the websocket task. PSRAM, RX_MAX + 1 bytes.
static ws_frame_t s_frame = {.capacity = RX_MAX};

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

static bool session_url(const char *server, const char *group, char *out, size_t out_len)
{
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
    int n = snprintf(out, out_len, "%s://%s/v1/agent-groups/%s/voice/sessions", scheme, server, group);
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

static int send_text(const char *json, int timeout_ms)
{
    if (!s_client || !esp_websocket_client_is_connected(s_client)) {
        return -1;
    }
    return esp_websocket_client_send_text(s_client, json, (int)strlen(json), pdMS_TO_TICKS(timeout_ms));
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
        s_started_us = now;
        s_state = COMMA_VOICE_UP;
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
        // What was said stays out of the log: only its length.
        ESP_LOGI(TAG, "transcript role=%s final=%s chars=%u", cJSON_IsString(role) ? role->valuestring : "",
                 cJSON_IsTrue(final) ? "true" : "false",
                 cJSON_IsString(text) ? (unsigned)strlen(text->valuestring) : 0U);
        if (cJSON_IsString(role) && strcmp(role->valuestring, "caller") == 0 && cJSON_IsTrue(final)) {
            note_activity();
        }
    } else if (strcmp(kind, "session.ended") == 0) {
        const cJSON *reason = cJSON_GetObjectItemCaseSensitive(root, "reason");
        const cJSON *duration = cJSON_GetObjectItemCaseSensitive(root, "duration_s");
        ESP_LOGI(TAG, "session.ended reason=%s duration_s=%.3f",
                 cJSON_IsString(reason) ? reason->valuestring : "",
                 cJSON_IsNumber(duration) ? duration->valuedouble : 0.0);
        s_session_ended = true;
        s_state = COMMA_VOICE_ENDED;
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
        if (s_stop) {
            // The call is over and the hang-up tone owns the speaker; agent audio still in flight
            // until the server's session.ended is dropped.
            return;
        }
        note_activity();
        playback_push(data, len);
    }
}

static void consume_chunk(const esp_websocket_event_data_t *event)
{
    const ws_frame_result_t result = ws_frame_feed(&s_frame, event);
    if (result == WS_FRAME_DONE) {
        dispatch_frame(s_frame.opcode, s_frame.buf, s_frame.len);
    } else if (result == WS_FRAME_TOO_BIG) {
        ESP_LOGW(TAG, "drop oversized frame: %u bytes", (unsigned)s_frame.len);
    }
}

static void on_ws(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == WEBSOCKET_EVENT_CONNECTED) {
        tcp_find_voice_socket();
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
        if (s_state == COMMA_VOICE_UP || s_state == COMMA_VOICE_CONNECTING) {
            s_state = s_close_code == 4401 || s_close_code == 4409 || s_close_code == 4503
                          ? COMMA_VOICE_FAILED
                          : COMMA_VOICE_ENDED;
        }
        ESP_LOGI(TAG, "websocket closed");
        s_stop = true;
    } else if (id == WEBSOCKET_EVENT_ERROR) {
        const esp_websocket_event_data_t *event = data;
        int http = event ? event->error_handle.esp_ws_handshake_status_code : 0;
        ESP_LOGE(TAG, "websocket error, handshake HTTP %d", http);
        s_state = COMMA_VOICE_FAILED;
        s_stop = true;
    }
}

typedef struct {
    struct tcpip_api_call_data call;
    // in
    uint16_t port;
    bool collect;
    // out
    bool found;
    int new_count;
    uint16_t new_port;
    uint32_t snd_buf;
    uint32_t queue_len;
    uint32_t unacked;
    uint32_t snd_wnd;
    uint32_t cwnd;
    uint32_t nrtx;
    int rto_ms;
    int srtt_ms;
} tcp_probe_t;

static bool port_known(uint16_t port)
{
    for (int i = 0; i < s_known_count; ++i) {
        if (s_known_ports[i] == port) {
            return true;
        }
    }
    return false;
}

// Runs on the tcpip thread (no core locking in this build), the only place pcbs may be read.
static err_t tcp_probe_on_tcpip(struct tcpip_api_call_data *call)
{
    tcp_probe_t *p = (tcp_probe_t *)call;
    if (p->collect) {
        s_known_count = 0;
        for (struct tcp_pcb *pcb = tcp_active_pcbs; pcb && s_known_count < KNOWN_PORTS_MAX; pcb = pcb->next) {
            s_known_ports[s_known_count++] = pcb->local_port;
        }
        return ERR_OK;
    }
    for (struct tcp_pcb *pcb = tcp_active_pcbs; pcb; pcb = pcb->next) {
        if (p->port == 0) {
            if (pcb->state == ESTABLISHED && !port_known(pcb->local_port)) {
                p->new_count++;
                p->new_port = pcb->local_port;
            }
            continue;
        }
        if (pcb->local_port != p->port) {
            continue;
        }
        p->found = true;
        p->snd_buf = tcp_sndbuf(pcb);
        p->queue_len = tcp_sndqueuelen(pcb);
        p->unacked = pcb->snd_nxt - pcb->lastack;
        p->snd_wnd = pcb->snd_wnd;
        p->cwnd = pcb->cwnd;
        p->nrtx = pcb->nrtx;
        p->rto_ms = pcb->rto * TCP_SLOW_INTERVAL;
        p->srtt_ms = (pcb->sa >> 3) * TCP_SLOW_INTERVAL;
        break;
    }
    return ERR_OK;
}

static bool tcp_probe(tcp_probe_t *p)
{
    uint16_t port = s_voice_port;
    memset(p, 0, sizeof(*p));
    if (port == 0) {
        return false;
    }
    p->port = port;
    if (tcpip_api_call(tcp_probe_on_tcpip, &p->call) != ERR_OK || !p->found) {
        return false;
    }
    if (p->unacked > s_tcp_unacked_max) {
        s_tcp_unacked_max = p->unacked;
    }
    if (p->nrtx > s_tcp_nrtx_max) {
        s_tcp_nrtx_max = p->nrtx;
    }
    return true;
}

// Before dialing: remember which TCP connections already exist (the Connector's among them).
static void tcp_remember_existing(void)
{
    tcp_probe_t p = {.collect = true};
    s_voice_port = 0;
    tcpip_api_call(tcp_probe_on_tcpip, &p.call);
}

// On connect: the voice socket is the one established connection that wasn't there before. If the
// Connector or a LAN request connected in the same window it's ambiguous, and the gate stays off.
static void tcp_find_voice_socket(void)
{
    tcp_probe_t p = {0};
    tcpip_api_call(tcp_probe_on_tcpip, &p.call);
    if (p.new_count == 1) {
        s_voice_port = p.new_port;
        ESP_LOGI(TAG, "voice socket: local port %u", (unsigned)p.new_port);
    } else {
        ESP_LOGW(TAG, "voice socket not identified (%d new connections): sends block, no TCP stats", p.new_count);
    }
}

// The same test lwIP's select() uses for "writable". While it holds, a write of up to snd_buf bytes
// neither waits in poll_write nor blocks in the socket.
static bool tcp_writable(const tcp_probe_t *t)
{
    return t->snd_buf > TCP_SNDLOWAT && t->queue_len < TCP_SNDQUEUELOWAT;
}

static bool tcp_room(const tcp_probe_t *t, size_t bytes)
{
    return tcp_writable(t) && t->snd_buf >= bytes + AUDIO_SEND_OVERHEAD &&
           t->queue_len + AUDIO_SEND_SEGMENTS <= TCP_SND_QUEUELEN;
}

static void describe_wifi(char *out, size_t len)
{
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        snprintf(out, len, "rssi=? phy=?");
        return;
    }
    wifi_ps_type_t ps = WIFI_PS_NONE;
    esp_wifi_get_ps(&ps);
    // IDF has no getter for the current TX/RX PHY rate, only the negotiated mode and bandwidth.
    snprintf(out, len, "rssi=%d ch=%u phy=%s%s%s%s/%s ps=%d", ap.rssi, ap.primary, ap.phy_11b ? "b" : "",
             ap.phy_11g ? "g" : "", ap.phy_11n ? "n" : "", ap.phy_11ax ? "ax" : "",
             ap.second == WIFI_SECOND_CHAN_NONE ? "HT20" : "HT40", (int)ps);
}

static void describe_tcp(const tcp_probe_t *t, bool known, char *out, size_t len)
{
    if (!known) {
        snprintf(out, len, "tcp=?");
        return;
    }
    snprintf(out, len, "tcp_unacked=%u tcp_sndbuf=%u tcp_q=%u peer_wnd=%u cwnd=%u nrtx=%u rto_ms=%d srtt_ms=%d",
             (unsigned)t->unacked, (unsigned)t->snd_buf, (unsigned)t->queue_len, (unsigned)t->snd_wnd,
             (unsigned)t->cwnd, (unsigned)t->nrtx, t->rto_ms, t->srtt_ms);
}

static void log_stall(const char *what, int64_t waited_us, const tcp_probe_t *t, bool known)
{
    char wifi[64];
    char tcp[160];
    describe_wifi(wifi, sizeof(wifi));
    describe_tcp(t, known, tcp, sizeof(tcp));
    ESP_LOGW(TAG, "uplink %s waited_ms=%lld %s %s mic_backlog_ms=%d mic_dropped=%d rx_ms(last s)=%d", what,
             (long long)(waited_us / 1000), wifi, tcp,
             (int)xStreamBufferBytesAvailable(s_mic_queue) / WIRE_BYTES_PER_MS, (int)s_mic_dropped, (int)s_rx_ms_last);
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
        send_text(json, SEND_TIMEOUT_MS);
    }
}

// The only task that writes to the socket after session.start: played marks first, then mic audio
// in 100 ms frames. Mic capture never waits on the network, and a slow audio send cannot hold
// output.played back long enough for the server to close with 4410. Nothing is written until the
// voice socket has room for it (tcp_room), so a stall is waited out here, outside the TX lock, while
// the mic backlog stays capped; when room comes back, queued marks go out before any audio.
static void sender_task(void *arg)
{
    (void)arg;
    static uint8_t chunk[SEND_CHUNK_BYTES];
    int64_t wait_since = 0;
    while (!s_stop) {
        tcp_probe_t tcp;
        bool known = tcp_probe(&tcp);
        if (!known || tcp_writable(&tcp)) {
            send_played_marks();
            if (known) {
                tcp_probe(&tcp);
            }
        }
        size_t backlog = xStreamBufferBytesAvailable(s_mic_queue);
        if (backlog > MIC_BACKLOG_MAX_BYTES) {
            size_t excess = backlog - MIC_BACKLOG_MAX_BYTES;
            while (excess > 0) {
                size_t n = xStreamBufferReceive(s_mic_queue, chunk, excess < sizeof(chunk) ? excess : sizeof(chunk), 0);
                if (n == 0) {
                    break;
                }
                excess -= n;
                s_mic_dropped += (int)(n / FRAME_BYTES);
            }
        }
        if (xStreamBufferBytesAvailable(s_mic_queue) < SEND_CHUNK_BYTES) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        int64_t now = esp_timer_get_time();
        const bool room = !known || tcp_room(&tcp, SEND_CHUNK_BYTES);
        if (!room) {
            if (wait_since == 0) {
                wait_since = now;
                s_wait_since_us = now;
            }
            if (now - wait_since >= UPLINK_STALL_LIMIT_MS * 1000LL) {
                log_stall("dead, hanging up", now - wait_since, &tcp, known);
                s_stop = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        size_t n = xStreamBufferReceive(s_mic_queue, chunk, sizeof(chunk), 0);
        if (n == 0 || !s_client || !esp_websocket_client_is_connected(s_client)) {
            continue;
        }
        int64_t start = wait_since != 0 ? wait_since : now;
        // Set for the send too: it can still wait on the TX lock (the websocket task holds it while
        // its own PING/PONG waits for room), and the stats loop must see that as a stall.
        s_wait_since_us = start;
        int sent = esp_websocket_client_send_bin(s_client, (const char *)chunk, (int)n, pdMS_TO_TICKS(SEND_TIMEOUT_MS));
        int64_t took = esp_timer_get_time() - start;
        s_send_us = took;
        wait_since = 0;
        s_wait_since_us = 0;
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

// The call is over and the mic is closed: tell the caller how it ended. A clean end is the caller's
// tap, the device's own idle hangup, or the server ending the session with close 1000 (or none).
// Anything else (an error close, a send stall, a lost connection, a dial that never connected) is
// a drop.
static void play_end_tone(void)
{
    const int code = s_close_code;
    const bool server_clean = (s_session_ended || code == 1000) && (code == 0 || code == 1000);
    const bool clean = s_user_hangup || s_idle_hangup || server_clean;
    s_last_end = clean ? COMMA_VOICE_END_NORMAL : COMMA_VOICE_END_DROPPED;
    ESP_LOGI(TAG, "call over: %s (tap=%d idle=%d session_ended=%d close=%d)", clean ? "ended" : "dropped",
             (int)s_user_hangup, (int)s_idle_hangup, (int)s_session_ended, code);
    playback_tone(clean ? PLAYBACK_TONE_NORMAL : PLAYBACK_TONE_DROPPED);
}

// One 20 ms frame goes on the wire as it is (pcm16_24k).
static void enqueue_frame(const int16_t *frame)
{
    if (xStreamBufferSend(s_mic_queue, frame, FRAME_BYTES, 0) != FRAME_BYTES) {
        s_mic_dropped++;
    }
}

// Per-second mic figures for the stats line.
typedef struct {
    int peak;          // raw mic, open frames
    int64_t sum_sq;    // raw mic, open frames
    int open_samples;
    int64_t echo_sq;   // raw mic while muted for agent speech: the speaker's echo
    int echo_samples;
    int vad_frames;    // frames the VAD called speech
    int sent_frames;   // frames that carried audio, not zeros
    int64_t ns_sq;     // AFE output (denoised if nsnet2 is on), open frames
    int ns_samples;
} mic_stats_t;

static int rms_of(int64_t sum_sq, int samples)
{
    return samples > 0 ? (int)sqrtf((float)sum_sq / (float)samples) : 0;
}

static int64_t sum_squares(const int16_t *pcm, int samples)
{
    int64_t sum = 0;
    for (int i = 0; i < samples; ++i) {
        sum += (int32_t)pcm[i] * pcm[i];
    }
    return sum;
}

// What voice_clean has kept back: up to PREROLL_FRAMES denoised frames that went out as zeros, the frames of
// hangover left, and how many frames the stream is ahead of real time after a pre-roll.
static int16_t (*s_preroll)[AUDIO_FRAME_SAMPLES];
static int s_preroll_head;
static int s_preroll_count;
static bool s_talking;
static int s_hangover_left;
static int s_preroll_debt;

static void clean_gate_reset(void)
{
    s_preroll_head = 0;
    s_preroll_count = 0;
    s_talking = false;
    s_hangover_left = 0;
    s_preroll_debt = 0;
}

// One denoised 20 ms frame: decide what goes on the wire for it.
static void clean_gate_frame(int16_t *frame, bool speech, bool gated, mic_stats_t *st)
{
    static const int16_t zeros[AUDIO_FRAME_SAMPLES];
    if (gated) {
        // Agent speech or its tail: zeros as before, and nothing from here may become pre-roll.
        s_talking = false;
        s_hangover_left = 0;
        s_preroll_count = 0;
        speech = false;
    } else {
        st->ns_sq += sum_squares(frame, AUDIO_FRAME_SAMPLES);
        st->ns_samples += AUDIO_FRAME_SAMPLES;
        if (speech) {
            st->vad_frames++;
        }
    }
    if (speech) {
        if (!s_talking) {
            s_talking = true;
            int start = (s_preroll_head - s_preroll_count + PREROLL_FRAMES) % PREROLL_FRAMES;
            for (int i = 0; i < s_preroll_count; ++i) {
                enqueue_frame(s_preroll[(start + i) % PREROLL_FRAMES]);
            }
            st->sent_frames += s_preroll_count;
            s_preroll_debt += s_preroll_count;
            s_preroll_count = 0;
        }
        s_hangover_left = HANGOVER_FRAMES;
        enqueue_frame(frame);
        st->sent_frames++;
        return;
    }
    if (s_talking && s_hangover_left > 0) {
        if (--s_hangover_left == 0) {
            s_talking = false;
        }
        enqueue_frame(frame);
        st->sent_frames++;
        return;
    }
    if (!gated) {
        memcpy(s_preroll[s_preroll_head], frame, sizeof(s_preroll[0]));
        s_preroll_head = (s_preroll_head + 1) % PREROLL_FRAMES;
        if (s_preroll_count < PREROLL_FRAMES) {
            s_preroll_count++;
        }
    }
    if (s_preroll_debt > 0) {
        s_preroll_debt--;
        return;
    }
    enqueue_frame(zeros);
}

// CPU load per core over the last stats second, from the idle tasks' run time.
static void cpu_load(int out[2])
{
    static uint32_t last_idle[2];
    static int64_t last_at;
    const int64_t now = esp_timer_get_time();
    for (int core = 0; core < 2; ++core) {
        const uint32_t idle = ulTaskGetIdleRunTimeCounterForCore(core);
        const int64_t span = now - last_at;
        out[core] = last_at == 0 || span <= 0 ? -1 : 100 - (int)((int64_t)(idle - last_idle[core]) * 100 / span);
        last_idle[core] = idle;
    }
    last_at = now;
}

// The microphone's I2S clock paces the uplink: one 20 ms frame goes out per 20 ms of mic audio.
// The mic is open for the whole call, except that frames are zeros while agent audio plays.
static void uplink_task(void *arg)
{
    (void)arg;
    static int16_t mic[MIC_FRAME_SAMPLES];
    static int16_t frame[AUDIO_FRAME_SAMPLES];
    mic_stats_t st = {0};
    int filled = 0;
    bool recording = false;
    bool was_gated = false;
    int64_t speaker_quiet_since = 0;
    int64_t stats_at = 0;
    int stall_logged_at = 0;
    clean_gate_reset();
    voice_clean_reset();
    while (!s_stop) {
        if (!s_start_sent && s_ws_up) {
            const char *start = "{\"type\":\"session.start\",\"audio_format\":\"" WIRE_FORMAT "\",\"client\":\""
                                CONFIG_COMMA_CLIENT_SOURCE "/" COMMA_FIRMWARE_VERSION "\"}";
            if (send_text(start, 2000) < 0) {
                ESP_LOGE(TAG, "session.start send failed");
                s_stop = true;
                break;
            }
            s_start_sent = true;
            ESP_LOGI(TAG, "sent session.start");
        }
        if (s_state != COMMA_VOICE_UP) {
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
        int got = board_audio_read_mono(mic + filled, MIC_FRAME_SAMPLES - filled);
        if (got < 0) {
            ESP_LOGE(TAG, "microphone read failed");
            s_stop = true;
            break;
        }
        filled += got;
        if (filled < MIC_FRAME_SAMPLES) {
            continue;
        }
        filled = 0;

        int64_t now = esp_timer_get_time();
        int64_t started = s_started_us;
        if (started != 0 && playback_ringing() && now - started >= GREETING_WAIT_US) {
            ESP_LOGW(TAG, "agent silent %lld ms after session.started: ringback off, mic open",
                     (long long)((now - started) / 1000));
            playback_tone(PLAYBACK_TONE_CONNECTED);
        }
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
            for (int i = 0; i < MIC_FRAME_SAMPLES; i++) {
                int magnitude = mic[i] < 0 ? -(int)mic[i] : mic[i];
                if (magnitude > st.peak) {
                    st.peak = magnitude;
                }
            }
            st.sum_sq += sum_squares(mic, MIC_FRAME_SAMPLES);
            st.open_samples += MIC_FRAME_SAMPLES;
        } else {
            st.echo_sq += sum_squares(mic, MIC_FRAME_SAMPLES);
            st.echo_samples += MIC_FRAME_SAMPLES;
        }
        if (idle != 0 && now - idle >= IDLE_HANGUP_US) {
            ESP_LOGI(TAG, "idle hangup: 60 s without a final caller transcript or agent audio");
            s_idle_hangup = true;
            s_stop = true;
            break;
        }
        // The AFE also hears the mic while it's muted, so its noise estimate never goes stale.
        voice_clean_feed(mic);
        bool speech = false;
        while (voice_clean_pop(frame, &speech)) {
            clean_gate_frame(frame, speech, gated, &st);
        }
        // Stall lines come from here, not the sender, which may itself be stuck inside a send.
        int64_t waiting_since = s_wait_since_us;
        if (waiting_since != 0 && now - waiting_since >= (stall_logged_at + 1) * STALL_LOG_MS * 1000LL &&
            stall_logged_at < 64) {
            tcp_probe_t tcp;
            const bool known = tcp_probe(&tcp);
            log_stall(stall_logged_at == 0 ? "stall" : "still stalled", now - waiting_since, &tcp, known);
            // Every 1, 2, 4, 8 s: enough to follow a long stall without flooding the log.
            stall_logged_at = stall_logged_at == 0 ? 1 : stall_logged_at * 2;
        } else if (waiting_since == 0 && stall_logged_at != 0) {
            tcp_probe_t tcp;
            const bool known = tcp_probe(&tcp);
            log_stall("stall over", s_send_us, &tcp, known);
            stall_logged_at = 0;
        }
        if (now - stats_at >= 1000000) {
            int rx_bytes = 0;
            int rx_peak = 0;
            playback_take_rx(&rx_bytes, &rx_peak);
            const int rx_ms = rx_bytes / (AUDIO_SAMPLE_RATE_HZ * (int)sizeof(int16_t) / 1000);
            s_rx_ms_last = rx_ms;
            // A send still waiting counts toward send_max, so a stall shows while it lasts.
            int64_t send_max_us = s_send_max_us;
            if (waiting_since != 0 && now - waiting_since > send_max_us) {
                send_max_us = now - waiting_since;
            }
            tcp_probe_t tcp;
            const bool known = tcp_probe(&tcp);
            char wifi[64];
            describe_wifi(wifi, sizeof(wifi));
            ESP_LOGI(TAG,
                     "stats rx_ms=%d rx_peak=%d queue_ms=%d marks=%d send_ms=%lld send_max_ms=%lld mic_backlog_ms=%d mic_dropped=%d mic=%s "
                     "mic_peak=%d",
                     rx_ms, rx_peak,
                     queued_bytes / (AUDIO_SAMPLE_RATE_HZ * (int)sizeof(int16_t) / 1000), pending_marks,
                     (long long)(s_send_us / 1000), (long long)(send_max_us / 1000),
                     (int)xStreamBufferBytesAvailable(s_mic_queue) / WIRE_BYTES_PER_MS,
                     (int)s_mic_dropped, gated ? "muted" : "open", st.peak);
            // mic_rms: raw mic while open; echo_rms: raw mic while muted for agent speech.
            int cpu[2];
            cpu_load(cpu);
            ESP_LOGI(TAG,
                     "voice mic_rms=%d echo_rms=%d ns_rms=%d vad_ms=%d sent_ms=%d preroll_debt_ms=%d afe_ms=%d cpu0=%d cpu1=%d "
                     "stack_free=%u",
                     rms_of(st.sum_sq, st.open_samples), rms_of(st.echo_sq, st.echo_samples),
                     rms_of(st.ns_sq, st.ns_samples), st.vad_frames * AUDIO_FRAME_MS, st.sent_frames * AUDIO_FRAME_MS,
                     s_preroll_debt * AUDIO_FRAME_MS, (int)(voice_clean_take_busy_us() / 1000), cpu[0], cpu[1],
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
            if (known) {
                ESP_LOGI(TAG, "net %s tcp_unacked=%u (max %u) tcp_sndbuf=%u tcp_q=%u peer_wnd=%u cwnd=%u nrtx_max=%u rto_ms=%d srtt_ms=%d", wifi,
                         (unsigned)tcp.unacked, (unsigned)s_tcp_unacked_max, (unsigned)tcp.snd_buf, (unsigned)tcp.queue_len,
                         (unsigned)tcp.snd_wnd, (unsigned)tcp.cwnd, (unsigned)s_tcp_nrtx_max, tcp.rto_ms, tcp.srtt_ms);
            } else {
                ESP_LOGI(TAG, "net %s tcp=?", wifi);
            }
            stats_at = now;
            s_send_max_us = 0;
            s_mic_dropped = 0;
            s_tcp_unacked_max = 0;
            s_tcp_nrtx_max = 0;
            st = (mic_stats_t){0};
        }
    }
    if (recording) {
        board_audio_set_recording(false);
    }
    play_end_tone();
    // The sender writes to s_client, so it must be gone before the client is destroyed.
    s_stop = true;
    while (s_sender_running) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_client && esp_websocket_client_is_connected(s_client)) {
        // session.end before session.start is a 4400, so just close.
        if (s_start_sent) {
            send_text("{\"type\":\"session.end\"}", 2000);
            ESP_LOGI(TAG, "sent session.end");
            int64_t until = esp_timer_get_time() + 1000000;
            while (s_state == COMMA_VOICE_UP && esp_timer_get_time() < until) {
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
    if (s_state == COMMA_VOICE_UP || s_state == COMMA_VOICE_CONNECTING) {
        s_state = COMMA_VOICE_ENDED;
    }
    s_ws_up = false;
    s_uplink_running = false;
    vTaskDelete(NULL);
}

esp_err_t comma_voice_start(void)
{
    if (s_key_rejected) {
        ESP_LOGE(TAG, "not retrying after close 4401");
        return ESP_ERR_INVALID_STATE;
    }
    comma_voice_state_t state = s_state;
    if (s_uplink_running || state == COMMA_VOICE_CONNECTING || state == COMMA_VOICE_UP) {
        return ESP_ERR_INVALID_STATE;
    }
    device_config_t *dc = calloc(1, sizeof(*dc));
    if (!dc) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t cfg_err = ESP_OK;
    if (!device_config_get(dc)) {
        ESP_LOGE(TAG, "not provisioned");
        cfg_err = ESP_ERR_INVALID_STATE;
    } else if (strncmp(dc->voice_key, "salix_vk_", 9) != 0) {
        ESP_LOGE(TAG, "voice key is not a voice agent key");
        cfg_err = ESP_ERR_INVALID_STATE;
    } else if (!session_url(dc->server, dc->group_id, s_uri, sizeof(s_uri))) {
        ESP_LOGE(TAG, "bad voice server or group");
        cfg_err = ESP_ERR_INVALID_ARG;
    } else {
        int header_n = snprintf(s_headers, sizeof(s_headers), "Authorization: Bearer %s\r\n", dc->voice_key);
        if (header_n < 0 || (size_t)header_n >= sizeof(s_headers)) {
            cfg_err = ESP_ERR_INVALID_SIZE;
        }
    }
    memset(dc, 0, sizeof(*dc));
    free(dc);
    if (cfg_err != ESP_OK) {
        return cfg_err;
    }
    if (!s_frame.buf) {
        s_frame.buf = heap_caps_malloc(RX_MAX + 1, COMMA_BUFFER_CAPS);
        if (!s_frame.buf) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (!s_mic_queue) {
        s_mic_queue_storage = heap_caps_malloc(MIC_QUEUE_BYTES + 1, COMMA_BUFFER_CAPS);
        if (!s_mic_queue_storage) {
            return ESP_ERR_NO_MEM;
        }
        s_mic_queue = xStreamBufferCreateStatic(MIC_QUEUE_BYTES, 1, s_mic_queue_storage, &s_mic_queue_struct);
    }
    if (!s_preroll) {
        s_preroll = heap_caps_malloc(PREROLL_FRAMES * sizeof(s_preroll[0]), COMMA_BUFFER_CAPS);
        if (!s_preroll) {
            return ESP_ERR_NO_MEM;
        }
    }
    xStreamBufferReset(s_mic_queue);
    s_mic_dropped = 0;
    s_send_us = 0;
    s_send_max_us = 0;
    s_wait_since_us = 0;
    s_rx_ms_last = 0;
    s_tcp_unacked_max = 0;
    s_tcp_nrtx_max = 0;
    playback_clear();
    s_stop = false;
    s_ws_up = false;
    s_start_sent = false;
    s_close_code = 0;
    s_user_hangup = false;
    s_idle_hangup = false;
    s_session_ended = false;
    s_started_us = 0;
    s_last_end = COMMA_VOICE_END_NONE;
    s_idle_us = 0;
    ws_frame_reset(&s_frame);
    s_state = COMMA_VOICE_CONNECTING;

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
        // Also the write timeout for the websocket task's own PING/PONG. Those aren't gated, but a
        // failed one is ignored (no abort), and it matches UPLINK_STALL_LIMIT_MS.
        .network_timeout_ms = 10000,
        .ping_interval_sec = 10,
        // Longer than UPLINK_STALL_LIMIT_MS, so a stall ends with our own logged hangup first.
        .pingpong_timeout_sec = 20,
    };
    if (strncmp(s_uri, "ws://", 5) == 0) {
        cfg.transport = WEBSOCKET_TRANSPORT_OVER_TCP;
        cfg.crt_bundle_attach = NULL;
    }
    s_client = esp_websocket_client_init(&cfg);
    if (!s_client) {
        s_state = COMMA_VOICE_FAILED;
        return ESP_FAIL;
    }
    ESP_ERROR_CHECK(esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, on_ws, NULL));
    tcp_remember_existing();
    esp_err_t err = esp_websocket_client_start(s_client);
    if (err != ESP_OK) {
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
        s_state = COMMA_VOICE_FAILED;
        return err;
    }
    s_sender_running = true;
    if (xTaskCreate(sender_task, "sender", 5120, NULL, 6, NULL) != pdPASS) {
        s_sender_running = false;
        s_stop = true;
        esp_websocket_client_stop(s_client);
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
        s_state = COMMA_VOICE_FAILED;
        return ESP_ERR_NO_MEM;
    }
    s_uplink_running = true;
    if (xTaskCreate(uplink_task, "uplink", UPLINK_STACK, NULL, 5, NULL) != pdPASS) {
        s_uplink_running = false;
        s_stop = true;
        while (s_sender_running) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        esp_websocket_client_stop(s_client);
        esp_websocket_client_destroy(s_client);
        s_client = NULL;
        s_state = COMMA_VOICE_FAILED;
        return ESP_ERR_NO_MEM;
    }
    playback_ringback(true);
    ESP_LOGI(TAG, "dialing %s", s_uri);
    return ESP_OK;
}

void comma_voice_credentials_changed(void)
{
    s_key_rejected = false;
}

void comma_voice_end(void)
{
    if (s_state == COMMA_VOICE_CONNECTING || s_state == COMMA_VOICE_UP) {
        ESP_LOGI(TAG, "hangup requested");
        s_user_hangup = true;
        s_stop = true;
    }
}

bool comma_voice_can_start(void)
{
    comma_voice_state_t state = s_state;
    return !s_key_rejected && !s_uplink_running && !s_sender_running && state != COMMA_VOICE_CONNECTING && state != COMMA_VOICE_UP;
}

comma_voice_state_t comma_voice_state(void)
{
    return s_state;
}

bool comma_voice_ending(void)
{
    const comma_voice_state_t state = s_state;
    return s_uplink_running && (s_stop || (state != COMMA_VOICE_CONNECTING && state != COMMA_VOICE_UP));
}

comma_voice_end_t comma_voice_last_end(void)
{
    return (comma_voice_end_t)s_last_end;
}

int comma_voice_close_code(void)
{
    return s_close_code;
}
