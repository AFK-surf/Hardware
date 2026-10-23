/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * ST7789 1.14" 135x240 LCD, portrait (board_display.h), M5Stack StickS3. The panel setup (SPI3 at
 * 40 MHz, mode 0, inverted colours, the 135x240 window at x 52, y 40 of the controller's 240x320,
 * a 20 kHz LEDC backlight) is from muse-gadget-sdk's board_m5stack_sticks3.c. The transfer is
 * the Waveshare AMOLED's scheme (ours).
 *
 * The 64.8 KB framebuffer is in PSRAM. The SPI DMA never reads PSRAM: each frame goes out in
 * 16-row stripes through one 4.3 KB internal DMA buffer (panel_transfer.c), so the SPI driver never
 * has to find a bounce buffer once Wi-Fi has taken internal RAM.
 */

#include "board_display.h"
#include "board_pins.h"

#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "panel_transfer.h"
#include "ui_card.h"

#define LCD_HOST SPI3_HOST
#define LCD_W 135
#define LCD_H 240
#define LCD_GAP_X 52
#define LCD_GAP_Y 40
#define LCD_PCLK_HZ (40 * 1000 * 1000)
#define STRIPE_ROWS 16
#define STRIPE_BYTES (LCD_W * STRIPE_ROWS * sizeof(uint16_t))
#define FB_BYTES (LCD_W * LCD_H * sizeof(uint16_t))
#define BL_TIMER LEDC_TIMER_0
#define BL_CHANNEL LEDC_CHANNEL_0
#define BL_BITS LEDC_TIMER_10_BIT
#define BL_DUTY_ON (1023 * 80 / 100)  // 80 %, as on the Waveshare: white text on black

_Static_assert(UI_CARD_W == LCD_W && UI_CARD_H == LCD_H && !UI_CARD_ROUND, "card size must match panel");
_Static_assert(STRIPE_BYTES < 16384, "the stripe stays under SPIRAM_MALLOC_ALWAYSINTERNAL");

static const char *TAG = "panel";

static panel_transfer_t s_transfer;
static esp_lcd_panel_io_handle_t s_io = NULL;

static void set_light(uint32_t duty)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BL_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BL_CHANNEL);
}

static esp_err_t backlight_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = BL_BITS,
        .timer_num = BL_TIMER,
        .freq_hz = 20000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    const ledc_channel_config_t channel = {
        .gpio_num = STICK_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BL_CHANNEL,
        .timer_sel = BL_TIMER,
        .duty = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "backlight timer");
    return ledc_channel_config(&channel);
}

esp_err_t board_display_init(void)
{
    ESP_RETURN_ON_ERROR(panel_transfer_init(&s_transfer, LCD_W, LCD_H, STRIPE_ROWS), TAG, "framebuffer");

    // Dark until the first frame (display.c turns the light on after it).
    ESP_ERROR_CHECK(backlight_init());

    const spi_bus_config_t bus = {
        .sclk_io_num = STICK_LCD_SCLK,
        .mosi_io_num = STICK_LCD_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = STRIPE_BYTES,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO));
    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = STICK_LCD_CS,
        .dc_gpio_num = STICK_LCD_DC,
        .spi_mode = 0,
        .pclk_hz = LCD_PCLK_HZ,
        .trans_queue_depth = 10,
        .on_color_trans_done = panel_transfer_done,
        .user_ctx = &s_transfer,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &s_io));
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = STICK_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(s_io, &panel_cfg, &s_transfer.panel));
    // Portrait, the front key below the screen: the controller's own orientation.
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_transfer.panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_transfer.panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_transfer.panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_transfer.panel, LCD_GAP_X, LCD_GAP_Y));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_transfer.panel, true));
    ESP_LOGI(TAG, "ST7789 ready: %dx%d, framebuffer %u B in %s, DMA stripe %u B internal", LCD_W, LCD_H,
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
    set_light(0);
    // Let a stripe in flight finish before the panel stops listening for pixels.
    panel_transfer_quiesce(&s_transfer);
    esp_lcd_panel_disp_on_off(s_transfer.panel, false);
    esp_lcd_panel_disp_sleep(s_transfer.panel, true);  // SLPIN; the ST7789 wants 120 ms before SLPOUT
    vTaskDelay(pdMS_TO_TICKS(120));
}

// SLPOUT needs 120 ms before the next command. display.c then draws and turns the panel on.
void board_display_wake(void)
{
    esp_lcd_panel_disp_sleep(s_transfer.panel, false);
    vTaskDelay(pdMS_TO_TICKS(120));
}

void board_display_on(void)
{
    esp_lcd_panel_disp_on_off(s_transfer.panel, true);
}

void board_display_light(bool on)
{
    set_light(on ? BL_DUTY_ON : 0);
}
