// SPDX-License-Identifier: Apache-2.0
#include "connector.h"

#include "connector_json.h"
#include "device_config.h"
#include "firmware_config.h"
#include "heap_diag.h"
#include "json_buf.h"
#include "lan_discover.h"
#include "lan_http.h"
#include "lan_tunnel.h"
#include "lan_udp.h"
#include "memory_caps.h"
#include "net_watchdog.h"
#include "provisioning.h"
#include "voice_state.h"
#include "wifi.h"
#include "ws_frame.h"

#include <ctype.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/api.h"
#include "lwip/priv/sockets_priv.h"
#include "lwip/priv/tcpip_priv.h"
#include "lwip/sockets.h"
#include "lwip/tcp.h"

// Largest request frame: room for an http_request body as big as the response cap, plus the
// envelope. A larger text frame is dropped; if its id can be read from the first chunk, it still
// gets an error so the caller does not wait out its timeout. PSRAM.
#define RX_MAX CONFIG_COMMA_CONNECTOR_RX_MAX
#define ID_MAX 64
// The server heartbeats every 15 s. Like the desktop Connector, 45 s without any frame means the link is dead.
#define READ_TIMEOUT_US (45LL * 1000000)
#define STATS_PERIOD_US (30LL * 1000000)
#define BACKOFF_MIN_MS 1000
#define BACKOFF_MAX_MS 60000
// A connection that lasted this long resets the backoff.
#define STABLE_US (60LL * 1000000)
#define SEND_TIMEOUT_MS 5000
// A reply with a 64 KB body can be several hundred KB of JSON.
#define SEND_LARGE_TIMEOUT_MS 20000
// Below the voice tasks (websocket 5, sender 6, uplink 5): the Connector only ever waits.
#define CONNECTOR_TASK_PRIO 2
#define CONNECTOR_WS_PRIO 3
// LAN requests (http_request, udp_request, lan_discover) run one at a time on their own task so the Connector
// task keeps answering heartbeats. A few more wait in line; past that the device answers "busy".
#define LAN_WORKER_PRIO 2
#define LAN_WORKER_STACK 8192
#define LAN_QUEUE_LEN 4

// Chunked http_request replies go out from the LAN worker as they are read, never through the
// event queue, so a body is never held whole. Like the voice sender, a frame is written only
// when the Connector socket passes lwIP's select() writable test, which guarantees more than
// TCP_SNDLOWAT (12 KB of the 24 KB send buffer) free. A frame of 8 KB raw is ~11 KB of JSON, so the
// write neither waits in poll_write nor blocks in the socket while holding the client's TX lock, and
// the websocket task's PING/PONG never times out on that lock behind it.
#define CHUNK_GATED_BYTES CONFIG_COMMA_LAN_HTTP_CHUNK_FRAME_BYTES
// No room for a frame for this long: give up with an error frame (the voice sender's no-progress limit).
#define CHUNK_STALL_LIMIT_MS 10000
// No chunk transfers during a voice call, for now: on the board a transfer alongside a call froze
// the whole Wi-Fi link. A request that arrives during a call is refused
// before any network use; a call that starts mid-transfer ends the transfer with the same error.
#define CALL_BUSY_MESSAGE "busy: voice call active"
#define BUSY_MESSAGE "the device is busy with other LAN requests; try again shortly"
// The refusal while the user has not allowed operations, worded like the desktop Connector's so the
// agent sees the same refusal.
#define READ_ONLY_REFUSAL(method) "method \"" method "\" is not permitted for a local_file_read connector"
#define OOM_MESSAGE "out of memory on this device"
// An error frame for a request id of up to ID_MAX bytes, every byte escaped as \u00XX.
#define OOM_FRAME_MAX (ID_MAX * 6 + 96)

// Dev builds only (main/CMakeLists.txt): dial this URL instead of the provisioned one, e.g.
// ws://<mac-ip>:8765/v1/connect for tools/fake_connector.py. The saved credentials are neither
// changed nor sent: the dial carries a dummy token.
#ifdef COMMA_CONNECT_URL_OVERRIDE
#define DEV_OVERRIDE_TOKEN "salix_conn_dev_override"
#endif

#define SCOPE_READ_ONLY "local_file_read"
#define SCOPE_OPERATIONS ""

static const char *TAG = "connector";

typedef enum {
    EV_OPEN,
    EV_TEXT,     // data: NUL-terminated frame, PSRAM, freed by the receiver
    EV_NO_MEMORY, // data: request id when recoverable; NULL closes the session
    EV_TOO_BIG,  // data: request id, or NULL when it could not be read
    EV_CLOSED,
    EV_REPLY,    // data: a whole reply frame from the LAN worker, PSRAM, for socket `session`
} event_kind_t;

typedef struct {
    event_kind_t kind;
    char *data;
    unsigned session;
    int64_t received_us;
} event_t;

typedef enum {
    LAN_HTTP_REQUEST,
    LAN_UDP_REQUEST,
    LAN_DISCOVER,
} lan_method_t;

typedef struct {
    char id[ID_MAX + 1];
    lan_method_t method;
    cJSON *params;     // detached from the request frame, freed by the worker
    unsigned session;  // the socket the request came in on
    unsigned generation;
    int64_t deadline_us;
} lan_job_t;

typedef struct {
    char url[160];
    char token[160];
    char name[64];        // the name saved at setup, or Comma-XXXX
    char alias[16];       // comma-xxxx: stable across renames, ends in the short id the backend matches
    unsigned generation;  // device_config_generation() these were read at
} credentials_t;

static QueueHandle_t s_events;
static esp_websocket_client_handle_t s_client;
// Held by the LAN worker while it sends a chunk on s_client, and by the Connector task while it
// replaces or destroys s_client.
static SemaphoreHandle_t s_client_mutex;
// The Connector's socket, for the chunk writable gate; -1 when it couldn't be identified.
static atomic_int s_conn_fd = -1;
static struct sockaddr_in s_conn_peer;  // what s_conn_fd was connected to, to notice reuse
// Before each dial: which lwIP sockets were connected, and to what.
static struct sockaddr_in s_fd_before[CONFIG_LWIP_MAX_SOCKETS];
static atomic_bool s_up;
static atomic_int s_close_code;
static atomic_int s_handshake_status;
static atomic_uint s_credentials_generation;
static QueueHandle_t s_lan_jobs;
static bool s_lan_ready;
// Counts sockets. A reply only goes out on the socket its request came in on; the server has
// already failed requests from a closed one.
static atomic_uint s_session;
static char s_instance_id[37];
// Room for a 159-byte connect_url plus a 63-byte name fully percent-encoded.
static char s_uri[512];
static char s_headers[512];
// The name of the current dial, for metadata's hostname. Connector task only.
static char s_name[64];

