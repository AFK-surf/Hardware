#include "connector.h"
#include "net_watchdog.h"

#include "device_config.h"
#include "lan_discover.h"
#include "lan_http.h"
#include "provisioning.h"
#include "wifi.h"

#include <limits.h>
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
#include "freertos/task.h"
#include "nvs.h"

#define WS_OP_TEXT 0x1
#define WS_OP_CLOSE 0x8
#define WS_OP_PING 0x9
#define WS_OP_PONG 0xA

// Largest request frame: room for an http_request body as big as the response cap, plus the
// envelope. A larger text frame is dropped; if its id can be read from the first chunk, it still
// gets an error so the caller does not wait out its timeout. PSRAM.
#define RX_MAX (LAN_HTTP_MAX_BYTES + 8 * 1024)
#define ID_MAX 64
// The server heartbeats every 15 s. Like salix-connect, 45 s without any frame means the link is dead.
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
// LAN requests (http_request, lan_discover) run one at a time on their own task so the Connector
// task keeps answering heartbeats. A few more wait in line; past that the device answers "busy".
#define LAN_WORKER_PRIO 2
#define LAN_WORKER_STACK 8192
#define LAN_QUEUE_LEN 4

// Dev builds only (main/CMakeLists.txt): run lan_discover every N seconds through the LAN worker and
// its access gate, and log the reply, for testing on the board before the backend can ask for it.
#ifndef COMMA_LAN_DISCOVER_SELFTEST_S
#define COMMA_LAN_DISCOVER_SELFTEST_S 0
#endif
#define SELFTEST_SESSION UINT_MAX

#define SCOPE_READ_ONLY "local_file_read"
#define SCOPE_OPERATIONS ""
#define NVS_NAMESPACE "connector"
#define NVS_KEY_ALLOW "allow_ops"
#define FIRMWARE_VERSION "0.1.0"

static const char *TAG = "connector";

typedef enum {
    EV_OPEN,
    EV_TEXT,     // data: NUL-terminated frame, PSRAM, freed by the receiver
    EV_TOO_BIG,  // data: request id, or NULL when it could not be read
    EV_CLOSED,
    EV_REPLY,    // data: a whole reply frame from the LAN worker, PSRAM, for socket `session`
} event_kind_t;

typedef struct {
    event_kind_t kind;
    char *data;
    unsigned session;
} event_t;

typedef enum {
    LAN_HTTP_REQUEST,
    LAN_DISCOVER,
} lan_method_t;

typedef struct {
    char id[ID_MAX + 1];
    lan_method_t method;
    cJSON *params;     // detached from the request frame, freed by the worker
    unsigned session;  // the socket the request came in on
} lan_job_t;

typedef struct {
    char url[160];
    char token[160];
    unsigned generation;  // device_config_generation() these were read at
} credentials_t;

static QueueHandle_t s_events;
static esp_websocket_client_handle_t s_client;
static atomic_bool s_up;
static atomic_int s_close_code;
static atomic_int s_handshake_status;
static atomic_bool s_allow_operations;
static QueueHandle_t s_lan_jobs;
static bool s_lan_ready;
// Counts sockets. A reply only goes out on the socket its request came in on; the server has
// already failed requests from a closed one.
static atomic_uint s_session;
static char s_instance_id[37];
static char s_uri[384];
static char s_headers[512];

static uint8_t *s_rx;
static size_t s_frame_got;
static size_t s_frame_len;
static uint8_t s_opcode;
static bool s_assembling;

static int s_reconnects;
static int64_t s_heartbeat_us;
static int64_t s_stats_us;

// The one place credentials come from: provisioned over BLE into NVS (device_config), or secrets.h
// with COMMA_DEV_SECRETS on a dev board.
static bool connector_credentials(credentials_t *out)
{
    device_config_t *cfg = calloc(1, sizeof(*cfg));
    if (!cfg) {
        return false;
    }
    out->generation = device_config_generation();
    const bool provisioned = device_config_get(cfg);
    strlcpy(out->url, cfg->connect_url, sizeof(out->url));
    strlcpy(out->token, cfg->connector_token, sizeof(out->token));
    memset(cfg, 0, sizeof(*cfg));
    free(cfg);
    return provisioned && strncmp(out->url, "ws", 2) == 0 && strncmp(out->token, "salix_conn_", 11) == 0;
}

// New credentials were saved (pairing mode) since these were read.
static bool credentials_stale(const credentials_t *creds)
{
    return device_config_generation() != creds->generation;
}

