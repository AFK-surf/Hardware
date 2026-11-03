// SPDX-License-Identifier: Apache-2.0
// A LAN script's file, pulled from the server in stream frames. See lan_file.h.

#include "lan_file.h"

#include "b64.h"
#include "json_buf.h"
#include "memory_caps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"

#define ID_MAX 64
#define POLL_MS 100

static const char *TAG = "lan_file";

static lan_file_send_fn s_send;
static SemaphoreHandle_t s_lock;
// Guarded by s_lock: which transfer frames belong to, and how it stands.
static char s_id[ID_MAX + 1];
static unsigned s_session;
static bool s_open, s_eof, s_overrun;
static StreamBufferHandle_t s_buf;
static uint8_t *s_frame;  // one decoded frame, connector task only

void lan_file_init(lan_file_send_fn send)
{
    s_send = send;
    s_lock = xSemaphoreCreateMutex();
}

static void send_ack(size_t seq)
{
    char frame[ID_MAX * 6 + 96];
    jbuf_t b = {.p = frame, .cap = sizeof(frame), .limit = sizeof(frame) - 1};
    frame[0] = 0;
    jbuf_str(&b, "{\"id\":");
    jbuf_json_str(&b, s_id);
    char tail[64];
    snprintf(tail, sizeof(tail), ",\"type\":\"stream\",\"stream\":{\"channel\":\"file_ack\",\"seq\":%u}}", (unsigned)seq);
    jbuf_str(&b, tail);
    if (!b.full && s_send) s_send(s_session, frame, b.len);
}

bool lan_file_open(const char *id, unsigned session)
{
    if (!s_lock || strlen(id) > ID_MAX) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = !s_open;
    if (ok) {
        s_buf = xStreamBufferCreateWithCaps(LAN_FILE_WINDOW, 1, COMMA_BUFFER_CAPS);
        s_frame = heap_caps_malloc(LAN_FILE_FRAME, COMMA_BUFFER_CAPS);
        ok = s_buf && s_frame;
        if (ok) {
            strlcpy(s_id, id, sizeof(s_id));
            s_session = session;
            s_open = true;
            s_eof = s_overrun = false;
        } else {
            if (s_buf) vStreamBufferDeleteWithCaps(s_buf);
            free(s_frame);
            s_buf = NULL;
            s_frame = NULL;
        }
    }
    xSemaphoreGive(s_lock);
    if (ok) send_ack(0);
    return ok;
}

bool lan_file_frame(const char *id, const cJSON *msg, unsigned session)
{
    const cJSON *stream = cJSON_GetObjectItemCaseSensitive(msg, "stream");
    const cJSON *channel = cJSON_GetObjectItemCaseSensitive(stream, "channel");
    if (!cJSON_IsString(channel) || strcmp(channel->valuestring, "file") != 0) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_open && session == s_session && strcmp(id, s_id) == 0 && !s_eof && !s_overrun) {
        const cJSON *data = cJSON_GetObjectItemCaseSensitive(stream, "data");
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(stream, "eof"))) {
            s_eof = true;
        } else if (cJSON_IsString(data)) {
            const long n = b64_decode(data->valuestring, strlen(data->valuestring), s_frame, LAN_FILE_FRAME);
            // The server keeps within the window, so a frame always fits; one that does not ends it.
            if (n < 0 || xStreamBufferSpacesAvailable(s_buf) < (size_t)n ||
                xStreamBufferSend(s_buf, s_frame, (size_t)n, 0) != (size_t)n) {
                ESP_LOGW(TAG, "%s: a file frame did not fit (%ld B)", id, n);
                s_overrun = true;
            }
        }
    }
    xSemaphoreGive(s_lock);
    return true;
}

long lan_file_read(uint8_t *out, size_t cap, const lan_request_t *request, char *err, size_t err_size)
{
    for (;;) {
        const char *why = lan_request_check(request);
        if (why) {
            snprintf(err, err_size, "%s", why);
            return -1;
        }
        const size_t n = xStreamBufferReceive(s_buf, out, cap, pdMS_TO_TICKS(POLL_MS));
        if (n > 0) return (long)n;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        const bool eof = s_eof && xStreamBufferIsEmpty(s_buf), overrun = s_overrun;
        xSemaphoreGive(s_lock);
        if (overrun) {
            snprintf(err, err_size, "the server sent more of the file than the board can hold");
            return -1;
        }
        if (eof) return 0;
    }
}

void lan_file_ack(size_t written) { send_ack(written); }

void lan_file_close(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_open) {
        vStreamBufferDeleteWithCaps(s_buf);
        free(s_frame);
        s_buf = NULL;
        s_frame = NULL;
        s_open = false;
    }
    xSemaphoreGive(s_lock);
}