// Incoming frames, reassembled in the websocket task. PSRAM, RX_MAX + 1 bytes.
static ws_frame_t s_frame = {.capacity = RX_MAX};

static int s_reconnects;
// WebSocket PONGs since the socket opened, counted in the websocket task. A PONG proves the server's
// WebSocket layer is reading our PINGs even when no text frame arrives.
static atomic_int s_pongs;
static int64_t s_heartbeat_us;
static int64_t s_stats_us;

// The one place credentials come from: provisioned over BLE into NVS (device_config), or secrets.h
// with COMMA_DEV_SECRETS on a dev board.
static bool connector_credentials(credentials_t *out)
{
    out->generation = device_config_generation();
    device_config_t *cfg = calloc(1, sizeof(*cfg));
    if (!cfg) {
        return false;
    }
    const bool provisioned = device_config_get(cfg);
    strlcpy(out->url, cfg->connect_url, sizeof(out->url));
    strlcpy(out->token, cfg->connector_token, sizeof(out->token));
    strlcpy(out->name, cfg->device_name, sizeof(out->name));
    memset(cfg, 0, sizeof(*cfg));
    // The Android app sends device_name at setup and mints the token with it. An older or empty
    // provisioning falls back to the BLE name, never a model name the backend cannot match.
    char ble_name[16];
    provisioning_ble_name(ble_name, sizeof(ble_name));
    if (out->name[strspn(out->name, " ")] == '\0') {
        strlcpy(out->name, ble_name, sizeof(out->name));
    }
    strlcpy(out->alias, ble_name, sizeof(out->alias));
    for (char *c = out->alias; *c; ++c) {
        *c = (char)tolower((unsigned char)*c);
    }
    free(cfg);
#ifdef COMMA_CONNECT_URL_OVERRIDE
    (void)provisioned;
    strlcpy(out->url, COMMA_CONNECT_URL_OVERRIDE, sizeof(out->url));
    strlcpy(out->token, DEV_OVERRIDE_TOKEN, sizeof(out->token));
    return true;
#endif
    return provisioned && strncmp(out->url, "ws", 2) == 0 && strncmp(out->token, "salix_conn_", 11) == 0;
}

// New credentials were saved (pairing mode) since these were read.
static bool credentials_stale(const credentials_t *creds)
{
    return device_config_generation() != creds->generation;
}

static bool operations_allowed(void)
{
    return device_config_operations_allowed(s_credentials_generation);
}

static const char *current_scope(void)
{
    return operations_allowed() ? SCOPE_OPERATIONS : SCOPE_READ_ONLY;
}

