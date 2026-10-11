#include "memory_caps.h"
// GC9D01 160x160 round panel. Idle screen is the Cue logo, still; calls and setup show text cards.
// The screen redraws only when what it shows changes, and sleeps (backlight off, panel in SLPIN)
// after DISPLAY_SLEEP_US without activity. Pins match the T-Circle-S3 panel used by the previous
// logo firmware.

#include "display.h"

#include <stdatomic.h>
#include <string.h>

#include "cue_logo.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_gc9d01.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
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

#define DISPLAY_SLEEP_US (15LL * 1000000)
#define POLL_MS 20

_Static_assert(CUE_LOGO_SIZE == LCD_W && CUE_LOGO_SIZE == LCD_H, "logo size must match panel");

extern const uint8_t logo_start[] asm("_binary_cue_logo_bin_start");
extern const uint8_t logo_end[] asm("_binary_cue_logo_bin_end");

static const char *TAG = "display";

#define FB_BYTES (LCD_W * LCD_H * sizeof(uint16_t))
// One 50 KB framebuffer in PSRAM; the SPI DMA reads it directly (psram_dma_direct). 64-byte
// alignment covers the PSRAM DMA and cache-line rules, so the driver never copies it into an
// internal bounce buffer. Redraws are rare, so drawing just waits for the previous transfer.
static uint16_t *s_fb;
static SemaphoreHandle_t s_fb_free;
static uint16_t s_lut[16];
static esp_lcd_panel_io_handle_t s_io = NULL;
static esp_lcd_panel_handle_t s_panel = NULL;
static bool s_ready = false;

static bool on_color_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    (void)io;
    (void)edata;
    (void)ctx;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_fb_free, &woken);
    return woken == pdTRUE;
}

static void build_lut(void)
{
    for (int i = 0; i < 16; ++i) {
        uint8_t v = (uint8_t)(i * 17);
        uint16_t c = ((uint16_t)(v & 0xf8) << 8) | ((uint16_t)(v & 0xfc) << 3) | (v >> 3);
        s_lut[i] = (uint16_t)((c << 8) | (c >> 8));
    }
}

static void expand_logo(uint16_t *dst)
{
    for (int i = 0; i < CUE_LOGO_BYTES; ++i) {
        uint8_t b = logo_start[i];
        dst[2 * i] = s_lut[b >> 4];
        dst[2 * i + 1] = s_lut[b & 0x0f];
    }
}

esp_err_t display_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }
    if (logo_end - logo_start != CUE_LOGO_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    build_lut();
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
    s_ready = true;
    return ESP_OK;
}

// ---- What to show, and when to sleep ----------------------------------------------------------

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static ui_card_t s_card;
static bool s_card_on;
static atomic_bool s_dirty;
static atomic_bool s_wake_req;
static atomic_bool s_keep_awake;
static _Atomic int64_t s_activity_us;
static atomic_bool s_asleep;

static void set_content(bool card_on, const char *label, const char *big, const char *body)
{
    ui_card_t card = {0};
    if (card_on) {
        strlcpy(card.label, label ? label : "", sizeof(card.label));
        strlcpy(card.big, big ? big : "", sizeof(card.big));
        strlcpy(card.body, body ? body : "", sizeof(card.body));
    }
    taskENTER_CRITICAL(&s_mux);
    // Same content as on screen: no redraw, but it still counts as activity.
    if (card_on != s_card_on || memcmp(&card, &s_card, sizeof(card)) != 0) {
        s_card_on = card_on;
        s_card = card;
        s_dirty = true;
    }
    taskEXIT_CRITICAL(&s_mux);
    display_wake();
}

void display_show_card(const char *label, const char *big, const char *body)
{
    set_content(true, label, big, body);
}

void display_show_logo(void)
{
    set_content(false, NULL, NULL, NULL);
}

void display_wake(void)
{
    s_activity_us = esp_timer_get_time();
    s_wake_req = true;
}

void display_keep_awake(bool on)
{
    if (on && !s_keep_awake) {
        s_activity_us = esp_timer_get_time();
    }
    s_keep_awake = on;
    if (on) {
        s_wake_req = true;
    }
}

bool display_asleep(void)
{
    return s_asleep;
}

static void draw(bool card_on, const ui_card_t *card)
{
    xSemaphoreTake(s_fb_free, portMAX_DELAY);
    if (card_on) {
        ui_card_render(card, s_fb, s_lut);
    } else {
        expand_logo(s_fb);
    }
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(s_panel, 0, 0, LCD_W, LCD_H, s_fb));
}

static void panel_sleep(void)
{
    gpio_set_level(LCD_BL, BL_OFF);
    // Let a transfer in flight finish before the panel stops listening for pixels.
    xSemaphoreTake(s_fb_free, portMAX_DELAY);
    xSemaphoreGive(s_fb_free);
    esp_lcd_panel_disp_on_off(s_panel, false);
    esp_lcd_panel_io_tx_param(s_io, LCD_CMD_SLPIN, NULL, 0);
    s_asleep = true;
    ESP_LOGI(TAG, "screen off");
}

// SLPOUT needs 120 ms before the next command. The caller then draws and turns the panel on.
static void panel_wake(void)
{
    esp_lcd_panel_io_tx_param(s_io, LCD_CMD_SLPOUT, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(120));
    s_asleep = false;
    ESP_LOGI(TAG, "screen on");
}

void display_run(void)
{
    s_activity_us = esp_timer_get_time();
    bool lit = false;
    for (;;) {
        const bool wake = atomic_exchange(&s_wake_req, false);
        const bool woke = wake && s_asleep;
        if (woke) {
            panel_wake();
        }
        if (woke || s_dirty || !lit) {
            ui_card_t card;
            bool card_on;
            taskENTER_CRITICAL(&s_mux);
            card = s_card;
            card_on = s_card_on;
            s_dirty = false;
            taskEXIT_CRITICAL(&s_mux);
            if (!s_asleep) {
                // Panel RAM survives SLPIN, but a wake redraws anyway: cheap, and it can't show a
                // stale frame. A change while asleep waits for the wake (s_dirty stays set).
                draw(card_on, &card);
                if (woke) {
                    esp_lcd_panel_disp_on_off(s_panel, true);
                }
                if (!lit || woke) {
                    gpio_set_level(LCD_BL, BL_ON);
                    lit = true;
                }
            } else {
                s_dirty = true;
            }
        }
        if (!s_asleep && !s_keep_awake && esp_timer_get_time() - s_activity_us >= DISPLAY_SLEEP_US) {
            panel_sleep();
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}
