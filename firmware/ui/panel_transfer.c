// SPDX-License-Identifier: Apache-2.0
#include "panel_transfer.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "memory_caps.h"

// A whole 160x160 frame takes ~10 ms at 40 MHz and a stripe ~1 ms: this is a lost transfer, not a
// slow one.
#define TRANSFER_TIMEOUT_MS 200

static const char *TAG = "panel";

esp_err_t panel_transfer_init(panel_transfer_t *t, int width, int height, int stripe_rows)
{
    memset(t, 0, sizeof(*t));
    t->width = width;
    t->height = height;
    t->stripe_rows = stripe_rows;
    const size_t fb_bytes = (size_t)width * height * sizeof(uint16_t);
    if (stripe_rows > 0) {
        t->fb = heap_caps_calloc(1, fb_bytes, COMMA_BUFFER_CAPS);
        t->stripe = heap_caps_malloc((size_t)width * stripe_rows * sizeof(uint16_t),
                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    } else {
        // 64-byte alignment covers the PSRAM DMA and cache-line rules, so the SPI driver never
        // copies the frame into an internal bounce buffer.
        t->fb = heap_caps_aligned_calloc(64, 1, fb_bytes, COMMA_BUFFER_CAPS);
    }
    t->idle = xSemaphoreCreateBinary();
    if (!t->fb || (stripe_rows > 0 && !t->stripe) || !t->idle) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(t->idle);
    return ESP_OK;
}

bool panel_transfer_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    (void)io;
    (void)edata;
    panel_transfer_t *t = ctx;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(t->idle, &woken);
    return woken == pdTRUE;
}

// Waits for the buffer. A transfer that never reported done is given up: the buffer is reclaimed for
// the next attempt, and this one fails.
static bool take_idle(panel_transfer_t *t)
{
    if (xSemaphoreTake(t->idle, pdMS_TO_TICKS(TRANSFER_TIMEOUT_MS)) == pdTRUE) {
        return true;
    }
    ESP_LOGE(TAG, "transfer lost (no done after %d ms)", TRANSFER_TIMEOUT_MS);
    xSemaphoreGive(t->idle);
    return false;
}

uint16_t *panel_transfer_frame(panel_transfer_t *t)
{
    // Stripes: flush() returns only after the last stripe is queued, and DMA never reads the
    // framebuffer, so it is free for drawing at once. Direct: it is free once the last frame is out;
    // taken here, it is given back by the done-callback after the next flush().
    return t->stripe || take_idle(t) ? t->fb : NULL;
}

esp_err_t panel_transfer_flush(panel_transfer_t *t)
{
    static bool timed;
    const int64_t start = esp_timer_get_time();
    if (!t->stripe) {
        const esp_err_t err = esp_lcd_panel_draw_bitmap(t->panel, 0, 0, t->width, t->height, t->fb);
        if (err != ESP_OK) {
            // Nothing was queued, so no done-callback will come: the buffer is free again.
            xSemaphoreGive(t->idle);
            return err;
        }
    }
    for (int y = 0; t->stripe && y < t->height; y += t->stripe_rows) {
        const int rows = t->height - y < t->stripe_rows ? t->height - y : t->stripe_rows;
        if (!take_idle(t)) {
            return ESP_ERR_TIMEOUT;
        }
        memcpy(t->stripe, t->fb + (size_t)y * t->width, (size_t)rows * t->width * sizeof(uint16_t));
        const esp_err_t err = esp_lcd_panel_draw_bitmap(t->panel, 0, y, t->width, y + rows, t->stripe);
        if (err != ESP_OK) {
            xSemaphoreGive(t->idle);
            return err;
        }
    }
    if (!timed) {
        timed = true;
        ESP_LOGI(TAG, "first frame queued in %lld ms", (long long)((esp_timer_get_time() - start) / 1000));
    }
    return ESP_OK;
}

void panel_transfer_quiesce(panel_transfer_t *t)
{
    if (take_idle(t)) {
        xSemaphoreGive(t->idle);
    }
}
