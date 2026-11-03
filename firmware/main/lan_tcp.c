// SPDX-License-Identifier: Apache-2.0
// tcp_request for LAN scripts: one connection, bytes out, bytes back. See lan_tcp.h.

#include "lan_tcp.h"

#include "b64.h"
#include "json_buf.h"
#include "lan_dest.h"
#if CONFIG_COMMA_LAN_SCRIPT_FILE_WINDOW > 0
#include "lan_file.h"
#endif
#include "memory_caps.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lwip/sockets.h"

#define HOST_MAX 253
// Each wait is cut to this, so a cancelled request (access off, socket gone) stops soon.
#define POLL_US 100000

static const char *TAG = "lan_tcp";

static bool int_param(const cJSON *params, const char *key, int dflt, int lo, int hi, int *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(params, key);
    if (!v || cJSON_IsNull(v)) {
        *out = dflt;
        return true;
    }
    if (!cJSON_IsNumber(v) || v->valuedouble != (double)(long long)v->valuedouble) return false;
    const double d = v->valuedouble;
    *out = d < lo ? lo : (d > hi ? hi : (int)d);
    return true;
}

// Waits until s is readable (or writable) or deadline_us passes. 1 ready, 0 not yet, -1 error;
// *why gets a cancel reason.
static int wait_fd(int s, bool write, int64_t deadline_us, const lan_request_t *request, const char **why)
{
    *why = lan_request_check(request);
    if (*why) return -1;
    const int64_t left = deadline_us - esp_timer_get_time();
    if (left <= 0) return 0;
    const int64_t wait = left > POLL_US ? POLL_US : left;
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(s, &fds);
    struct timeval tv = {.tv_sec = (long)(wait / 1000000), .tv_usec = (long)(wait % 1000000)};
    const int r = select(s + 1, write ? NULL : &fds, write ? &fds : NULL, NULL, &tv);
    return r < 0 ? -1 : r > 0 ? 1 : 2;  // 2: this slice passed, look again
}

// The whole of buf to s, waiting while the socket is full. False with err set when it cannot.
static bool send_all(int s, const uint8_t *buf, size_t len, int64_t deadline_us, const lan_request_t *request,
                     const char *dest, char *err, size_t err_size)
{
    const char *why;
    for (size_t sent = 0; sent < len;) {
        const int r = wait_fd(s, true, deadline_us, request, &why);
        if (r == 0 || r < 0) {
            snprintf(err, err_size, "%s", why ? why : r == 0 ? "sending timed out" : "sending failed");
            return false;
        }
        if (r != 1) continue;
        const int n = send(s, buf + sent, len - sent, 0);
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            snprintf(err, err_size, "sending to %s failed (errno %d)", dest, errno);
            return false;
        }
        if (n > 0) sent += (size_t)n;
    }
    return true;
}

char *lan_tcp_run_request(const char *id, const cJSON *params, const lan_request_t *request,
                          const lan_tcp_file_t *file)
{
    const char *why = lan_request_check(request);
    if (why) return json_error_frame(id, why);
    char err[LAN_DEST_ERR_MAX] = "";
    if (!cJSON_IsObject(params)) return json_error_frame(id, "params must be an object with host and port");
    const cJSON *host = cJSON_GetObjectItemCaseSensitive(params, "host");
    if (!cJSON_IsString(host) || !host->valuestring[0] || strlen(host->valuestring) > HOST_MAX)
        return json_error_frame(id, "'host' is required");
    const cJSON *port = cJSON_GetObjectItemCaseSensitive(params, "port");
    if (!cJSON_IsNumber(port) || port->valuedouble != (double)(int)port->valuedouble || port->valueint < 1 ||
        port->valueint > 65535)
        return json_error_frame(id, "'port' must be a whole number between 1 and 65535");
    int timeout_ms, idle_ms, max_bytes;
    if (!int_param(params, "timeout_ms", LAN_TCP_TIMEOUT_DEFAULT_MS, 100, LAN_TCP_TIMEOUT_MAX_MS, &timeout_ms) ||
        !int_param(params, "idle_ms", LAN_TCP_IDLE_DEFAULT_MS, 0, LAN_TCP_TIMEOUT_MAX_MS, &idle_ms) ||
        !int_param(params, "max_bytes", LAN_TCP_MAX_BYTES, 0, LAN_TCP_MAX_BYTES, &max_bytes))
        return json_error_frame(id, "'timeout_ms', 'idle_ms' and 'max_bytes' must be whole numbers");
    const cJSON *close_write = cJSON_GetObjectItemCaseSensitive(params, "close_write");
    const bool send_file = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(params, "send_file"));
#if CONFIG_COMMA_LAN_SCRIPT_FILE_WINDOW > 0
    if (send_file && !file) return json_error_frame(id, "send_file needs the request's file (device.lan_script stream_file)");
#else
    if (send_file) return json_error_frame(id, "this board cannot stream a file to a script");
