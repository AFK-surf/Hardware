// SPDX-License-Identifier: Apache-2.0
// GC9D01 160x160 round panel (board_display.h). Pins match the T-Circle-S3 panel used by the
// previous logo firmware. Sleep turns the backlight off and puts the panel in SLPIN.
//
// One 50 KB framebuffer in PSRAM that the SPI DMA reads directly (psram_dma_direct). Redraws are
// rare, so drawing waits for the previous transfer, never longer than panel_transfer.c allows.

#include "board_display.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_gc9d01.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "panel_transfer.h"
#include "ui_card.h"

#define LCD_HOST SPI2_HOST
#define LCD_W 160
#define LCD_H 160
#define LCD_MOSI GPIO_NUM_17
#define LCD_SCLK GPIO_NUM_15
#define LCD_DC GPIO_NUM_16
#define LCD_CS GPIO_NUM_13
#define LCD_BL GPIO_NUM_18
// The backlight is on when GPIO18 is low.
#define BL_ON 0
#define BL_OFF 1
#define LCD_CMD_SLPIN 0x10
#define LCD_CMD_SLPOUT 0x11

_Static_assert(UI_CARD_SIZE == LCD_W && UI_CARD_SIZE == LCD_H, "card size must match panel");

#define FB_BYTES (LCD_W * LCD_H * sizeof(uint16_t))
static panel_transfer_t s_transfer;
static esp_lcd_panel_io_handle_t s_io = NULL;

esp_err_t board_display_init(void)
{
    const esp_err_t err = panel_transfer_init(&s_transfer, LCD_W, LCD_H, 0);
    if (err != ESP_OK) {
        return err;
    }

    gpio_config_t bl = {
        .pin_bit_mask = 1ULL << LCD_BL,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&bl));
    gpio_set_level(LCD_BL, BL_OFF);

    const spi_bus_config_t bus = GC9D01_PANEL_BUS_SPI_CONFIG(LCD_SCLK, LCD_MOSI, FB_BYTES);
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_cfg =
        GC9D01_PANEL_IO_SPI_CONFIG(LCD_CS, LCD_DC, panel_transfer_done, &s_transfer);
    io_cfg.flags.psram_dma_direct = 1;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &s_io));

    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_gc9d01(s_io, &panel_cfg, &s_transfer.panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_transfer.panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_transfer.panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_transfer.panel, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_transfer.panel, true));
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
    gpio_set_level(LCD_BL, BL_OFF);
    // Let a transfer in flight finish before the panel stops listening for pixels.
    panel_transfer_quiesce(&s_transfer);
    esp_lcd_panel_disp_on_off(s_transfer.panel, false);
    esp_lcd_panel_io_tx_param(s_io, LCD_CMD_SLPIN, NULL, 0);
}

// SLPOUT needs 120 ms before the next command. display.c then draws and turns the panel on.
void board_display_wake(void)
{
    esp_lcd_panel_io_tx_param(s_io, LCD_CMD_SLPOUT, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(120));
}

void board_display_on(void)
{
    esp_lcd_panel_disp_on_off(s_transfer.panel, true);
}

void board_display_light(bool on)
{
    gpio_set_level(LCD_BL, on ? BL_ON : BL_OFF);
}
