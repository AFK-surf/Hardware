/*
 * CO5300 466x466 AMOLED over QSPI (board_display.h), Waveshare ESP32-S3-Touch-AMOLED-1.32.
 *
 * The init command table and the 6-column gap are adapted from the Waveshare BSP
 * (waveshare/esp32_s3_touch_amoled_1_32, esp32_s3_touch_amoled_1_32.c):
 *   SPDX-FileCopyrightText: Waveshare
 *   SPDX-License-Identifier: Apache-2.0
 * The rest is ours.
 *
 * The 434 KB framebuffer is in PSRAM. The SPI DMA never reads PSRAM: each frame goes out in
 * 16-row stripes copied into one 14.9 KB internal DMA buffer (zero-copy for the SPI driver, no
 * bounce buffer to allocate when Wi-Fi has taken internal RAM). A stripe that fails to queue, or
 * whose done-callback does not come within STRIPE_TIMEOUT_MS, fails the frame; display.c logs it
 * and draws again later. Nothing here waits forever.
 *
 * CO5300 windows must start on an even column/row and span an even count: stripes are the full
 * 466-pixel width (columns 6..471 after the gap) and 16 rows (2 for the last), all from even rows.
 * No backlight: light is the brightness command (0x51), 0 while dark.
 */

#include "board_display.h"
#include "board_pins.h"
#include "memory_caps.h"

#include <string.h>

#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_co5300.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ui_card.h"

#define LCD_HOST SPI2_HOST
#define LCD_W 466
#define LCD_H 466
#define LCD_X_GAP 6
#define STRIPE_ROWS 16
#define STRIPE_BYTES (LCD_W * STRIPE_ROWS * sizeof(uint16_t))
#define FB_BYTES (LCD_W * LCD_H * sizeof(uint16_t))
// One stripe takes ~0.75 ms at 40 MHz QSPI; this is a lost transfer, not a slow one.
#define STRIPE_TIMEOUT_MS 200
#define BRIGHTNESS 0xCC  // 80 %: white text on black, most of the panel is off anyway
#define LCD_OPCODE_WRITE_CMD 0x02
#define LCD_CMD_SLPIN 0x10
#define LCD_CMD_SLPOUT 0x11
#define LCD_CMD_BRIGHTNESS 0x51

_Static_assert(UI_CARD_SIZE == LCD_W && UI_CARD_SIZE == LCD_H, "card size must match panel");
_Static_assert(STRIPE_ROWS % 2 == 0 && LCD_H % 2 == 0 && LCD_W % 2 == 0 && LCD_X_GAP % 2 == 0,
               "CO5300 windows must be even-aligned");

static const char *TAG = "panel";

// From the Waveshare BSP, except brightness 0 (0x51): the panel stays dark until the first frame.
static const co5300_lcd_init_cmd_t s_init_cmds[] = {
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0x00}, 1, 0},
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x06, 0x01, 0xD7}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xD1}, 4, 0},
    {0x11, (uint8_t[]){0x00}, 0, 100},
    {0x29, (uint8_t[]){0x00}, 0, 0},
};

static uint16_t *s_fb;
static uint16_t *s_stripe;
static SemaphoreHandle_t s_stripe_free;
static esp_lcd_panel_io_handle_t s_io = NULL;
static esp_lcd_panel_handle_t s_panel = NULL;

static bool on_color_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    (void)io;
    (void)edata;
    (void)ctx;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_stripe_free, &woken);
    return woken == pdTRUE;
}

static esp_err_t send_cmd(uint8_t cmd, const uint8_t *param, size_t len)
{
    const int lcd_cmd = (LCD_OPCODE_WRITE_CMD << 24) | (cmd << 8);
    return esp_lcd_panel_io_tx_param(s_io, lcd_cmd, param, len);
}

// Wait for the stripe buffer. A transfer that never reports done is taken as lost: the buffer is
// reclaimed (a late callback finds the semaphore already given, which is harmless).
static bool take_stripe(void)
{
    if (xSemaphoreTake(s_stripe_free, pdMS_TO_TICKS(STRIPE_TIMEOUT_MS)) == pdTRUE) {
        return true;
    }
    ESP_LOGE(TAG, "stripe transfer lost (no done after %d ms)", STRIPE_TIMEOUT_MS);
    return false;
}

