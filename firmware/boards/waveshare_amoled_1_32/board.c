// Waveshare ESP32-S3-Touch-AMOLED-1.32: ES8311 audio (board_audio.c), CST820 touch and BOOT
// (board_touch.c), CO5300 AMOLED (panel.c). Calls, the screen and BOOT holds are shared
// (voice/call.c, ui/display.c, main.c). A short BOOT press calls too (COMMA_BOOT_PRESS_CALLS).
#include "board.h"
#include "board_audio.h"
#include "board_input.h"
#include "board_pins.h"
#include "call.h"
#include "display.h"

#include "esp_log.h"

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

void board_init(void)
{
    // board_audio_init() pulls the speaker PA low before it touches the codec.
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