static void make_instance_id(void)
{
    uint8_t b[16];
    esp_fill_random(b, sizeof(b));
    b[6] = (b[6] & 0x0F) | 0x40;
    b[8] = (b[8] & 0x3F) | 0x80;
    snprintf(s_instance_id, sizeof(s_instance_id),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1], b[2], b[3], b[4],
             b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

static void post_session(event_kind_t kind, char *data, unsigned session)
{
    event_t ev = {.kind = kind, .data = data, .session = session, .received_us = esp_timer_get_time()};
    // The LAN worker can afford to wait; a dropped reply leaves the caller waiting out its timeout.
    const TickType_t wait = pdMS_TO_TICKS(kind == EV_REPLY ? 5000 : 100);
    if (xQueueSend(s_events, &ev, wait) != pdTRUE) {
        ESP_LOGW(TAG, "event queue full, dropped event %d", kind);
        free(data);
    }
}

static void post(event_kind_t kind, char *data)
{
    post_session(kind, data, s_session);
}

static char *psram_strndup(const char *src, size_t len)
{
    char *out = heap_caps_malloc(len + 1, COMMA_BUFFER_CAPS);
    if (out) {
        memcpy(out, src, len);
        out[len] = 0;
    }
    return out;
}

// Best effort: find a top-level "id":"req_..." in the first chunk of an oversized frame. A quote
// escaped inside a string value is preceded by a backslash, which rules out ids in bodies.
static char *find_request_id(const char *data, size_t len)
{
    static const char key[] = "\"id\":\"";
    const size_t key_len = sizeof(key) - 1;
    for (size_t i = 0; i + key_len < len; ++i) {
        if (memcmp(data + i, key, key_len) != 0 || (i > 0 && data[i - 1] == '\\')) {
            continue;
        }
        const char *start = data + i + key_len;
        size_t n = 0;
        while (start + n < data + len && n < ID_MAX &&
               (start[n] == '_' || start[n] == '-' || (start[n] >= '0' && start[n] <= '9') ||
                (start[n] >= 'a' && start[n] <= 'z') || (start[n] >= 'A' && start[n] <= 'Z'))) {
            n++;
        }
        if (n > 0 && start + n < data + len && start[n] == '"') {
            return psram_strndup(start, n);
        }
    }
    return NULL;
}

static void consume_chunk(const esp_websocket_event_data_t *event)
{
    switch (ws_frame_feed(&s_frame, event)) {
    case WS_FRAME_CONTROL:
        if (event->op_code == WS_OP_PONG) {
            s_pongs++;
        }
        break;
    case WS_FRAME_TOO_BIG:
        ESP_LOGW(TAG, "drop oversized frame: %u bytes", (unsigned)s_frame.len);
        if (s_frame.opcode == WS_OP_TEXT) {
            post(EV_TOO_BIG, find_request_id(event->data_ptr, (size_t)event->data_len));
        }
        break;
    case WS_FRAME_DONE:
        if (s_frame.opcode == WS_OP_CLOSE) {
            s_close_code = s_frame.len >= 2 ? (s_frame.buf[0] << 8) | s_frame.buf[1] : 0;
        } else if (s_frame.opcode == WS_OP_TEXT) {
            char *text = psram_strndup((const char *)s_frame.buf, s_frame.len);
            if (text) {
                post(EV_TEXT, text);
            } else {
                comma_heap_log("rx_copy_oom");
                ESP_LOGW(TAG, "out of memory for a %u-byte frame", (unsigned)s_frame.len);
                post(EV_NO_MEMORY, find_request_id((const char *)s_frame.buf, s_frame.len));
            }
        }
        // The server ignores binary frames, so it never sends any.
        break;
    case WS_FRAME_PENDING:
        break;
    }
}

// Runs in the websocket client's task. Never sends: replies go out from connector_task.
static void on_ws(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    const esp_websocket_event_data_t *event = data;
    if (id == WEBSOCKET_EVENT_CONNECTED) {
        s_up = true;
        post(EV_OPEN, NULL);
    } else if (id == WEBSOCKET_EVENT_DATA) {
        consume_chunk(event);
    } else if (id == WEBSOCKET_EVENT_DISCONNECTED || id == WEBSOCKET_EVENT_CLOSED || id == WEBSOCKET_EVENT_ERROR) {
        int http = event ? event->error_handle.esp_ws_handshake_status_code : 0;
        if (http != 0) {
            s_handshake_status = http;
        }
        // ERROR and DISCONNECTED can both arrive; the session ends on the first, the rest are drained.
        s_up = false;
        post(EV_CLOSED, NULL);
    }
}

static bool send_json(const char *json)
{
    if (!s_client || !esp_websocket_client_is_connected(s_client)) {
        return false;
    }
    const size_t len = strlen(json);
    const int timeout_ms = len > 16 * 1024 ? SEND_LARGE_TIMEOUT_MS : SEND_TIMEOUT_MS;
    int sent = esp_websocket_client_send_text(s_client, json, (int)len, pdMS_TO_TICKS(timeout_ms));
    if (sent < 0) {
        ESP_LOGW(TAG, "send failed");
        return false;
    }
    return true;
}

static bool send_object(cJSON *obj)
{
    char *json = obj ? cJSON_PrintUnformatted(obj) : NULL;
    cJSON_Delete(obj);
    if (!json) {
        ESP_LOGW(TAG, "out of memory building a frame");
        return false;
    }
    bool ok = send_json(json);
    cJSON_free(json);
    return ok;
}

static void send_metadata(void)
{
    char collected_at[40] = "";
    time_t now = time(NULL);
    // No SNTP on the board: leave the timestamp out rather than send 1970.
    if (now > 1700000000) {
        snprintf(collected_at, sizeof(collected_at), ",\"collected_at\":%lld", (long long)now);
    }
    // hostname is what Settings › Devices shows (until the user renames it there).
    cJSON *hostname_str = cJSON_CreateString(s_name);
    char *hostname = hostname_str ? cJSON_PrintUnformatted(hostname_str) : NULL;
    cJSON_Delete(hostname_str);
    if (!hostname) {
        ESP_LOGE(TAG, "metadata: out of memory");
        return;
    }
    // Connector task only, so static keeps it off the task stack.
    static char json[1280]; // includes probe limits and a maximally JSON-escaped device name
    int n = snprintf(json, sizeof(json),
                     "{\"type\":\"metadata\","
                     "\"capabilities\":{\"scope\":\"%s\",\"device_access\":true,\"execution_boundary\":\"lan-gateway\","
                     "\"persistent_processes\":false,\"computer_use_tool\":false,\"host_access_required\":false,"
                     "\"runtime_probe\":false,\"meeting_runtime\":false,"
                     // Not part of the desktop Connector's set: tells the backend what this gateway does.
                     "\"lan_gateway\":{\"methods\":[\"http_request\",\"udp_request\""
#if CONFIG_COMMA_LAN_DISCOVER
                     ",\"lan_discover\""
#endif
#if CONFIG_COMMA_LAN_TUNNELS > 0
                     ",\"lan_tcp_dial\""
#endif
                     "],\"max_bytes\":%d,"
                     "\"max_timeout_seconds\":%d,"
                     "\"response_chunks\":{\"max_total_bytes\":%d,\"chunk_bytes\":%d}"
#if CONFIG_COMMA_LAN_DISCOVER && CONFIG_COMMA_LAN_DISCOVER_PROBES > 0
                     ",\"discover_probes\":{\"max_probes\":%d,\"max_payload_bytes\":%d,"
                     "\"max_answer_bytes\":%d,\"max_total_answer_bytes\":%d}"
#endif
                     "},"
                     "\"component_versions\":{\"" CONFIG_COMMA_CLIENT_SOURCE "\":\"" COMMA_FIRMWARE_VERSION "\"}},"
                     "\"skills\":[],"
                     "\"system_info\":{\"client_source\":\"" CONFIG_COMMA_CLIENT_SOURCE "\",\"hostname\":%s,\"os_type\":\"esp-idf\","
                     "\"arch\":\"xtensa\"%s}}",
                     current_scope(), LAN_HTTP_MAX_BYTES, LAN_HTTP_TIMEOUT_MAX_S, LAN_HTTP_CHUNK_MAX_TOTAL,
                     LAN_HTTP_CHUNK_BYTES,
#if CONFIG_COMMA_LAN_DISCOVER && CONFIG_COMMA_LAN_DISCOVER_PROBES > 0
                     CONFIG_COMMA_LAN_DISCOVER_PROBES, CONFIG_COMMA_LAN_DISCOVER_PROBE_PAYLOAD,
                     CONFIG_COMMA_LAN_DISCOVER_PROBE_ANSWER, CONFIG_COMMA_LAN_DISCOVER_PROBE_BUDGET,
#endif
                     hostname, collected_at);
    cJSON_free(hostname);
    if (n <= 0 || (size_t)n >= sizeof(json)) {
        ESP_LOGE(TAG, "metadata does not fit");
        return;
    }
    if (send_json(json)) {
        ESP_LOGI(TAG, "sent metadata scope=%s", operations_allowed() ? "\"\"" : SCOPE_READ_ONLY);
    }
}

// Built on the stack: it answers an allocation failure.
static void send_oom(const char *id)
{
    char frame[OOM_FRAME_MAX];
    if (json_error_frame_into(frame, sizeof(frame), id, OOM_MESSAGE)) {
        send_json(frame);
    }
}

static void send_error(const char *id, const char *message)
{
    char *frame = json_error_frame(id, message);
    if (!frame) {
        ESP_LOGW(TAG, "out of memory building a frame");
        return;
    }
    send_json(frame);
    free(frame);
}

static void handle_device_access(const char *id, const cJSON *params)
{
    const cJSON *allow = cJSON_IsObject(params) ? cJSON_GetObjectItemCaseSensitive(params, "allow_operations") : NULL;
    if (!cJSON_IsBool(allow)) {
        send_error(id, "allow_operations must be a boolean");
        return;
    }
    const bool value = cJSON_IsTrue(allow);
    const bool was_allowed = operations_allowed();
    esp_err_t err = device_config_set_operations_allowed(s_credentials_generation, value);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving access failed: %s", esp_err_to_name(err));
        send_error(id, "could not save device access");
        return;
    }
    if (value != was_allowed) {
        ESP_LOGI(TAG, "access: %s", value ? "operations allowed" : "read-only");
    }
    // The server learns the new scope only from metadata, so it goes out before the response.
    send_metadata();
    cJSON *obj = cJSON_CreateObject();
    cJSON *result = cJSON_CreateObject();
    if (obj && result) {
        cJSON_AddStringToObject(obj, "id", id);
        cJSON_AddStringToObject(obj, "type", "response");
        cJSON_AddBoolToObject(result, "allows_operations", value);
        cJSON_AddItemToObject(obj, "result", result);
        result = NULL;
    }
    cJSON_Delete(result);
    send_object(obj);
}

