// SPDX-License-Identifier: Apache-2.0
#pragma once

// How a display board's framebuffer reaches its panel (board_display.h), one of two ways:
//   direct:  the SPI DMA reads the framebuffer itself (the T-Circle's 50 KB frame in PSRAM);
//   stripes: the frame goes out a few rows at a time, copied into one internal DMA buffer, so the
//            SPI driver never needs a bounce buffer once Wi-Fi has taken internal RAM.
// Every wait for the panel's done-callback is bounded. A transfer that never reports done is taken
// as lost and its buffer reclaimed (a late callback then finds it already free, which is harmless):
// that frame fails, display.c logs it and draws again later, and nothing here waits forever.

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef struct {
    esp_lcd_panel_handle_t panel;  // set by the board once it has created the panel
    int width;
    int height;
    int stripe_rows;               // 0: direct
    uint16_t *fb;                  // width x height RGB565, zeroed at init
    uint16_t *stripe;              // stripe_rows x width in internal DMA RAM; NULL when direct
    SemaphoreHandle_t idle;        // given while no DMA reads the buffer it guards
} panel_transfer_t;

// Allocates the framebuffer (PSRAM where available; 64-byte aligned for direct DMA) and, with
// stripe_rows > 0, the DMA stripe.
esp_err_t panel_transfer_init(panel_transfer_t *t, int width, int height, int stripe_rows);

// The panel IO's on_color_trans_done, with the panel_transfer_t as its user_ctx.
bool panel_transfer_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *ctx);

// board_display_frame(): the framebuffer, once no DMA reads it; NULL if the last transfer was lost.
uint16_t *panel_transfer_frame(panel_transfer_t *t);
// board_display_flush(): sends the whole framebuffer.
esp_err_t panel_transfer_flush(panel_transfer_t *t);
// Before the panel goes to sleep: lets a transfer in flight finish, or gives it up.
void panel_transfer_quiesce(panel_transfer_t *t);