#endif
    const cJSON *send_b64 = cJSON_GetObjectItemCaseSensitive(params, "send_b64");
    if (send_b64 && !cJSON_IsNull(send_b64) && !cJSON_IsString(send_b64))
        return json_error_frame(id, "'send_b64' must be standard base64");
    const char *text = cJSON_IsString(send_b64) ? send_b64->valuestring : "";
    const size_t text_len = strlen(text);
    uint8_t *out = heap_caps_malloc(text_len / 4 * 3 + 1, COMMA_BUFFER_CAPS);
    uint8_t *in = heap_caps_malloc((size_t)max_bytes + 1, COMMA_BUFFER_CAPS);
    if (!out || !in) {
        free(out);
        free(in);
        return json_error_frame(id, "out of memory");
    }
    const long out_len = b64_decode(text, text_len, out, text_len / 4 * 3 + 1);
    if (out_len < 0) {
        free(out);
        free(in);
        return json_error_frame(id, "'send_b64' is not valid standard base64 (with padding, no whitespace)");
    }
    uint32_t ip;
    if (!lan_dest_resolve(host->valuestring, &ip, err) || !lan_dest_check(ip, err)) {
        free(out);
        free(in);
        return json_error_frame(id, err);
    }
    const uint8_t *b = (const uint8_t *)&ip;
    char dest[24];
    snprintf(dest, sizeof(dest), "%u.%u.%u.%u:%d", b[0], b[1], b[2], b[3], port->valueint);

    size_t file_bytes = 0;
    const int64_t start_us = esp_timer_get_time();
    const int64_t deadline_us = lan_request_deadline(request, start_us + (int64_t)timeout_ms * 1000);
    size_t got = 0;
    bool closed = false, truncated = false, timed_out = false;
    const int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) {
        free(out);
        free(in);
        return json_error_frame(id, "the device has no free socket right now; try again shortly");
    }
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port->valueint)};
    to.sin_addr.s_addr = ip;
    if (connect(s, (struct sockaddr *)&to, sizeof(to)) < 0 && errno != EINPROGRESS) {
        snprintf(err, sizeof(err), "connect to %s failed (errno %d)", dest, errno);
        goto done;
    }
    for (;;) {
        const int r = wait_fd(s, true, deadline_us, request, &why);
        if (r == 1) break;
        if (r == 0) {
            snprintf(err, sizeof(err), "connect to %s timed out", dest);
            goto done;
        }
        if (r < 0) {
            snprintf(err, sizeof(err), "%s", why ? why : "waiting to connect failed");
            goto done;
        }
    }
    int so_err = 0;
    socklen_t so_len = sizeof(so_err);
    getsockopt(s, SOL_SOCKET, SO_ERROR, &so_err, &so_len);
    if (so_err) {
        snprintf(err, sizeof(err), so_err == ECONNREFUSED ? "connect to %s refused: nothing is listening on that port"
                                                          : "connect to %s failed",
                 dest);
        goto done;
    }
    if (!send_all(s, out, (size_t)out_len, deadline_us, request, dest, err, sizeof(err))) goto done;
#if CONFIG_COMMA_LAN_SCRIPT_FILE_WINDOW > 0
    if (send_file) {
        if (!lan_file_open(file->id, file->session)) {
            snprintf(err, sizeof(err), "no memory to receive the file");
            goto done;
        }
        size_t written = 0;
        for (;;) {
            const long n = lan_file_read(in, max_bytes > 0 ? (size_t)max_bytes : 1, request, err, sizeof(err));
            if (n == 0) break;
            if (n < 0 || !send_all(s, in, (size_t)n, deadline_us, request, dest, err, sizeof(err))) {
                lan_file_close();
                goto done;
            }
            written += (size_t)n;
            lan_file_ack(written);
        }
        lan_file_close();
        file_bytes = written;
    }
#endif
    if (cJSON_IsTrue(close_write)) shutdown(s, SHUT_WR);
    int64_t last_us = esp_timer_get_time();
    while (got < (size_t)max_bytes) {
        const int64_t idle_deadline = idle_ms > 0 ? last_us + (int64_t)idle_ms * 1000 : deadline_us;
        const int r = wait_fd(s, false, idle_deadline < deadline_us ? idle_deadline : deadline_us, request, &why);
        if (r < 0) {
            snprintf(err, sizeof(err), "%s", why ? why : "reading failed");
            goto done;
        }
        if (r == 0) {
            timed_out = esp_timer_get_time() >= deadline_us;
            break;  // idle: the host said what it had to say
        }
        if (r != 1) continue;
        const int n = recv(s, in + got, (size_t)max_bytes - got, 0);
        if (n == 0) {
            closed = true;
            break;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            if (errno == ECONNRESET) {
                closed = true;
                break;
            }
            snprintf(err, sizeof(err), "reading from %s failed (errno %d)", dest, errno);
            goto done;
        }
        got += (size_t)n;
        last_us = esp_timer_get_time();
    }
    if (got >= (size_t)max_bytes && !closed) {
        uint8_t probe;
        truncated = recv(s, &probe, 1, MSG_DONTWAIT) > 0;
    }
done:
    close(s);
    free(out);
    ESP_LOGI(TAG, "%s %s: sent %ld B + %u B of file, got %u B%s%s%s, %lld ms", id, dest, out_len,
             (unsigned)file_bytes, (unsigned)got,
             closed ? ", closed" : "", truncated ? ", truncated" : "", timed_out ? ", timed out" : "",
             (long long)((esp_timer_get_time() - start_us) / 1000));
    if (err[0]) {
        free(in);
        return json_error_frame(id, err);
    }
    jbuf_t reply = {0};
    jbuf_str(&reply, "{\"id\":");
    jbuf_json_str(&reply, id);
    jbuf_str(&reply, ",\"type\":\"response\",\"result\":{\"data_b64\":\"");
    char *encoded = heap_caps_malloc(B64_ENCODED_LEN(got) + 1, COMMA_BUFFER_CAPS);
    const bool encoded_ok = encoded != NULL;
    if (encoded) jbuf_add(&reply, encoded, b64_encode(in, got, encoded));
    free(encoded);
    free(in);
    char tail[96];
    snprintf(tail, sizeof(tail), "\",\"closed\":%s,\"truncated\":%s,\"timed_out\":%s}}", closed ? "true" : "false",
             truncated ? "true" : "false", timed_out ? "true" : "false");
    jbuf_str(&reply, tail);
    if (!encoded_ok || reply.oom) {
        jbuf_free(&reply);
        return json_error_frame(id, "out of memory");
    }
    return reply.p;
}