static const char *lan_method_name(lan_method_t method)
{
    return method == LAN_DISCOVER ? "lan_discover" : method == LAN_UDP_REQUEST ? "udp_request" : "http_request";
}

static const char *read_only_refusal(lan_method_t method)
{
    return method == LAN_DISCOVER      ? READ_ONLY_REFUSAL("lan_discover")
           : method == LAN_UDP_REQUEST ? READ_ONLY_REFUSAL("udp_request")
                                       : READ_ONLY_REFUSAL("http_request");
}

// The access gate, for every method that touches the LAN. The server does not check access on the
// device-placed MCP path, so this is the only thing between the cloud and the LAN while the user has
// not allowed operations.
static void handle_lan_request(const char *id, lan_method_t method, cJSON *root, int64_t received_us)
{
    if (!operations_allowed()) {
        ESP_LOGW(TAG, "%s refused: read-only", id);
        send_error(id, read_only_refusal(method));
        return;
    }
    if (!s_lan_ready) {
        char message[96];
        snprintf(message, sizeof(message), "%s is unavailable: the device could not start its LAN worker",
                 lan_method_name(method));
        send_error(id, message);
        return;
    }
    if (strlen(id) > ID_MAX) {
        send_error(id, "request id too long for this device");
        return;
    }
    lan_job_t job = {.session = s_session, .generation = s_credentials_generation, .method = method};
    strlcpy(job.id, id, sizeof(job.id));
    job.params = cJSON_DetachItemFromObjectCaseSensitive(root, "params");
    const int seconds = method == LAN_HTTP_REQUEST ? lan_http_timeout_seconds(job.params) : 30;
    job.deadline_us = received_us + (int64_t)seconds * 1000000;
    if (xQueueSend(s_lan_jobs, &job, 0) != pdTRUE) {
        comma_heap_log("lan_result_ready");
        cJSON_Delete(job.params);
        send_error(id, BUSY_MESSAGE);
    }
}

// ---- Chunked replies (http_request response_mode "chunks") --------------------------------------

typedef struct {
    unsigned session;  // the socket the request came in on; chunks never go out on another
    const lan_request_t *request;
    unsigned frames;
    size_t wire_bytes;
    int64_t send_max_us;  // longest gate wait + write for one frame
    int64_t started_us;
    size_t internal_min;  // lowest internal free seen during the transfer
    bool gated;           // every frame passed the writable gate
} chunk_ctx_t;

static const char *chunk_refuse(void *arg)
{
    (void)arg;
    return comma_voice_active() ? CALL_BUSY_MESSAGE : NULL;
}

static size_t chunk_frame_bytes(void *arg)
{
    (void)arg;
    // Unidentified socket: no gate, so fewer, bigger frames; each write may block (up to the limit).
    return s_conn_fd >= 0 ? CHUNK_GATED_BYTES : LAN_HTTP_CHUNK_BYTES;
}

// The socket's peer, or false when fd is not a connected socket.
static bool fd_peer(int fd, struct sockaddr_in *peer)
{
    socklen_t len = sizeof(*peer);
    memset(peer, 0, sizeof(*peer));
    return getpeername(fd, (struct sockaddr *)peer, &len) == 0 && peer->sin_family == AF_INET;
}

static bool same_peer(const struct sockaddr_in *a, const struct sockaddr_in *b)
{
    return a->sin_port == b->sin_port && a->sin_addr.s_addr == b->sin_addr.s_addr;
}

// Waits until the Connector socket has room for a gated frame. False after CHUNK_STALL_LIMIT_MS
// without room, or when the socket is gone.
static bool chunk_wait_room(chunk_ctx_t *c, char *err, size_t err_size)
{
    const int64_t since = esp_timer_get_time();
    for (;;) {
        const char *cancelled = lan_request_check(c->request);
        if (cancelled) {
            snprintf(err, err_size, "%s", cancelled);
            return false;
        }
        if (c->session != s_session || !s_up) {
            snprintf(err, err_size, "the Connector socket closed during the transfer");
            return false;
        }
        const int fd = s_conn_fd;
        struct sockaddr_in peer;
        if (fd < 0 || !fd_peer(fd, &peer) || !same_peer(&peer, &s_conn_peer)) {
            c->gated = false;
            return true;  // can't tell: the write below blocks, up to the same limit
        }
        fd_set writable;
        FD_ZERO(&writable);
        FD_SET(fd, &writable);
        struct timeval tv = {.tv_sec = 0, .tv_usec = 100 * 1000};
        const int n = select(fd + 1, NULL, &writable, NULL, &tv);
        if (n > 0) {
            return true;
        }
        if (n < 0) {
            c->gated = false;
            return true;
        }
        if (esp_timer_get_time() - since >= CHUNK_STALL_LIMIT_MS * 1000LL) {
            ESP_LOGW(TAG, "chunk: no room on the Connector socket for %d s, giving up", CHUNK_STALL_LIMIT_MS / 1000);
            snprintf(err, err_size, "the device's link to the server made no progress for %d s",
                     CHUNK_STALL_LIMIT_MS / 1000);
            return false;
        }
    }
}

// Writes one frame on socket `session` from a task other than the Connector's (after its gate).
static bool send_on_session(unsigned session, const char *frame, size_t len)
{
    bool ok = false;
    if (xSemaphoreTake(s_client_mutex, pdMS_TO_TICKS(CHUNK_STALL_LIMIT_MS)) == pdTRUE) {
        ok = s_up && session == s_session && s_client && esp_websocket_client_is_connected(s_client) &&
             esp_websocket_client_send_text(s_client, frame, (int)len, pdMS_TO_TICKS(CHUNK_STALL_LIMIT_MS)) >= 0;
        xSemaphoreGive(s_client_mutex);
    }
    return ok;
}

static bool chunk_send(void *arg, const char *frame, size_t len, size_t raw_bytes, char *err, size_t err_size)
{
    chunk_ctx_t *c = arg;
    (void)raw_bytes;
    if (!operations_allowed()) {
        snprintf(err, err_size, "%s", READ_ONLY_REFUSAL("http_request"));
        return false;
    }
    if (comma_voice_active()) {
        snprintf(err, err_size, "%s", CALL_BUSY_MESSAGE);
        return false;
    }
    const int64_t start = esp_timer_get_time();
    if (!chunk_wait_room(c, err, err_size)) {
        return false;
    }
    if (!send_on_session(c->session, frame, len)) {
        snprintf(err, err_size, "sending to the server failed during the transfer");
        return false;
    }
    const int64_t took = esp_timer_get_time() - start;
    if (took > c->send_max_us) {
        c->send_max_us = took;
    }
    c->frames++;
    c->wire_bytes += len;
    const size_t internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (internal < c->internal_min) {
        c->internal_min = internal;
    }
    return true;
}

