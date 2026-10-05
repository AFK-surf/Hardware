// GC9D01 160x160 round panel. Idle screen is the Cue logo animation.
// Pins match the T-Circle-S3 panel used by the previous logo firmware.

#include "display.h"

#include <stdatomic.h>

#include "cue_logo_frames.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_gc9d01.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define LCD_HOST SPI2_HOST
#define LCD_W 160
#define LCD_H 160
#define LCD_MOSI GPIO_NUM_17
#define LCD_SCLK GPIO_NUM_15
#define LCD_DC GPIO_NUM_16
#define LCD_CS GPIO_NUM_13
#define LCD_BL GPIO_NUM_18

_Static_assert(CUE_LOGO_SIZE == LCD_W && CUE_LOGO_SIZE == LCD_H, "frame size must match panel");

extern const uint8_t frames_start[] asm("_binary_cue_logo_frames_bin_start");
extern const uint8_t frames_end[] asm("_binary_cue_logo_frames_bin_end");

static const char *TAG = "cue_logo";

static DMA_ATTR uint16_t s_fb[2][LCD_W * LCD_H];
static SemaphoreHandle_t s_fb_free[2];
static volatile uint8_t s_inflight[4];
static volatile uint32_t s_inflight_head, s_inflight_tail;
static uint16_t s_lut[16];
static esp_lcd_panel_handle_t s_panel = NULL;
static bool s_ready = false;

static bool on_color_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    (void)io;
    (void)edata;
    (void)ctx;
    BaseType_t woken = pdFALSE;
    if (s_inflight_tail != s_inflight_head) {
        xSemaphoreGiveFromISR(s_fb_free[s_inflight[s_inflight_tail++ & 3]], &woken);
    }
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

static void expand(const uint8_t *src, uint16_t *dst)
{
    for (int i = 0; i < CUE_LOGO_FRAME_BYTES; ++i) {
        uint8_t b = src[i];
        dst[2 * i] = s_lut[b >> 4];
        dst[2 * i + 1] = s_lut[b & 0x0f];
    }
}

esp_err_t display_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }
    if (frames_end - frames_start != CUE_LOGO_FRAMES * CUE_LOGO_FRAME_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    build_lut();
    for (int i = 0; i < 2; ++i) {
        s_fb_free[i] = xSemaphoreCreateBinary();
        if (!s_fb_free[i]) {
            return ESP_ERR_NO_MEM;
        }
        xSemaphoreGive(s_fb_free[i]);
    }

    gpio_config_t bl = {
        .pin_bit_mask = 1ULL << LCD_BL,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&bl));
    gpio_set_level(LCD_BL, 1);

    const spi_bus_config_t bus = GC9D01_PANEL_BUS_SPI_CONFIG(LCD_SCLK, LCD_MOSI, sizeof(s_fb[0]));
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io = NULL;
    const esp_lcd_panel_io_spi_config_t io_cfg = GC9D01_PANEL_IO_SPI_CONFIG(LCD_CS, LCD_DC, on_color_done, NULL);
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io));

    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_gc9d01(io, &panel_cfg, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));
    s_ready = true;
    ESP_LOGI(TAG, "%d frames @ %d fps, hold %d", CUE_LOGO_FRAMES, CUE_LOGO_FPS, CUE_LOGO_HOLD_FRAMES);
    return ESP_OK;
}

static atomic_bool s_animating;

void display_set_animating(bool animating)
{
    s_animating = animating;
}

static void draw_frame(int frame, int *buf)
{
    xSemaphoreTake(s_fb_free[*buf], portMAX_DELAY);
    expand(frames_start + (size_t)frame * CUE_LOGO_FRAME_BYTES, s_fb[*buf]);
    s_inflight[s_inflight_head++ & 3] = (uint8_t)*buf;
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(s_panel, 0, 0, LCD_W, LCD_H, s_fb[*buf]));
    *buf ^= 1;
}

void display_run_logo(void)
{
    const TickType_t period = pdMS_TO_TICKS(1000 / CUE_LOGO_FPS);
    const int steps = CUE_LOGO_FRAMES - 1 + CUE_LOGO_HOLD_FRAMES;
    TickType_t wake = xTaskGetTickCount();
    int buf = 0;

    // Idle: the finished logo, still.
    draw_frame(CUE_LOGO_FRAMES - 1, &buf);
    gpio_set_level(LCD_BL, 0);

    for (;;) {
        if (!s_animating) {
            vTaskDelay(period);
            wake = xTaskGetTickCount();
            continue;
        }
        // In a call: loop the animation. A pass that is under way when the call ends plays to its
        // last frame, so the logo always comes to rest whole.
        for (int step = 0; step < steps; ++step) {
            if (step < CUE_LOGO_FRAMES) {
                draw_frame(step, &buf);
            } else if (!s_animating) {
                break;
            }
            vTaskDelayUntil(&wake, period);
        }
    }
}
