// SPDX-License-Identifier: Apache-2.0
// Waveshare ESP32-S3-Touch-AMOLED-1.43C: ES8311 speaker codec, ES7210 mics and NS4150B amplifier
// (board_audio.c), CST820 touch and BOOT (board_touch.c), CO5300 466x466 AMOLED (panel.c). Calls,
// the screen and BOOT holds are shared (voice/call.c, ui/display.c, main.c). The board's PWR key
// switches the power path in hardware and is not read by the firmware.
#include "board.h"
#include "board_audio.h"
#include "board_input.h"
#include "board_pins.h"
#include "call.h"
#include "display.h"

#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_system.h"

static const char *TAG = "main";
static i2c_master_bus_handle_t s_i2c;

i2c_master_bus_handle_t ws_i2c_bus(void)
{
    return s_i2c;
}

static void i2c_init(void)
{
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = WS_I2C_SDA,
        .scl_io_num = WS_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    const esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_i2c);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed: %s", esp_err_to_name(err));
        s_i2c = NULL;
    }
}

static void output_pin(gpio_num_t pin, int level, bool pull_down)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = pull_down ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_set_level(pin, level);
    if (gpio_config(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "GPIO%d config failed", pin);
    }
    gpio_set_level(pin, level);
}

static const char *reset_reason(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_EXT: return "external pin";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "SDIO";
    case ESP_RST_USB: return "USB";
    case ESP_RST_JTAG: return "JTAG";
    case ESP_RST_EFUSE: return "eFuse";
    case ESP_RST_PWR_GLITCH: return "power glitch";
    case ESP_RST_CPU_LOCKUP: return "CPU lockup";
    default: return "unknown";
    }
}

void board_init(void)
{
    const esp_reset_reason_t why = esp_reset_reason();
    ESP_LOGI(TAG, "reset reason: %s (%d)", reset_reason(why), (int)why);
    // Amplifier off before anything touches the codecs; it is switched on only while playing.
    output_pin(WS_PA, 0, true);
    // Codec supply on (the board's pull-up already enables it; driven so it cannot float), then
    // give the codecs time to come out of power-on reset before the first I2C access.
    output_pin(WS_CODEC_EN, 1, false);
    esp_rom_delay_us(10 * 1000);
    i2c_init();
    const esp_err_t audio = board_audio_init();
    if (audio != ESP_OK) {
        ESP_LOGE(TAG, "audio init failed: %s", esp_err_to_name(audio));
    }
    call_voice_clean_init();
    board_input_init();
    call_init();
    ESP_ERROR_CHECK(display_init());
}

void board_run_ui(void *arg)
{
    (void)arg;
    call_run_ui("tap the screen or press BOOT to call, again to hang up; hold BOOT 3 s to pair, 10 s to reset");
}