// ---- TCP tunnels (lan_tcp_dial) ---------------------------------------------------------------------

// Tunnel frames are at most LAN_TUNNEL_MAX_FRAME (8 KB) raw, so they pass the same writable gate as
// chunks and never block holding the client's TX lock.
#if CONFIG_COMMA_LAN_TUNNELS > 0
static bool tunnel_send(void *arg, unsigned session, const char *frame, size_t len)
{
    (void)arg;
    chunk_ctx_t c = {.session = session, .gated = true};
    char err[96];
    if (!chunk_wait_room(&c, err, sizeof(err))) {
        ESP_LOGW(TAG, "tunnel frame not sent: %s", err);
        return false;
    }
    return send_on_session(session, frame, len);
}

static lan_tunnel_verdict_t tunnel_check(void *arg, unsigned session, const char **reason)
{
    (void)arg;
    if (session != s_session || !s_up) {
        return LAN_TUNNEL_END_SILENT;
    }
    if (!operations_allowed()) {
        *reason = READ_ONLY_REFUSAL("lan_tcp_dial");
        return LAN_TUNNEL_END;
    }
    // Like chunk mode: no LAN transfer alongside a call. Checked again on every pass of the
    // tunnel loop, so a call that starts ends any open tunnel within ~20 ms.
    if (comma_voice_active()) {
        *reason = CALL_BUSY_MESSAGE;
        return LAN_TUNNEL_END;
    }
    return LAN_TUNNEL_GO;
}

static bool s_tunnel_ready;

// Same gates as the other LAN methods, answered right here; the tunnel task checks them again.
static void handle_tcp_dial(const char *id, const cJSON *params)
{
    if (!operations_allowed()) {
        ESP_LOGW(TAG, "%s refused: read-only", id);
        send_error(id, READ_ONLY_REFUSAL("lan_tcp_dial"));
    } else if (!s_tunnel_ready) {
        send_error(id, "lan_tcp_dial is unavailable: the device could not start its tunnel task");
    } else if (strlen(id) > ID_MAX) {
        send_error(id, "request id too long for this device");
    } else if (comma_voice_active()) {
        ESP_LOGW(TAG, "%s refused: %s", id, CALL_BUSY_MESSAGE);
        send_error(id, CALL_BUSY_MESSAGE);
    } else if (!lan_tunnel_dial(id, params, s_session)) {
        send_error(id, BUSY_MESSAGE);
    }
}

#endif

// The socket the job came in on is still open, with the credentials it was dialed with.
static bool job_current(const lan_job_t *job)
{
    return s_up && job->session == s_session && job->generation == device_config_generation();
}

// Rechecked by each LAN implementation before network I/O (including after DNS).
static const char *job_cancelled(void *arg)
{
    const lan_job_t *job = arg;
    if (!job_current(job)) {
        return "the Connector socket or credentials changed before the request completed";
    }
    if (!device_config_operations_allowed(job->generation)) {
        return "device operations are not allowed";
    }
    return NULL;
}

static char *run_http_request(const lan_job_t *job, const lan_request_t *request)
{
    chunk_ctx_t ctx = {
        .session = job->session,
        .request = request,
        .started_us = esp_timer_get_time(),
        .internal_min = heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        .gated = s_conn_fd >= 0,
    };
    const size_t internal_before = ctx.internal_min;
    const lan_http_sink_t sink = {
        .refuse = chunk_refuse, .frame_bytes = chunk_frame_bytes, .send = chunk_send, .ctx = &ctx};
    char *frame = lan_http_run_request(job->id, job->params, &sink, request);
    if (ctx.frames > 0) {
        ESP_LOGI(TAG, "%s chunks: %u frames, %u B on the wire, %lld ms, send_max_ms=%lld, gated=%s, "
                      "internal before %u min %u (lowest since boot %u)",
                 job->id, ctx.frames, (unsigned)ctx.wire_bytes,
                 (long long)((esp_timer_get_time() - ctx.started_us) / 1000), (long long)(ctx.send_max_us / 1000),
                 ctx.gated ? "yes" : "no", (unsigned)internal_before,
                 (unsigned)ctx.internal_min, (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    }
    return frame;
}

static void lan_worker(void *arg)
{
    (void)arg;
    for (;;) {
        lan_job_t job;
        xQueueReceive(s_lan_jobs, &job, portMAX_DELAY);
        if (!job_current(&job)) {
            // The server has already failed the requests of a closed socket: nothing to answer.
            ESP_LOGI(TAG, "%s skipped: its socket closed", job.id);
            cJSON_Delete(job.params);
            continue;
        }
        comma_heap_log("lan_begin");
        const lan_request_t request = {.deadline_us = job.deadline_us, .cancelled = job_cancelled, .ctx = &job};
        const char *cancelled = lan_request_check(&request);
        char *frame;
        if (cancelled) {
            // Access and the deadline may have changed while this waited in line.
            frame = json_error_frame(job.id, cancelled);
#if CONFIG_COMMA_LAN_DISCOVER
        } else if (job.method == LAN_DISCOVER) {
            frame = lan_discover_run_request(job.id, job.params, &request);
#endif
        } else if (job.method == LAN_UDP_REQUEST) {
            frame = lan_udp_run_request(job.id, job.params, &request);
        } else {
            frame = run_http_request(&job, &request);
        }
        comma_heap_log("lan_result_ready");
        cJSON_Delete(job.params);
        if (frame) {
            post_session(EV_REPLY, frame, job.session);
        } else {
            char error[OOM_FRAME_MAX];
            const size_t len = json_error_frame_into(error, sizeof(error), job.id, OOM_MESSAGE);
            if (len) {
                send_on_session(job.session, error, len);
            }
        }
    }
}

static void handle_request(const char *id, const char *method, cJSON *root, int64_t received_us)
{
    ESP_LOGI(TAG, "request %s %s", id, method);
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
    if (strcmp(method, "device_access") == 0) {
        handle_device_access(id, params);
    } else if (strcmp(method, "http_request") == 0) {
        handle_lan_request(id, LAN_HTTP_REQUEST, root, received_us);
    } else if (strcmp(method, "udp_request") == 0) {
        handle_lan_request(id, LAN_UDP_REQUEST, root, received_us);
#if CONFIG_COMMA_LAN_DISCOVER
    } else if (strcmp(method, "lan_discover") == 0) {
        handle_lan_request(id, LAN_DISCOVER, root, received_us);
#endif
#if CONFIG_COMMA_LAN_TUNNELS > 0
    } else if (strcmp(method, "lan_tcp_dial") == 0) {
        handle_tcp_dial(id, params);
#endif
    } else {
        char message[128];
        snprintf(message, sizeof(message), "unknown method: %.100s", method);
        send_error(id, message);
    }
}

static void handle_json_oom(const char *json)
{
    ESP_LOGW(TAG, "out of memory parsing frame");
    comma_heap_log("json_parse_oom");
    char *id = find_request_id(json, strlen(json));
    if (id) { send_oom(id); free(id); }
    else { post(EV_CLOSED, NULL); }
}

static void handle_text(const char *json, int64_t received_us)
{
    cJSON *root = connector_json_parse(json, handle_json_oom);
    if (!root) {
        return;
    }
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    const char *kind = cJSON_IsString(type) ? type->valuestring : "";
    if (strcmp(kind, "heartbeat") == 0) {
        // Only the device's own heartbeat frames keep the socket alive server-side (45 s).
        if (send_json("{\"type\":\"heartbeat\"}")) {
            s_heartbeat_us = esp_timer_get_time();
        }
    } else if (strcmp(kind, "request") == 0) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
        const cJSON *method = cJSON_GetObjectItemCaseSensitive(root, "method");
        if (!cJSON_IsString(id) || id->valuestring[0] == '\0') {
            ESP_LOGW(TAG, "request without an id");
        } else if (!cJSON_IsString(method)) {
            send_error(id->valuestring, "method must be a string");
        } else {
            handle_request(id->valuestring, method->valuestring, root, received_us);
        }
    } else if (strcmp(kind, "stream") == 0) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
        if (!cJSON_IsString(id) || !lan_tunnel_frame(id->valuestring, root, s_session)) {
            ESP_LOGI(TAG, "ignored stream frame");
        }
    } else if (strcmp(kind, "connected") == 0) {
        const cJSON *run = cJSON_GetObjectItemCaseSensitive(root, "connector_run_id");
        const cJSON *device = cJSON_GetObjectItemCaseSensitive(root, "device_id");
        const cJSON *generation = cJSON_GetObjectItemCaseSensitive(root, "connection_generation");
        provisioning_set_connector_state("connector_up", cJSON_IsString(device) ? device->valuestring : NULL);
        ESP_LOGI(TAG, "connected run=%s device=%s generation=%d", cJSON_IsString(run) ? run->valuestring : "",
                 cJSON_IsString(device) ? device->valuestring : "",
                 cJSON_IsNumber(generation) ? generation->valueint : -1);
    } else {
        ESP_LOGI(TAG, "ignored frame type=%s", kind);
    }
    cJSON_Delete(root);
}

