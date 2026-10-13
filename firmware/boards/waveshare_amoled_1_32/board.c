// SPDX-License-Identifier: Apache-2.0
// Waveshare ESP32-S3-Touch-AMOLED-1.32: display and Connector gateway, no voice. The board has a mic
// (ES8311) but no speaker, only a speaker header, so the codec is never set up and the speaker PA
// (GPIO46) is held low. CST820 touch and BOOT (board_touch.c), CO5300 AMOLED (panel.c); the screen
// and BOOT holds are shared (ui/display.c, main.c). A tap or a short BOOT press only wakes the screen.
#include "app_controls.h"
#include "board.h"
#include "board_input.h"
#include "board_pins.h"
#include "display.h"
#include "provisioning.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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

// Nothing drives the amplifier on this board: low, with the pull-down, from the first moment.
static void pa_off(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << WS_PA,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_set_level(WS_PA, 0);
    if (gpio_config(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "speaker PA pin config failed");
    }
    gpio_set_level(WS_PA, 0);
}

void board_init(void)
{
    pa_off();
    i2c_init();
    board_input_init();
    ESP_ERROR_CHECK(display_init());
}

void board_run_ui(void *arg)
{
    (void)arg;
    app_controls_init(board_input_boot_held());
    for (;;) {
        const int64_t now = esp_timer_get_time();
        if (board_input_poll() == BOARD_INPUT_TAP) {
            display_wake();
        }
        // app_poll() wakes the screen when BOOT goes down, and owns the 3 s / 10 s holds.
        const bool boot_tracking = app_poll(now, false, board_input_boot_held());
        display_keep_awake(provisioning_active() || boot_tracking);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
