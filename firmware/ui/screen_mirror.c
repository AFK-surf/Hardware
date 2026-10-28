// SPDX-License-Identifier: Apache-2.0
// Screen mirror over USB (screen_mirror.h). The display task codes each frame into a back buffer;
// a low-priority task sends the newest coded frame, so a slow or absent reader never holds up the
// screen. Nothing runs or is allocated until a computer turns mirroring on.

#include "screen_mirror.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "display.h"
#include "memory_caps.h"
#include "ui_card.h"  // UI_CARD_W, UI_CARD_H

#include "driver/usb_serial_jtag.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define KEEPALIVE_US (30LL * 1000000)
// A coded frame larger than this is not sent (a settings page codes to roughly 10-40 KB).
#define CODED_MAX (160 * 1024)
#define PART_BYTES 600
#define WRITE_WAIT_MS 200

static const char *TAG = "mirror";

typedef struct {
    uint8_t *data;
    size_t len;
} coded_t;

static atomic_bool s_on;
static _Atomic int64_t s_keepalive_us;
static atomic_bool s_force;
static SemaphoreHandle_t s_lock;  // guards s_back and s_pending
static TaskHandle_t s_task;
static coded_t s_back, s_front;
static bool s_pending;
static uint32_t s_sent_hash;
static uint32_t s_frame_id;

static uint32_t fnv1a(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    while (n--) {
        h ^= *p++;
        h *= 16777619u;
    }
    return h;
}

static size_t base64(const uint8_t *in, size_t n, char *out)
{
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = T[v >> 18 & 63];
        out[o++] = T[v >> 12 & 63];
        out[o++] = i + 1 < n ? T[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? T[v & 63] : '=';
    }
    return o;
}

static bool write_all(const char *s, size_t n)
{
    return usb_serial_jtag_write_bytes(s, n, pdMS_TO_TICKS(WRITE_WAIT_MS)) == (int)n;
}

static void send_frame(const coded_t *f)
{
    const uint32_t hash = fnv1a(f->data, f->len);
    const bool force = atomic_exchange(&s_force, false);
    if (hash == s_sent_hash && !force) {
        return;
    }
    const unsigned id = ++s_frame_id;
    const unsigned parts = (unsigned)((f->len + PART_BYTES - 1) / PART_BYTES);
    static char line[64 + PART_BYTES * 4 / 3 + 8];
    for (unsigned i = 0; i < parts; ++i) {
        const size_t off = (size_t)i * PART_BYTES;
        const size_t n = f->len - off < PART_BYTES ? f->len - off : PART_BYTES;
        int h = snprintf(line, sizeof(line), "@@F %u %u %u %08lx ", id, i, parts, (unsigned long)hash);
        size_t len = (size_t)h + base64(f->data + off, n, line + h);
        line[len++] = '\n';
        if (!write_all(line, len)) {
            return;  // nobody reading: try again with the next frame or refresh
        }
    }
    s_sent_hash = hash;
}

static void mirror_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        bool have = false;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_pending) {
            const coded_t t = s_front;
            s_front = s_back;
            s_back = t;
            s_pending = false;
            have = true;
        }
        xSemaphoreGive(s_lock);
        if (have && s_on) {
            send_frame(&s_front);
        } else if (!have && s_on && s_force && s_front.len) {
            s_sent_hash = 0;
            send_frame(&s_front);
        }
    }
}

static bool start(void)
{
    if (s_task) {
        return true;
    }
    s_lock = xSemaphoreCreateMutex();
    s_back.data = heap_caps_malloc(CODED_MAX, COMMA_BUFFER_CAPS);
    s_front.data = heap_caps_malloc(CODED_MAX, COMMA_BUFFER_CAPS);
    if (!s_lock || !s_back.data || !s_front.data ||
        xTaskCreate(mirror_task, "mirror", 3072, NULL, 1, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "no memory for the screen mirror");
        return false;
    }
    return true;
}

void screen_mirror_request(bool on)
{
    if (on && !start()) {
        return;
    }
    s_keepalive_us = esp_timer_get_time();
    if (on && !s_on) {
        ESP_LOGI(TAG, "on");
        s_force = true;
        s_sent_hash = 0;
    } else if (!on && s_on) {
        ESP_LOGI(TAG, "off");
    }
    const bool was_on = s_on;
    s_on = on;
    if (on && !was_on) {
        display_redraw();  // the screen may not change for a while: send what it shows now
    }
}

void screen_mirror_refresh(void)
{
    if (s_on && s_task) {
        s_force = true;
        display_redraw();
    }
}

// Panel pixels are RGB565, big-endian, from the 16-level gray table; green's 6 bits tell the level.
static inline uint8_t level_of(uint16_t p)
{
    const uint16_t c = (uint16_t)(p >> 8 | p << 8);
    return (uint8_t)((((c >> 5) & 0x3f) * 15 + 31) / 63);
}

static size_t code_frame(const uint16_t *fb, uint8_t *out)
{
    size_t o = 0;
    out[o++] = UI_CARD_W & 0xff;
    out[o++] = UI_CARD_W >> 8;
    out[o++] = UI_CARD_H & 0xff;
    out[o++] = UI_CARD_H >> 8;
    const size_t total = (size_t)UI_CARD_W * UI_CARD_H;
    size_t i = 0;
    while (i < total) {
        const uint8_t lv = level_of(fb[i]);
        size_t run = 1;
        while (i + run < total && run < 65535 && level_of(fb[i + run]) == lv) {
            ++run;
        }
        if (o + 3 > CODED_MAX) {
            return 0;
        }
        if (run <= 15) {
            out[o++] = (uint8_t)(lv << 4 | (run - 1));
        } else {
            out[o++] = (uint8_t)(lv << 4 | 15);
            out[o++] = run & 0xff;
            out[o++] = run >> 8;
        }
        i += run;
    }
    return o;
}

void screen_mirror_capture(const uint16_t *fb)
{
    if (!s_on) {
        return;
    }
    if (esp_timer_get_time() - s_keepalive_us > KEEPALIVE_US) {
        s_on = false;
        ESP_LOGI(TAG, "off: no keepalive");
        return;
    }
    // Coding takes a few milliseconds of the display task; the lock only guards the swap.
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const size_t len = code_frame(fb, s_back.data);
    if (len) {
        s_back.len = len;
        s_pending = true;
    }
    xSemaphoreGive(s_lock);
    if (len) {
        xTaskNotifyGive(s_task);
    }
}

void screen_mirror_sleep(void)
{
    if (s_on) {
        static const char OFF[] = "@@S off\n";
        write_all(OFF, sizeof(OFF) - 1);
        s_sent_hash = 0;  // the first frame after waking goes out even if unchanged
    }
}