static const char *current_scope(void)
{
    return s_allow_operations ? SCOPE_OPERATIONS : SCOPE_READ_ONLY;
}

// Read-only until the user allows operations in Settings, like salix-connect --device.
static void load_access(void)
{
    nvs_handle_t nvs;
    uint8_t allow = 0;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_u8(nvs, NVS_KEY_ALLOW, &allow);
        nvs_close(nvs);
    }
    s_allow_operations = allow != 0;
}

static esp_err_t save_access(bool allow)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(nvs, NVS_KEY_ALLOW, allow ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
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
    event_t ev = {.kind = kind, .data = data, .session = session};
    // The LAN worker can afford to wait; a dropped reply leaves the caller waiting out its timeout.
    const TickType_t wait = pdMS_TO_TICKS(kind == EV_REPLY ? 5000 : 100);
    if (xQueueSend(s_events, &ev, wait) != pdTRUE) {
        ESP_LOGW(TAG, "event queue full, dropped event %d", kind);
        free(data);
    }
}

static void post(event_kind_t kind, char *data)
{
    post_session(kind, data, 0);
}

static char *psram_strndup(const char *src, size_t len)
{
    char *out = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
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
            if (event->op_code == WS_OP_TEXT) {
                post(EV_TOO_BIG, find_request_id(event->data_ptr, chunk));
            }
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
    s_assembling = false;
    if (s_opcode == WS_OP_CLOSE) {
        s_close_code = s_frame_len >= 2 ? (s_rx[0] << 8) | s_rx[1] : 0;
    } else if (s_opcode == WS_OP_TEXT) {
        char *text = psram_strndup((const char *)s_rx, s_frame_len);
        if (text) {
            post(EV_TEXT, text);
        } else {
            ESP_LOGW(TAG, "out of memory for a %u-byte frame", (unsigned)s_frame_len);
        }
    }
    // The server ignores binary frames, so it never sends any.
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
    char json[768];
    int n = snprintf(json, sizeof(json),
                     "{\"type\":\"metadata\","
                     "\"capabilities\":{\"scope\":\"%s\",\"device_access\":true,\"execution_boundary\":\"lan-gateway\","
                     "\"persistent_processes\":false,\"computer_use_tool\":false,\"host_access_required\":false,"
                     "\"runtime_probe\":false,\"meeting_runtime\":false,"
                     // Not part of salix-connect's set: tells the backend (C2) what this gateway does.
                     "\"lan_gateway\":{\"methods\":[\"http_request\",\"lan_discover\"],\"max_bytes\":%d,"
                     "\"max_timeout_seconds\":%d},"
                     "\"component_versions\":{\"t-circle\":\"" FIRMWARE_VERSION "\"}},"
                     "\"skills\":[],"
                     "\"system_info\":{\"client_source\":\"t-circle\",\"hostname\":\"T-Circle\",\"os_type\":\"esp-idf\","
                     "\"arch\":\"xtensa\"%s}}",
                     current_scope(), LAN_HTTP_MAX_BYTES, LAN_HTTP_TIMEOUT_MAX_S, collected_at);
    if (n <= 0 || (size_t)n >= sizeof(json)) {
        ESP_LOGE(TAG, "metadata does not fit");
        return;
    }
    if (send_json(json)) {
        ESP_LOGI(TAG, "sent metadata scope=%s", s_allow_operations ? "\"\"" : SCOPE_READ_ONLY);
    }
}

static void send_error(const char *id, const char *message)
{
    cJSON *obj = cJSON_CreateObject();
    if (obj) {
        cJSON_AddStringToObject(obj, "id", id);
        cJSON_AddStringToObject(obj, "type", "error");
        cJSON_AddStringToObject(obj, "error", message);
    }
    send_object(obj);
}

static void handle_device_access(const char *id, const cJSON *params)
{
    const cJSON *allow = cJSON_IsObject(params) ? cJSON_GetObjectItemCaseSensitive(params, "allow_operations") : NULL;
    if (!cJSON_IsBool(allow)) {
        send_error(id, "allow_operations must be a boolean");
        return;
    }
    const bool value = cJSON_IsTrue(allow);
    esp_err_t err = save_access(value);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving access failed: %s", esp_err_to_name(err));
        send_error(id, "could not save device access");
        return;
    }
    if (value != s_allow_operations) {
        ESP_LOGI(TAG, "access: %s", value ? "operations allowed" : "read-only");
    }
    s_allow_operations = value;
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
    return method == LAN_DISCOVER ? "lan_discover" : "http_request";
}

