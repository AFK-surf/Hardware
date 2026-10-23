// SPDX-License-Identifier: Apache-2.0
// udp_request for the Connector: one datagram out, answers in, as base64. See lan_udp.h.

#include "lan_udp.h"

#include "b64.h"
#include "json_buf.h"
#include "lan_dest.h"
#include "memory_caps.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/sockets.h"

#define HOST_MAX 253

static const char *TAG = "lan_udp";

// A whole number in [lo, hi] (a JSON number with no fraction). Missing or null -> dflt.
static bool int_param(const cJSON *params, const char *key, int dflt, int lo, int hi, int *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(params, key);
    if (!v || cJSON_IsNull(v)) {
        *out = dflt;
        return true;
    }
    if (!cJSON_IsNumber(v) || v->valuedouble != (double)(long long)v->valuedouble) {
        return false;
    }
    const double d = v->valuedouble;
    *out = d < lo ? lo : (d > hi ? hi : (int)d);
    return true;
}

char *lan_udp_run_request(const char *id, const cJSON *params, const lan_request_t *request)
{
    const char *cancelled = lan_request_check(request);
    if (cancelled) return json_error_frame(id, cancelled);
    char err[LAN_DEST_ERR_MAX] = "";
    if (!cJSON_IsObject(params)) {
        return json_error_frame(id, "params must be an object with host, port and payload_b64");
    }
    const cJSON *host = cJSON_GetObjectItemCaseSensitive(params, "host");
    if (!cJSON_IsString(host) || host->valuestring[0] == 0) {
        return json_error_frame(id, "'host' is required");
    }
    if (strlen(host->valuestring) > HOST_MAX) {
        return json_error_frame(id, "'host' is too long");
    }
    const cJSON *port = cJSON_GetObjectItemCaseSensitive(params, "port");
    if (!cJSON_IsNumber(port) || port->valuedouble != (double)(int)port->valuedouble || port->valueint < 1 ||
        port->valueint > 65535) {
        return json_error_frame(id, "'port' must be a whole number between 1 and 65535");
    }
    const cJSON *b64 = cJSON_GetObjectItemCaseSensitive(params, "payload_b64");
    if (!cJSON_IsString(b64)) {
        return json_error_frame(id, "'payload_b64' is required (standard base64)");
    }
    int timeout_ms;
    if (!int_param(params, "timeout_ms", LAN_UDP_TIMEOUT_DEFAULT_MS, LAN_UDP_TIMEOUT_MIN_MS, LAN_UDP_TIMEOUT_MAX_MS,
                   &timeout_ms)) {
        return json_error_frame(id, "'timeout_ms' must be a whole number of milliseconds");
    }
    int max_responses;
    if (!int_param(params, "max_responses", 1, 0, LAN_UDP_MAX_RESPONSES, &max_responses)) {
        return json_error_frame(id, "'max_responses' must be a whole number (0-16)");
    }
    uint8_t payload[LAN_UDP_MAX_PAYLOAD];
    const long len = b64_decode(b64->valuestring, strlen(b64->valuestring), payload, sizeof(payload));
    if (len == B64_TOO_LONG) {
        snprintf(err, sizeof(err), "the payload is longer than %d bytes, the most this device sends in one datagram",
                 LAN_UDP_MAX_PAYLOAD);
        return json_error_frame(id, err);
    }
    if (len < 0) {
        return json_error_frame(id, "'payload_b64' is not valid standard base64 (with padding, no whitespace)");
    }
    uint32_t ip;
    if (!lan_dest_resolve(host->valuestring, &ip, err) || !lan_dest_check(ip, err)) {
        return json_error_frame(id, err);
    }

    cancelled = lan_request_check(request);
    if (cancelled) return json_error_frame(id, cancelled);
    const int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) {
        return json_error_frame(id, "the device has no free socket right now; try again shortly");
    }
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port->valueint)};
    to.sin_addr.s_addr = ip;
    char dest[24];
    const uint8_t *b = (const uint8_t *)&ip;
    snprintf(dest, sizeof(dest), "%u.%u.%u.%u:%d", b[0], b[1], b[2], b[3], port->valueint);
    const int64_t start_us = esp_timer_get_time();
    cancelled = lan_request_check(request);
    if (cancelled) {
        close(s);
        return json_error_frame(id, cancelled);
    }
    if (sendto(s, payload, (size_t)len, 0, (struct sockaddr *)&to, sizeof(to)) < 0) {
        snprintf(err, sizeof(err), "sending to %s failed (errno %d)", dest, errno);
        close(s);
        return json_error_frame(id, err);
    }

    // The reply is built in PSRAM: through cJSON, 16 base64 answers would sit in internal RAM.
    jbuf_t out = {0};
    jbuf_str(&out, "{\"id\":");
    jbuf_json_str(&out, id);
    jbuf_str(&out, ",\"type\":\"response\",\"result\":{\"responses\":[");
    uint8_t *buf = heap_caps_malloc(LAN_UDP_MAX_ANSWER, COMMA_BUFFER_CAPS);
    char *text = heap_caps_malloc(B64_ENCODED_LEN(LAN_UDP_MAX_ANSWER), COMMA_BUFFER_CAPS);
    if (out.oom || !buf || !text) {
        jbuf_free(&out);
        free(buf);
        free(text);
        close(s);
        return json_error_frame(id, "out of memory");
    }
    const int64_t deadline_us = lan_request_deadline(request, start_us + (int64_t)timeout_ms * 1000);
    int got = 0;
    int ignored = 0;
    bool timed_out = false;
    while (got < max_responses) {
        cancelled = lan_request_check(request);
        if (cancelled) {
            snprintf(err, sizeof(err), "%s", cancelled);
            break;
        }
        const int64_t left_us = deadline_us - esp_timer_get_time();
        if (left_us <= 0) {
            timed_out = true;
            break;
        }
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(s, &fds);
        const int64_t wait_us = request && left_us > 100000 ? 100000 : left_us;
        struct timeval tv = {.tv_sec = (long)(wait_us / 1000000), .tv_usec = (long)(wait_us % 1000000)};
        const int ready = select(s + 1, &fds, NULL, NULL, &tv);
        if (ready < 0) {
            snprintf(err, sizeof(err), "waiting for answers from %s failed (errno %d)", dest, errno);
            break;
        }
        if (ready == 0) {
            continue;  // the deadline check above ends the loop
        }
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        const int n = recvfrom(s, buf, LAN_UDP_MAX_ANSWER, MSG_DONTWAIT, (struct sockaddr *)&from, &from_len);
        if (n < 0) {
            continue;
        }
        if (from.sin_addr.s_addr != ip) {
            ignored++;
            continue;  // not the host we asked
        }
        const uint8_t *fb = (const uint8_t *)&from.sin_addr.s_addr;
        char from_text[24];
        snprintf(from_text, sizeof(from_text), "%u.%u.%u.%u:%u", fb[0], fb[1], fb[2], fb[3], ntohs(from.sin_port));
        jbuf_str(&out, got ? ",{\"from\":\"" : "{\"from\":\"");
        jbuf_str(&out, from_text);
        jbuf_str(&out, "\",\"payload_b64\":\"");
        jbuf_add(&out, text, b64_encode(buf, (size_t)n, text));
        jbuf_str(&out, "\"}");
        got++;
    }
    close(s);
    free(buf);
    free(text);
    const int elapsed_ms = (int)((esp_timer_get_time() - start_us) / 1000);
    ESP_LOGI(TAG, "%s %s: sent %d B, %d answer(s)%s%s, %d ms", id, dest, len, got, timed_out ? ", timed out" : "",
             ignored ? " (others ignored)" : "", elapsed_ms);
    jbuf_str(&out, timed_out ? "],\"timed_out\":true}}" : "],\"timed_out\":false}}");
    if (err[0] || out.oom) {
        jbuf_free(&out);
        return json_error_frame(id, err[0] ? err : "out of memory");
    }
    return out.p;
}