static void log_stats(void)
{
    int64_t now = esp_timer_get_time();
    if (now - s_stats_us < STATS_PERIOD_US) {
        return;
    }
    s_stats_us = now;
    comma_heap_log("connector_stats");
    char age[24] = "never";
    if (s_heartbeat_us != 0) {
        snprintf(age, sizeof(age), "%llds", (long long)((now - s_heartbeat_us) / 1000000));
    }
    ESP_LOGI(TAG, "stats connected=%s last_heartbeat=%s reconnects=%d heap_internal=%u (min %u, largest %u) psram=%u",
             s_up ? "yes" : "no", age, s_reconnects, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

// Sleep for ms while still logging stats.
static void wait_ms(uint32_t ms)
{
    int64_t until = esp_timer_get_time() + (int64_t)ms * 1000;
    while (esp_timer_get_time() < until) {
        log_stats();
        int64_t left_ms = (until - esp_timer_get_time()) / 1000;
        vTaskDelay(pdMS_TO_TICKS(left_ms > 1000 ? 1000 : (left_ms > 0 ? left_ms : 1)));
    }
}

static void drain_events(void)
{
    event_t ev;
    while (xQueueReceive(s_events, &ev, 0) == pdTRUE) {
        free(ev.data);
    }
}

// Percent-encode src (UTF-8 bytes) for a query value: RFC 3986 unreserved characters pass through.
// Returns false when it does not fit.
static bool url_encode(char *dst, size_t size, const char *src)
{
    static const char HEX[] = "0123456789ABCDEF";
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p; ++p) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            if (n + 1 >= size) {
                return false;
            }
            dst[n++] = (char)*p;
        } else {
            if (n + 3 >= size) {
                return false;
            }
            dst[n++] = '%';
            dst[n++] = HEX[*p >> 4];
            dst[n++] = HEX[*p & 0xF];
        }
    }
    dst[n] = '\0';
    return true;
}

static bool build_dial(const credentials_t *creds)
{
    char name[3 * sizeof(creds->name)];
    char alias[3 * sizeof(creds->alias)];
    if (!url_encode(name, sizeof(name), creds->name) || !url_encode(alias, sizeof(alias), creds->alias)) {
        return false;
    }
    strlcpy(s_name, creds->name, sizeof(s_name));
    // scope goes on every dial, or a reconnect leaves the device with no command environment.
    int n = snprintf(s_uri, sizeof(s_uri), "%s?name=%s&alias=%s&os=esp-idf&arch=xtensa&scope=%s", creds->url,
                     name, alias, current_scope());
    if (n <= 0 || (size_t)n >= sizeof(s_uri)) {
        return false;
    }
    n = snprintf(s_headers, sizeof(s_headers), "Authorization: Bearer %s\r\nX-Salix-Connector-Instance-ID: %s\r\n",
                 creds->token, s_instance_id);
    return n > 0 && (size_t)n < sizeof(s_headers);
}

// The port a dial to s_uri connects to.
static int dial_port(void)
{
    const char *sep = strstr(s_uri, "://");
    const char *host = sep ? sep + 3 : s_uri;
    const size_t host_len = strcspn(host, "/?#");
    const char *colon = memchr(host, ':', host_len);
    if (colon) {
        return atoi(colon + 1);
    }
    return strncmp(s_uri, "wss://", 6) == 0 ? 443 : 80;
}

// Before dialing: remember which lwIP sockets are connected, and to what.
static void conn_fd_snapshot(void)
{
    for (int i = 0; i < CONFIG_LWIP_MAX_SOCKETS; ++i) {
        fd_peer(LWIP_SOCKET_OFFSET + i, &s_fd_before[i]);
    }
}

