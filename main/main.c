// LilyGO T-Circle-S3: loop Cue's logo animation on the 160x160 GC9D01 round screen.
// Frames are pre-rendered from Cue's CueLogoAnimation by tools/gen_cue_logo_frames.py.
// Pins and panel setup match voice-box-agent's board_ui_t_circle.c.

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

#define LCD_HOST   SPI2_HOST
#define LCD_W      160
#define LCD_H      160
#define LCD_MOSI   GPIO_NUM_17
#define LCD_SCLK   GPIO_NUM_15
#define LCD_DC     GPIO_NUM_16
#define LCD_CS     GPIO_NUM_13
#define LCD_BL     GPIO_NUM_18  // active low

_Static_assert(CUE_LOGO_SIZE == LCD_W && CUE_LOGO_SIZE == LCD_H, "frame size must match panel");

extern const uint8_t frames_start[] asm("_binary_cue_logo_frames_bin_start");
extern const uint8_t frames_end[] asm("_binary_cue_logo_frames_bin_end");

static const char *TAG = "cue_logo";

// Two framebuffers: fill one while the other is on the SPI bus.
static DMA_ATTR uint16_t s_fb[2][LCD_W * LCD_H];
static SemaphoreHandle_t s_fb_free[2];
// Buffers on the SPI queue, oldest first; completions arrive in submit order.
static volatile uint8_t s_inflight[4];
static volatile uint32_t s_inflight_head, s_inflight_tail;
static uint16_t s_lut[16];

static bool on_color_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    BaseType_t woken = pdFALSE;
    if (s_inflight_tail != s_inflight_head) {
        xSemaphoreGiveFromISR(s_fb_free[s_inflight[s_inflight_tail++ & 3]], &woken);
    }
    return woken == pdTRUE;
}

// 4-bit coverage -> RGB565 grey ramp, byte-swapped for the SPI wire.
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

void app_main(void)
{
    ESP_ERROR_CHECK(frames_end - frames_start == CUE_LOGO_FRAMES * CUE_LOGO_FRAME_BYTES ? ESP_OK : ESP_ERR_INVALID_SIZE);
    build_lut();
    for (int i = 0; i < 2; ++i) {
        s_fb_free[i] = xSemaphoreCreateBinary();
        xSemaphoreGive(s_fb_free[i]);
    }

    gpio_config_t bl = {
        .pin_bit_mask = 1ULL << LCD_BL,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&bl));
    gpio_set_level(LCD_BL, 1);  // off while initialising

    const spi_bus_config_t bus = GC9D01_PANEL_BUS_SPI_CONFIG(LCD_SCLK, LCD_MOSI, sizeof(s_fb[0]));
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io = NULL;
    const esp_lcd_panel_io_spi_config_t io_cfg = GC9D01_PANEL_IO_SPI_CONFIG(LCD_CS, LCD_DC, on_color_done, NULL);
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io));

    esp_lcd_panel_handle_t panel = NULL;
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_gc9d01(io, &panel_cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    ESP_LOGI(TAG, "%d frames @ %d fps, hold %d", CUE_LOGO_FRAMES, CUE_LOGO_FPS, CUE_LOGO_HOLD_FRAMES);

    const TickType_t period = pdMS_TO_TICKS(1000 / CUE_LOGO_FPS);
    const int steps = CUE_LOGO_FRAMES - 1 + CUE_LOGO_HOLD_FRAMES;  // last frame == first frame
    TickType_t wake = xTaskGetTickCount();
    int buf = 0;
    bool backlight_on = false;

    for (int cycle = 0;; ++cycle) {
        int64_t cycle_start = esp_timer_get_time();
        for (int step = 0; step < steps; ++step) {
            int frame = step < CUE_LOGO_FRAMES ? step : CUE_LOGO_FRAMES - 1;
            // The hold only needs one draw; the panel keeps the last image.
            if (step < CUE_LOGO_FRAMES) {
                xSemaphoreTake(s_fb_free[buf], portMAX_DELAY);
                expand(frames_start + (size_t)frame * CUE_LOGO_FRAME_BYTES, s_fb[buf]);
                s_inflight[s_inflight_head++ & 3] = (uint8_t)buf;
                ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel, 0, 0, LCD_W, LCD_H, s_fb[buf]));
                buf ^= 1;
                if (!backlight_on) {
                    gpio_set_level(LCD_BL, 0);
                    backlight_on = true;
                }
            }
            vTaskDelayUntil(&wake, period);
        }
        ESP_LOGI(TAG, "cycle %d: %lld ms", cycle, (esp_timer_get_time() - cycle_start) / 1000);
    }
}
