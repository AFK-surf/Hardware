// SPDX-License-Identifier: Apache-2.0
// lan_script for the Connector: the script runtime (components/lan_script) with this gateway's LAN
// methods as its host. See lan_script_job.h.

#include "lan_script_job.h"

#include "json_buf.h"
#include "lan_http.h"
#include "lan_script.h"
#include "lan_tcp.h"
#include "lan_udp.h"
#include "memory_caps.h"
#if CONFIG_COMMA_LAN_DISCOVER
#include "lan_discover.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "lan_script";

typedef struct {
    const lan_request_t *request;
    // The script's own deadline: the request's, or sooner by timeout_ms. Each LAN call ends by it too.
    lan_request_t bounded;
    unsigned calls;
    int64_t yielded_us;
} script_ctx_t;

// A script that only computes never blocks: the runtime asks `cancelled` every few thousand
// instructions, and it gives the CPU away a tick this often, so the idle task (and its watchdog)
// and lower tasks still run.
#define YIELD_EVERY_US (50 * 1000)

static int timeout_ms(const cJSON *params)
{
    const cJSON *v = cJSON_IsObject(params) ? cJSON_GetObjectItemCaseSensitive(params, "timeout_ms") : NULL;
    if (!cJSON_IsNumber(v)) return LAN_SCRIPT_TIMEOUT_DEFAULT_MS;
    const double d = v->valuedouble;
    return d < LAN_SCRIPT_TIMEOUT_MIN_MS ? LAN_SCRIPT_TIMEOUT_MIN_MS
           : d > LAN_SCRIPT_TIMEOUT_MAX_MS ? LAN_SCRIPT_TIMEOUT_MAX_MS
                                           : (int)d;
}

int lan_script_timeout_seconds(const cJSON *params) { return (timeout_ms(params) + 999) / 1000; }

// One LAN method, through the same function the Connector runs it with; its reply frame is unwrapped
// into the result, or the error message.
static bool call(void *arg, const char *method, const char *params_text, size_t params_len, char **out)
{
    script_ctx_t *c = arg;
    *out = NULL;
    cJSON *params = cJSON_ParseWithLength(params_text, params_len);
    if (!params || !cJSON_IsObject(params)) {
        cJSON_Delete(params);
        *out = strdup("params must be a JSON object");
        return false;
    }
    char *frame = NULL;
    c->calls++;
    if (strcmp(method, "http_request") == 0) {
        // Text mode only: a script holds the body in its heap.
        frame = cJSON_GetObjectItemCaseSensitive(params, "response_mode")
                    ? json_error_frame("script", "response_mode is not available to scripts")
                    : lan_http_run_request("script", params, NULL, &c->bounded);
    } else if (strcmp(method, "udp_request") == 0) {
        frame = lan_udp_run_request("script", params, &c->bounded);
    } else if (strcmp(method, "tcp_request") == 0) {
        frame = lan_tcp_run_request("script", params, &c->bounded);
#if CONFIG_COMMA_LAN_DISCOVER
    } else if (strcmp(method, "lan_discover") == 0) {
        frame = lan_discover_run_request("script", params, &c->bounded);
#endif
    } else {
        char message[96];
        snprintf(message, sizeof(message), "this gateway has no %.40s", method);
        frame = json_error_frame("script", message);
    }
    cJSON_Delete(params);
    if (!frame) return false;
    cJSON *reply = cJSON_Parse(frame);
    free(frame);
    const cJSON *result = reply ? cJSON_GetObjectItemCaseSensitive(reply, "result") : NULL;
    const cJSON *error = reply ? cJSON_GetObjectItemCaseSensitive(reply, "error") : NULL;
    bool ok = false;
    if (result) {
        *out = cJSON_PrintUnformatted(result);
        ok = *out != NULL;
    } else {
        *out = strdup(cJSON_IsString(error) ? error->valuestring : "the request failed");
    }
    cJSON_Delete(reply);
    return ok;
}

static int64_t wall_ms(void *arg)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static int64_t monotonic_ms(void *arg) { return esp_timer_get_time() / 1000; }

static void sleep_ms(void *arg, uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms ? ms : 1)); }

static const char *cancelled(void *arg)
{
    script_ctx_t *c = arg;
    const int64_t now = esp_timer_get_time();
    if (now - c->yielded_us >= YIELD_EVERY_US) {
        vTaskDelay(1);
        c->yielded_us = esp_timer_get_time();
    }
    return c->request && c->request->cancelled ? c->request->cancelled(c->request->ctx) : NULL;
}

char *lan_script_run_request(const char *id, const cJSON *params, const lan_request_t *request)
{
    const cJSON *source = cJSON_IsObject(params) ? cJSON_GetObjectItemCaseSensitive(params, "source") : NULL;
    if (!cJSON_IsString(source) || !source->valuestring[0]) return json_error_frame(id, "'source' is required");
    const size_t source_len = strlen(source->valuestring);
    if (source_len > LAN_SCRIPT_MAX_SOURCE) {
        char message[80];
        snprintf(message, sizeof(message), "'source' is longer than %d bytes", LAN_SCRIPT_MAX_SOURCE);
        return json_error_frame(id, message);
    }
    const cJSON *args = cJSON_GetObjectItemCaseSensitive(params, "args");
    char *args_text = args && !cJSON_IsNull(args) ? cJSON_PrintUnformatted(args) : NULL;
    if (args && !cJSON_IsNull(args) && !args_text) return NULL;

    const int64_t start_us = esp_timer_get_time();
    script_ctx_t c = {.request = request, .yielded_us = start_us};
    c.bounded.deadline_us = lan_request_deadline(request, start_us + (int64_t)timeout_ms(params) * 1000);
    c.bounded.cancelled = request ? request->cancelled : NULL;
    c.bounded.ctx = request ? request->ctx : NULL;

    void *heap = heap_caps_malloc(LAN_SCRIPT_HEAP_BYTES, COMMA_BUFFER_CAPS);
    if (!heap) {
        free(args_text);
        return json_error_frame(id, "not enough memory on this device to run a script");
    }
    const lan_script_host_t host = {call, wall_ms, monotonic_ms, sleep_ms, cancelled, &c};
    const lan_script_limits_t limits = {
        .heap = heap,
        .heap_bytes = LAN_SCRIPT_HEAP_BYTES,
        .deadline_ms = c.bounded.deadline_us / 1000,
        .max_result_bytes = LAN_SCRIPT_MAX_RESULT,
        .max_log_bytes = LAN_SCRIPT_MAX_LOGS,
    };
    char *outcome = lan_script_run(&host, &limits, source->valuestring, source_len, args_text,
                                   args_text ? strlen(args_text) : 0);
    free(heap);
    free(args_text);
    ESP_LOGI(TAG, "%s: %u B source, %u LAN call(s), %lld ms%s", id, (unsigned)source_len, c.calls,
             (long long)((esp_timer_get_time() - start_us) / 1000),
             outcome && strncmp(outcome, "{\"error\"", 8) == 0 ? ", script failed" : "");
    if (!outcome) return NULL;
    jbuf_t out = {0};
    jbuf_str(&out, "{\"id\":");
    jbuf_json_str(&out, id);
    jbuf_str(&out, ",\"type\":\"response\",\"result\":");
    jbuf_str(&out, outcome);
    jbuf_str(&out, "}");
    free(outcome);
    if (out.oom) {
        jbuf_free(&out);
        return NULL;
    }
    return out.p;
}
