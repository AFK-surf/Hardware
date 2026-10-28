// SPDX-License-Identifier: Apache-2.0
/*
 * CO5300 466x466 AMOLED over QSPI (board_display.h), Waveshare ESP32-S3-Touch-AMOLED-1.43C.
 * The 1.32's panel.c with this board's pins and orientation.
 *
 * The init command table is adapted from the Waveshare BSP for the 1.32
 * (waveshare/esp32_s3_touch_amoled_1_32, esp32_s3_touch_amoled_1_32.c):
 *   SPDX-FileCopyrightText: Waveshare
 *   SPDX-License-Identifier: Apache-2.0
 * The 1.43C mounts the panel turned by 180 degrees: Waveshare's 1.43C example
 * (port_bsp/esp32_s3_touch_amoled_1.43c.cpp) adds MADCTL 0xC0 (rows and columns mirrored) to the
 * same table and moves the column gap from 6 to 8 (480 - 466 - 6), which this file follows.
 * The rest is ours.
 *
 * The 434 KB framebuffer is in PSRAM. The SPI DMA never reads PSRAM: each frame goes out in
 * 16-row stripes through one 14.9 KB internal DMA buffer (panel_transfer.c), with no bounce buffer
 * to allocate when Wi-Fi has taken internal RAM.
 *
 * CO5300 windows must start on an even column/row and span an even count: stripes are the full
 * 466-pixel width (columns 8..473 after the gap) and 16 rows (2 for the last), all from even rows.
 * No backlight: light is the brightness command (0x51), 0 while dark.
 */

#include "board_display.h"
#include "board_pins.h"

#include "driver/spi_master.h"
#include "esp_lcd_co5300.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "panel_transfer.h"
#include "ui_card.h"

#define LCD_HOST SPI2_HOST
#define LCD_W 466
#define LCD_H 466
// 8 with MADCTL 0xC0 (the 1.32 uses 6 without it).
#define LCD_X_GAP 8
#define STRIPE_ROWS 16
#define STRIPE_BYTES (LCD_W * STRIPE_ROWS * sizeof(uint16_t))
#define FB_BYTES (LCD_W * LCD_H * sizeof(uint16_t))
#define BRIGHTNESS 0xCC  // 80 %: white text on black, most of the panel is off anyway
#define LCD_OPCODE_WRITE_CMD 0x02
#define LCD_CMD_SLPIN 0x10
#define LCD_CMD_SLPOUT 0x11
#define LCD_CMD_BRIGHTNESS 0x51

_Static_assert(UI_CARD_SIZE == LCD_W && UI_CARD_SIZE == LCD_H, "card size must match panel");
_Static_assert(STRIPE_ROWS % 2 == 0 && LCD_H % 2 == 0 && LCD_W % 2 == 0 && LCD_X_GAP % 2 == 0,
               "CO5300 windows must be even-aligned");

static const char *TAG = "panel";

// From the Waveshare BSP, except brightness 0 (0x51): the panel stays dark until the first frame;
// plus MADCTL 0xC0 (0x36), as in Waveshare's 1.43C example: the panel is mounted upside down.
static const co5300_lcd_init_cmd_t s_init_cmds[] = {
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0x00}, 1, 0},
    {0x36, (uint8_t[]){0xC0}, 1, 0},
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x06, 0x01, 0xD7}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xD1}, 4, 0},
    {0x11, (uint8_t[]){0x00}, 0, 100},
    {0x29, (uint8_t[]){0x00}, 0, 0},
};

static panel_transfer_t s_transfer;
static esp_lcd_panel_io_handle_t s_io = NULL;

static esp_err_t send_cmd(uint8_t cmd, const uint8_t *param, size_t len)
{
    const int lcd_cmd = (LCD_OPCODE_WRITE_CMD << 24) | (cmd << 8);
    return esp_lcd_panel_io_tx_param(s_io, lcd_cmd, param, len);
}

esp_err_t board_display_init(void)
{
    const esp_err_t err = panel_transfer_init(&s_transfer, LCD_W, LCD_H, STRIPE_ROWS);
    if (err != ESP_OK) {
        return err;
    }

    const spi_bus_config_t bus = CO5300_PANEL_BUS_QSPI_CONFIG(WS_LCD_PCLK, WS_LCD_D0, WS_LCD_D1, WS_LCD_D2,
                                                              WS_LCD_D3, STRIPE_BYTES);
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO));
    const esp_lcd_panel_io_spi_config_t io_cfg = CO5300_PANEL_IO_QSPI_CONFIG(WS_LCD_CS, panel_transfer_done, &s_transfer);
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
    ESP_ERROR_CHECK(esp_lcd_new_panel_co5300(s_io, &panel_cfg, &s_transfer.panel));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_transfer.panel, LCD_X_GAP, 0));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_transfer.panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_transfer.panel));
    ESP_LOGI(TAG, "CO5300 ready: %dx%d, framebuffer %u B in %s, DMA stripe %u B internal", LCD_W, LCD_H,
             (unsigned)FB_BYTES, esp_ptr_external_ram(s_transfer.fb) ? "PSRAM" : "internal RAM",
             (unsigned)STRIPE_BYTES);
    return ESP_OK;
}

uint16_t *board_display_frame(void)
{
    return panel_transfer_frame(&s_transfer);
}

esp_err_t board_display_flush(void)
{
    return panel_transfer_flush(&s_transfer);
}

void board_display_sleep(void)
{
    send_cmd(LCD_CMD_BRIGHTNESS, (uint8_t[]){0x00}, 1);
    // Let a stripe in flight finish before the panel stops listening for pixels.
    panel_transfer_quiesce(&s_transfer);
    esp_lcd_panel_disp_on_off(s_transfer.panel, false);
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
    esp_lcd_panel_disp_on_off(s_transfer.panel, true);
}

void board_display_light(bool on)
{
    send_cmd(LCD_CMD_BRIGHTNESS, (uint8_t[]){on ? BRIGHTNESS : 0x00}, 1);
}