esp_err_t board_display_init(void)
{
    s_fb = heap_caps_malloc(FB_BYTES, COMMA_BUFFER_CAPS);
    s_stripe = heap_caps_malloc(STRIPE_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    s_stripe_free = xSemaphoreCreateBinary();
    if (!s_fb || !s_stripe || !s_stripe_free) {
        return ESP_ERR_NO_MEM;
    }
    memset(s_fb, 0, FB_BYTES);
    xSemaphoreGive(s_stripe_free);

    const spi_bus_config_t bus = CO5300_PANEL_BUS_QSPI_CONFIG(WS_LCD_PCLK, WS_LCD_D0, WS_LCD_D1, WS_LCD_D2,
                                                              WS_LCD_D3, STRIPE_BYTES);
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO));
    const esp_lcd_panel_io_spi_config_t io_cfg = CO5300_PANEL_IO_QSPI_CONFIG(WS_LCD_CS, on_color_done, NULL);
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &s_io));

    co5300_vendor_config_t vendor = {
        .init_cmds = s_init_cmds,
        .init_cmds_size = sizeof(s_init_cmds) / sizeof(s_init_cmds[0]),
        .flags = {
            .use_qspi_interface = 1,
        },
    };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = WS_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
        .bits_per_pixel = 16,
        .vendor_config = &vendor,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_co5300(s_io, &panel_cfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel, LCD_X_GAP, 0));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_LOGI(TAG, "CO5300 ready: %dx%d, framebuffer %u B in %s, DMA stripe %u B internal", LCD_W, LCD_H,
             (unsigned)FB_BYTES, esp_ptr_external_ram(s_fb) ? "PSRAM" : "internal RAM", (unsigned)STRIPE_BYTES);
    return ESP_OK;
}

uint16_t *board_display_frame(void)
{
    // board_display_flush() returns only after the last stripe is queued; the framebuffer itself is
    // never read by DMA, so it is free for drawing at once.
    return s_fb;
}

esp_err_t board_display_flush(void)
{
    static bool timed;
    const int64_t start = esp_timer_get_time();
    for (int y = 0; y < LCD_H; y += STRIPE_ROWS) {
        const int rows = LCD_H - y < STRIPE_ROWS ? LCD_H - y : STRIPE_ROWS;
        if (!take_stripe()) {
            xSemaphoreGive(s_stripe_free);
            return ESP_ERR_TIMEOUT;
        }
        memcpy(s_stripe, s_fb + (size_t)y * LCD_W, (size_t)rows * LCD_W * sizeof(uint16_t));
        const esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, 0, y, LCD_W, y + rows, s_stripe);
        if (err != ESP_OK) {
            // Nothing was queued, so no done-callback will come: the buffer is free again.
            xSemaphoreGive(s_stripe_free);
            return err;
        }
    }
    if (!timed) {
        timed = true;
        ESP_LOGI(TAG, "first frame queued in %lld ms", (long long)((esp_timer_get_time() - start) / 1000));
    }
    return ESP_OK;
}

void board_display_sleep(void)
{
    send_cmd(LCD_CMD_BRIGHTNESS, (uint8_t[]){0x00}, 1);
    // Let a stripe in flight finish before the panel stops listening for pixels.
    take_stripe();
    xSemaphoreGive(s_stripe_free);
    esp_lcd_panel_disp_on_off(s_panel, false);
    send_cmd(LCD_CMD_SLPIN, NULL, 0);
    // The CO5300 wants 120 ms after SLPIN before an SLPOUT.
    vTaskDelay(pdMS_TO_TICKS(120));
}

// SLPOUT needs 120 ms before the next command. display.c then draws and turns the panel on.
void board_display_wake(void)
{
    send_cmd(LCD_CMD_SLPOUT, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(120));
}

void board_display_on(void)
{
    esp_lcd_panel_disp_on_off(s_panel, true);
}

void board_display_light(bool on)
{
    send_cmd(LCD_CMD_BRIGHTNESS, (uint8_t[]){on ? BRIGHTNESS : 0x00}, 1);
}