// On open: the Connector socket is the one connection to the dial's port that is new since the
// snapshot. If the voice call dialed the same port in the same window it's ambiguous, and chunks go
// out ungated.
static void conn_fd_find(void)
{
    const int port = dial_port();
    int found = -1;
    int count = 0;
    struct sockaddr_in peer;
    for (int i = 0; i < CONFIG_LWIP_MAX_SOCKETS; ++i) {
        const int fd = LWIP_SOCKET_OFFSET + i;
        if (!fd_peer(fd, &peer) || ntohs(peer.sin_port) != port || same_peer(&peer, &s_fd_before[i])) {
            continue;
        }
        count++;
        found = fd;
        s_conn_peer = peer;
    }
    s_conn_fd = count == 1 ? found : -1;
    if (count == 1) {
        ESP_LOGI(TAG, "Connector socket: fd %d", found);
    } else {
        ESP_LOGW(TAG, "Connector socket not identified (%d new connections to port %d): chunks go out ungated", count,
                 port);
    }
}

// The Connector socket's TCP state, read in the tcpip thread (no core locking in this build).
typedef struct {
    struct tcpip_api_call_data call;
    struct netconn *conn;
    int state;
    uint32_t unacked;
    uint32_t nrtx;
    uint32_t rcv_wnd;
    int refused;
} conn_tcp_t;

static err_t conn_tcp_snap(struct tcpip_api_call_data *data)
{
    conn_tcp_t *t = (conn_tcp_t *)data;
    struct tcp_pcb *pcb = t->conn ? t->conn->pcb.tcp : NULL;
    if (!pcb) {
        return ERR_CONN;
    }
    t->state = pcb->state;
    t->unacked = pcb->snd_nxt - pcb->lastack;
    t->nrtx = pcb->nrtx;
    t->rcv_wnd = pcb->rcv_wnd;
    t->refused = pcb->refused_data != NULL;
    return ERR_OK;
}

// Where a silent Connector socket stalled: data waiting unread on the device (unread, refused) is
// a device bug; PONGs mean the server's WebSocket layer is alive but sends no frames; unacked bytes
// with retransmissions mean the path is gone. Logged once, when the 45 s read timeout fires.
static void log_silent_link(void)
{
    const int fd = s_conn_fd;
    int unread = -1;
    conn_tcp_t t = {0};
    struct lwip_sock *sock = fd >= 0 ? lwip_socket_dbg_get_socket(fd) : NULL;
    t.conn = sock ? sock->conn : NULL;
    if (fd >= 0) {
        ioctl(fd, FIONREAD, &unread);
    }
    if (!t.conn || tcpip_api_call(conn_tcp_snap, &t.call) != ERR_OK) {
        ESP_LOGW(TAG, "silent link: pongs=%d fd=%d unread=%d tcp=?", (int)s_pongs, fd, unread);
        return;
    }
    ESP_LOGW(TAG, "silent link: pongs=%d fd=%d unread=%d tcp state=%d unacked=%u nrtx=%u rcv_wnd=%u refused=%d",
             (int)s_pongs, fd, unread, t.state, (unsigned)t.unacked, (unsigned)t.nrtx, (unsigned)t.rcv_wnd, t.refused);
}

// One connection, from dial to close. Returns how long the socket was open, in microseconds.
static int64_t run_session(const credentials_t *creds)
{
    if (credentials_stale(creds)) return 0;
    s_credentials_generation = creds->generation;
    if (!build_dial(creds)) {
        ESP_LOGE(TAG, "connect URL or token too long");
        return 0;
    }
    s_session++;
    s_close_code = 0;
    s_handshake_status = 0;
    ws_frame_reset(&s_frame);
    drain_events();

    comma_heap_log("connector_before_dial");
    esp_websocket_client_config_t cfg = {
        .uri = s_uri,
        .headers = s_headers,
        .transport = WEBSOCKET_TRANSPORT_OVER_SSL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_reconnect = true,
        .buffer_size = CONFIG_COMMA_CONNECTOR_WS_BUFFER_SIZE,
        .task_stack = 6144,
        .task_prio = CONNECTOR_WS_PRIO,
        .network_timeout_ms = 10000,
        .ping_interval_sec = 15,
        .pingpong_timeout_sec = 45,
    };
    if (strncmp(s_uri, "ws://", 5) == 0) {
        cfg.transport = WEBSOCKET_TRANSPORT_OVER_TCP;
        cfg.crt_bundle_attach = NULL;
    }
#ifdef COMMA_CONNECT_TEST_CA
    extern const char test_ca[] asm("_binary_connector_test_ca_pem_start");
    cfg.cert_pem = test_ca;
    cfg.crt_bundle_attach = NULL;
#endif
    // The client mallocs two buffer_size buffers, which CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL would
    // put in internal RAM. Right after BLE provisioning that has ~12 KB left, too little for them
    // plus the task stack, so steer this one init to PSRAM. Connector traffic is light.
#if CONFIG_SPIRAM
    heap_caps_malloc_extmem_enable(0);
#endif
    esp_websocket_client_handle_t client = esp_websocket_client_init(&cfg);
#if CONFIG_SPIRAM
    heap_caps_malloc_extmem_enable(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL);
#endif
    xSemaphoreTake(s_client_mutex, portMAX_DELAY);
    s_client = client;
    xSemaphoreGive(s_client_mutex);
    if (!s_client) {
        ESP_LOGE(TAG, "websocket client init failed");
        return 0;
    }
    esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, on_ws, NULL);
    ESP_LOGI(TAG, "dialing %s", s_uri);
    provisioning_set_connector_state("connector_connecting", NULL);
    int64_t opened_us = 0;
    int64_t last_rx_us = esp_timer_get_time();
    s_conn_fd = -1;
    conn_fd_snapshot();
    if (esp_websocket_client_start(s_client) != ESP_OK) {
        ESP_LOGE(TAG, "websocket client start failed (heap_internal=%u, largest %u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    } else {
        for (;;) {
            log_stats();
            event_t ev;
            if (credentials_stale(creds)) {
                ESP_LOGI(TAG, "credentials changed, reconnecting");
                break;
            }
            if (xQueueReceive(s_events, &ev, pdMS_TO_TICKS(1000)) != pdTRUE) {
                if (opened_us != 0 && esp_timer_get_time() - last_rx_us > READ_TIMEOUT_US) {
                    ESP_LOGW(TAG, "no frame for 45 s, reconnecting");
                    log_silent_link();
                    break;
                }
                continue;
            }
            if (ev.kind == EV_CLOSED || credentials_stale(creds) || !s_up) {
                free(ev.data);
                break;
            }
            if (ev.session != s_session) {
                free(ev.data);
                continue;
            }
            if (ev.kind != EV_REPLY) last_rx_us = ev.received_us;
            if (ev.kind == EV_OPEN) {
                opened_us = last_rx_us;
                s_heartbeat_us = 0;
                s_pongs = 0;
                comma_heap_log("connector_open");
                ESP_LOGI(TAG, "socket open");
                conn_fd_find();
                send_metadata();
            } else if (ev.kind == EV_TEXT) {
                handle_text(ev.data, ev.received_us);
            } else if (ev.kind == EV_REPLY) {
                if (ev.session == s_session) {
                    send_json(ev.data);
                }
            } else if (ev.kind == EV_NO_MEMORY) {
                if (!ev.data) break; // Closing the session fails its pending requests server-side.
                send_oom(ev.data);
            } else if (ev.kind == EV_TOO_BIG) {
                if (ev.data) {
                    send_error(ev.data, "request too large for this device");
                }
            }
            free(ev.data);
            if (ev.kind == EV_REPLY) comma_heap_log("lan_reply_freed");
        }
    }
    s_up = false;
    ++s_session;  // invalidate queued work before reconnect backoff
    s_conn_fd = -1;
    // Waits for a chunk send in progress (bounded by its own timeout) to finish with the client.
    xSemaphoreTake(s_client_mutex, portMAX_DELAY);
    s_client = NULL;
    xSemaphoreGive(s_client_mutex);
    esp_websocket_client_stop(client);
    esp_websocket_client_destroy(client);
    drain_events();
    ESP_LOGW(TAG, "closed code=%d handshake=%d after %llds", (int)s_close_code, (int)s_handshake_status,
             (long long)(opened_us ? (esp_timer_get_time() - opened_us) / 1000000 : 0));
    return opened_us ? esp_timer_get_time() - opened_us : 0;
}

