// SPDX-License-Identifier: Apache-2.0
#include "memory_caps.h"
// GC9D01 160x160 round panel (board_display.h). Pins match the T-Circle-S3 panel used by the
// previous logo firmware. Sleep turns the backlight off and puts the panel in SLPIN.

#include "board_display.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_gc9d01.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
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
// One 50 KB framebuffer in PSRAM; the SPI DMA reads it directly (psram_dma_direct). 64-byte
// alignment covers the PSRAM DMA and cache-line rules, so the driver never copies it into an
// internal bounce buffer. Redraws are rare, so drawing just waits for the previous transfer.
static uint16_t *s_fb;
static SemaphoreHandle_t s_fb_free;
static esp_lcd_panel_io_handle_t s_io = NULL;
static esp_lcd_panel_handle_t s_panel = NULL;

static bool on_color_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    (void)io;
    (void)edata;
    (void)ctx;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_fb_free, &woken);
    return woken == pdTRUE;
}

esp_err_t board_display_init(void)
{
    s_fb = heap_caps_aligned_calloc(64, 1, FB_BYTES, COMMA_BUFFER_CAPS);
    s_fb_free = xSemaphoreCreateBinary();
    if (!s_fb || !s_fb_free) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(s_fb_free);

    gpio_config_t bl = {
        .pin_bit_mask = 1ULL << LCD_BL,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&bl));
    gpio_set_level(LCD_BL, BL_OFF);

    const spi_bus_config_t bus = GC9D01_PANEL_BUS_SPI_CONFIG(LCD_SCLK, LCD_MOSI, FB_BYTES);
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_spi_config_t io_cfg = GC9D01_PANEL_IO_SPI_CONFIG(LCD_CS, LCD_DC, on_color_done, NULL);
    io_cfg.flags.psram_dma_direct = 1;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &s_io));

    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_gc9d01(s_io, &panel_cfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));
    return ESP_OK;
}

uint16_t *board_display_frame(void)
{
    xSemaphoreTake(s_fb_free, portMAX_DELAY);
    return s_fb;
}

esp_err_t board_display_flush(void)
{
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(s_panel, 0, 0, LCD_W, LCD_H, s_fb));
    return ESP_OK;
}

void board_display_sleep(void)
{
    gpio_set_level(LCD_BL, BL_OFF);
    // Let a transfer in flight finish before the panel stops listening for pixels.
    xSemaphoreTake(s_fb_free, portMAX_DELAY);
    xSemaphoreGive(s_fb_free);
    esp_lcd_panel_disp_on_off(s_panel, false);
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
    esp_lcd_panel_disp_on_off(s_panel, true);
}

void board_display_light(bool on)
{
    gpio_set_level(LCD_BL, on ? BL_ON : BL_OFF);
}