// The read-only wording matches salix-connect, so the agent sees the same refusal.
static void send_read_only_refusal(const char *id, lan_method_t method)
{
    char message[96];
    snprintf(message, sizeof(message), "method \"%s\" is not permitted for a local_file_read connector",
             lan_method_name(method));
    send_error(id, message);
}

// The access gate, for every method that touches the LAN. The server does not check access on the
// device-placed MCP path, so this is the only thing between the cloud and the LAN while the user has
// not allowed operations.
static void handle_lan_request(const char *id, lan_method_t method, cJSON *root)
{
    if (!s_allow_operations) {
        ESP_LOGW(TAG, "%s refused: read-only", id);
        send_read_only_refusal(id, method);
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
    lan_job_t job = {.session = s_session, .method = method};
    strlcpy(job.id, id, sizeof(job.id));
    job.params = cJSON_DetachItemFromObjectCaseSensitive(root, "params");
    if (xQueueSend(s_lan_jobs, &job, 0) != pdTRUE) {
        cJSON_Delete(job.params);
        send_error(id, "the T-Circle is busy with other LAN requests; try again shortly");
    }
}

static void lan_worker(void *arg)
{
    (void)arg;
    for (;;) {
        lan_job_t job;
        xQueueReceive(s_lan_jobs, &job, portMAX_DELAY);
        char *frame;
        const bool selftest = job.session == SELFTEST_SESSION;
        if (!selftest && job.session != s_session) {
            ESP_LOGI(TAG, "%s skipped: its socket closed", job.id);
            frame = NULL;
        } else if (!s_allow_operations) {
            // Access was turned off while this one waited in line.
            char message[96];
            snprintf(message, sizeof(message), "method \"%s\" is not permitted for a local_file_read connector",
                     lan_method_name(job.method));
            cJSON *obj = cJSON_CreateObject();
            cJSON_AddStringToObject(obj, "id", job.id);
            cJSON_AddStringToObject(obj, "type", "error");
            cJSON_AddStringToObject(obj, "error", message);
            frame = cJSON_PrintUnformatted(obj);
            cJSON_Delete(obj);
        } else if (job.method == LAN_DISCOVER) {
            frame = lan_discover_run(job.id, job.params);
        } else {
            frame = lan_http_run(job.id, job.params);
        }
        cJSON_Delete(job.params);
        if (frame && selftest) {
            const size_t len = strlen(frame);
            for (size_t at = 0; at < len; at += 400) {
                ESP_LOGI(TAG, "%s [%u/%u] %.400s", job.id, (unsigned)(at / 400 + 1), (unsigned)((len + 399) / 400),
                         frame + at);
            }
            free(frame);
        } else if (frame) {
            post_session(EV_REPLY, frame, job.session);
        }
    }
}

#if COMMA_LAN_DISCOVER_SELFTEST_S > 0
static void selftest_task(void *arg)
{
    (void)arg;
    for (unsigned n = 1;; ++n) {
        vTaskDelay(pdMS_TO_TICKS(COMMA_LAN_DISCOVER_SELFTEST_S * 1000));
        if (!wifi_is_up()) {
            continue;
        }
        lan_job_t job = {.session = SELFTEST_SESSION, .method = LAN_DISCOVER};
        snprintf(job.id, sizeof(job.id), "selftest_%u", n);
        if (!s_allow_operations) {
            ESP_LOGW(TAG, "%s refused: read-only", job.id);
            continue;
        }
        if (xQueueSend(s_lan_jobs, &job, 0) != pdTRUE) {
            ESP_LOGW(TAG, "%s: LAN worker busy", job.id);
        }
    }
}
#endif

static void handle_request(const char *id, const char *method, cJSON *root)
{
    ESP_LOGI(TAG, "request %s %s", id, method);
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
    if (strcmp(method, "device_access") == 0) {
        handle_device_access(id, params);
    } else if (strcmp(method, "http_request") == 0) {
        handle_lan_request(id, LAN_HTTP_REQUEST, root);
    } else if (strcmp(method, "lan_discover") == 0) {
        handle_lan_request(id, LAN_DISCOVER, root);
    } else {
        char message[128];
        snprintf(message, sizeof(message), "unknown method: %.100s", method);
        send_error(id, message);
    }
}

static void handle_text(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGW(TAG, "ignored malformed text frame");
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
            handle_request(id->valuestring, method->valuestring, root);
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

static bool build_dial(const credentials_t *creds)
{
    // scope goes on every dial, or a reconnect leaves the device with no command environment.
    int n = snprintf(s_uri, sizeof(s_uri), "%s?name=T-Circle&alias=tcircle&os=esp-idf&arch=xtensa&scope=%s",
                     creds->url, current_scope());
    if (n <= 0 || (size_t)n >= sizeof(s_uri)) {
        return false;
    }
    n = snprintf(s_headers, sizeof(s_headers), "Authorization: Bearer %s\r\nX-Salix-Connector-Instance-ID: %s\r\n",
                 creds->token, s_instance_id);
    return n > 0 && (size_t)n < sizeof(s_headers);
}

// One connection, from dial to close. Returns how long the socket was open, in microseconds.
static int64_t run_session(const credentials_t *creds)
{
    if (!build_dial(creds)) {
        ESP_LOGE(TAG, "connect URL or token too long");
        return 0;
    }
    s_session++;
    s_close_code = 0;
    s_handshake_status = 0;
    s_assembling = false;
    drain_events();

    esp_websocket_client_config_t cfg = {
        .uri = s_uri,
        .headers = s_headers,
        .transport = WEBSOCKET_TRANSPORT_OVER_SSL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_reconnect = true,
        .buffer_size = 4096,
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
    // The client mallocs two buffer_size buffers, which CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL would
    // put in internal RAM. Right after BLE provisioning that has ~12 KB left, too little for them
    // plus the task stack, so steer this one init to PSRAM. Connector traffic is light.
    heap_caps_malloc_extmem_enable(0);
    s_client = esp_websocket_client_init(&cfg);
    heap_caps_malloc_extmem_enable(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL);
    if (!s_client) {
        ESP_LOGE(TAG, "websocket client init failed");
        return 0;
    }
    esp_websocket_register_events(s_client, WEBSOCKET_EVENT_ANY, on_ws, NULL);
    ESP_LOGI(TAG, "dialing %s", s_uri);
    provisioning_set_connector_state("connector_connecting", NULL);
    int64_t opened_us = 0;
    int64_t last_rx_us = esp_timer_get_time();
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
                    break;
                }
                continue;
            }
            if (ev.kind == EV_CLOSED) {
                break;
            }
            last_rx_us = esp_timer_get_time();
            if (ev.kind == EV_OPEN) {
                opened_us = last_rx_us;
                s_heartbeat_us = 0;
                ESP_LOGI(TAG, "socket open");
                send_metadata();
            } else if (ev.kind == EV_TEXT) {
                handle_text(ev.data);
            } else if (ev.kind == EV_REPLY) {
                if (ev.session == s_session) {
                    send_json(ev.data);
                }
            } else if (ev.kind == EV_TOO_BIG) {
                if (ev.data) {
                    send_error(ev.data, "request too large for this device");
                }
            }
            free(ev.data);
        }
    }
    s_up = false;
    esp_websocket_client_handle_t client = s_client;
    s_client = NULL;
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
    credentials_t creds;
    load_access();
    make_instance_id();
    ESP_LOGI(TAG, "access: %s, instance %s", s_allow_operations ? "operations allowed" : "read-only", s_instance_id);
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
    if (started) {
        return;
    }
    started = true;
    s_events = xQueueCreate(16, sizeof(event_t));
    s_lan_jobs = xQueueCreate(LAN_QUEUE_LEN, sizeof(lan_job_t));
    s_rx = heap_caps_malloc(RX_MAX + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_events || !s_lan_jobs || !s_rx) {
        ESP_LOGE(TAG, "out of memory, Connector off");
        return;
    }
    // The worker never writes flash, so its stack can be in PSRAM.
    s_lan_ready = xTaskCreateWithCaps(lan_worker, "lan", LAN_WORKER_STACK, NULL, LAN_WORKER_PRIO, NULL,
                                      MALLOC_CAP_SPIRAM) == pdPASS;
    if (!s_lan_ready) {
        ESP_LOGE(TAG, "LAN worker create failed, http_request and lan_discover off");
    }
#if COMMA_LAN_DISCOVER_SELFTEST_S > 0
    ESP_LOGW(TAG, "lan_discover self-test every %d s (dev build)", COMMA_LAN_DISCOVER_SELFTEST_S);
    if (s_lan_ready) {
        xTaskCreateWithCaps(selftest_task, "lan_selftest", 3072, NULL, LAN_WORKER_PRIO, NULL, MALLOC_CAP_SPIRAM);
    }
#endif
    if (xTaskCreate(connector_task, "connector", 6144, NULL, CONNECTOR_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "task create failed, Connector off");
    }
}

bool connector_is_up(void)
{
    return s_up;
}