// Wait (logging stats) until new credentials are saved.
static void wait_for_new_credentials(const credentials_t *creds)
{
    while (!credentials_stale(creds)) {
        wait_ms(1000);
    }
}

static void connector_task(void *arg)
{
    (void)arg;
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY); // startup publishes all worker readiness first
    credentials_t creds;
    make_instance_id();
    ESP_LOGI(TAG, "access: %s, instance %s", operations_allowed() ? "operations allowed" : "read-only", s_instance_id);
    uint32_t backoff_ms = BACKOFF_MIN_MS;
    bool first = true;
    for (;;) {
        if (!connector_credentials(&creds)) {
            ESP_LOGW(TAG, "no connect_url / connector token provisioned, Connector waits for setup");
            wait_for_new_credentials(&creds);
            continue;
        }
        if (!wifi_is_up()) {
            ESP_LOGI(TAG, "waiting for Wi-Fi");
            for (;;) {
                const esp_err_t err = wifi_wait(1000);
                if (err == ESP_OK) {
                    break;
                }
                if (err != ESP_ERR_TIMEOUT) {
                    vTaskDelay(pdMS_TO_TICKS(1000));  // no network configured right now (pairing mode)
                }
                log_stats();
            }
        }
        if (first) {
            ESP_LOGI(TAG, "before first dial: heap_internal=%u (largest %u) psram=%u",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            first = false;
        } else {
            s_reconnects++;
        }
        int64_t lasted_us = run_session(&creds);
        const int http = s_handshake_status;
        net_watchdog_dial_result(lasted_us > 0, http);
        if (credentials_stale(&creds)) {
            backoff_ms = BACKOFF_MIN_MS;
            continue;  // dial again right away with the new token
        }
        if (http == 401 || http == 403) {
            // The token was revoked. Retrying would not help; wait for re-provisioning (pairing mode).
            ESP_LOGE(TAG, "handshake HTTP %d: token rejected, Connector stopped until new credentials", http);
            provisioning_set_connector_state("connector_failed", NULL);
            wait_for_new_credentials(&creds);
            backoff_ms = BACKOFF_MIN_MS;
            continue;
        }
        provisioning_set_connector_state("connector_connecting", NULL);
        if (lasted_us >= STABLE_US) {
            backoff_ms = BACKOFF_MIN_MS;
        }
        // Full jitter over the upper half, so a fleet that lost the backend together spreads out.
        uint32_t delay_ms = backoff_ms / 2 + esp_random() % (backoff_ms / 2 + 1);
        ESP_LOGI(TAG, "reconnecting in %u ms", (unsigned)delay_ms);
        wait_ms(delay_ms);
        backoff_ms = backoff_ms * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : backoff_ms * 2;
    }
}

void connector_start(void)
{
    static bool started;
    static int64_t retry_after;
    if (started || esp_timer_get_time() < retry_after) {
        return;
    }
    retry_after = esp_timer_get_time() + 5000000;
    TaskHandle_t worker = NULL, connector = NULL;
    s_events = xQueueCreate(16, sizeof(event_t));
    s_lan_jobs = xQueueCreate(LAN_QUEUE_LEN, sizeof(lan_job_t));
    s_client_mutex = xSemaphoreCreateMutex();
    s_frame.buf = heap_caps_malloc(RX_MAX + 1, COMMA_BUFFER_CAPS);
    if (!s_events || !s_lan_jobs || !s_client_mutex || !s_frame.buf) {
        ESP_LOGE(TAG, "out of memory, Connector retries in 5 s");
        goto fail;
    }
    // The worker never writes flash, so its stack can be in PSRAM.
    s_lan_ready = xTaskCreateWithCaps(lan_worker, "lan", LAN_WORKER_STACK, NULL, LAN_WORKER_PRIO, &worker,
                                      COMMA_BUFFER_CAPS) == pdPASS;
    if (!s_lan_ready) {
        ESP_LOGE(TAG, "LAN worker create failed, http_request, udp_request and lan_discover off");
    }
#ifdef COMMA_CONNECT_URL_OVERRIDE
    ESP_LOGW(TAG, "dev build: Connector dials %s with a dummy token, not the saved credentials",
             COMMA_CONNECT_URL_OVERRIDE);
#endif
    if (xTaskCreate(connector_task, "connector", 6144, NULL, CONNECTOR_TASK_PRIO, &connector) != pdPASS) {
        ESP_LOGE(TAG, "task create failed, Connector retries in 5 s");
        if (worker) vTaskDeleteWithCaps(worker);
        s_lan_ready = false;
        goto fail;
    }
    started = true;
#if CONFIG_COMMA_LAN_TUNNELS > 0
    const lan_tunnel_host_t tunnel_host = {.send = tunnel_send, .check = tunnel_check};
    s_tunnel_ready = lan_tunnel_start(&tunnel_host);
#endif
    xTaskNotifyGive(connector);
    return;
fail:
    if (s_events) vQueueDelete(s_events);
    if (s_lan_jobs) vQueueDelete(s_lan_jobs);
    if (s_client_mutex) vSemaphoreDelete(s_client_mutex);
    free(s_frame.buf);
    s_events = NULL;
    s_lan_jobs = NULL;
    s_client_mutex = NULL;
    s_frame.buf = NULL;
}

bool connector_online(void)
{
    return s_up;
}
