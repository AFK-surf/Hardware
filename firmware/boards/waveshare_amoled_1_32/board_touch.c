// SPDX-License-Identifier: Apache-2.0
// Waveshare ESP32-S3-Touch-AMOLED-1.32: CST820 touch on the shared I2C bus, BOOT on GPIO0.
// The screen is polled, as on the T-Circle's CST816D (same register layout: finger count at 0x02,
// chip ID at 0xA7, I2C address 0x15). A failed read counts as no finger, so a controller that
// naps between touches only costs a NACK. A tap shorter than 280 ms becomes BOARD_INPUT_TAP when
// the finger lifts; a longer press is not a tap. BOOT is debounced here; app_poll() owns the holds.
// This board has no voice: a tap or a BOOT press only wakes the screen (board.c).

#include "board_input.h"
#include "board_pins.h"

#include <stdatomic.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define CST820_ADDRESS 0x15
#define CST820_REG_FINGER_COUNT 0x02
#define CST820_REG_DEVICE_ID 0xA7
// CST816-family "DisAutoSleep". Best effort: the polling works either way.
#define CST820_REG_DIS_AUTO_SLEEP 0xFE

#define TAP_MAX_US (280 * 1000)
#define BOOT_DEBOUNCE_US (20 * 1000)

static const char *TAG = "touch";

static i2c_master_dev_handle_t s_touch = NULL;
static bool s_touch_ready = false;

static bool s_screen_raw = false;
static atomic_bool s_boot_held = false;
static bool s_boot_raw = false;
static int64_t s_boot_change_us = 0;

static bool s_tap_armed = false;
static int64_t s_down_us = 0;

static esp_err_t touch_read_register(uint8_t reg, uint8_t *value)
{
    if (!s_touch || !value) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2c_master_transmit_receive(s_touch, &reg, 1, value, 1, 20);
}

static esp_err_t touch_write_register(uint8_t reg, uint8_t value)
{
    if (!s_touch) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t payload[] = {reg, value};
    return i2c_master_transmit(s_touch, payload, sizeof(payload), 20);
}

static bool read_screen(bool *down)
{
    if (!s_touch_ready) {
        *down = false;
        return true;
    }
    uint8_t fingers = 0;
    if (touch_read_register(CST820_REG_FINGER_COUNT, &fingers) != ESP_OK) {
        *down = false;
        return true;
    }
    *down = (fingers & 0x0f) > 0;
    return true;
}

void board_input_init(void)
{
    const gpio_config_t boot_cfg = {
        .pin_bit_mask = 1ULL << WS_BOOT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&boot_cfg);
    s_boot_raw = gpio_get_level(WS_BOOT) == 0;
    s_boot_held = s_boot_raw;
    s_boot_change_us = esp_timer_get_time();

    // INT is not used (the chip is polled); keep it a quiet input.
    const gpio_config_t int_cfg = {
        .pin_bit_mask = 1ULL << WS_TOUCH_INT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&int_cfg);
    const gpio_config_t rst_cfg = {
        .pin_bit_mask = 1ULL << WS_TOUCH_RST,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&rst_cfg);
    gpio_set_level(WS_TOUCH_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(WS_TOUCH_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(200));

    if (!ws_i2c_bus()) {
        ESP_LOGE(TAG, "CST820: no I2C bus");
        return;
    }
    const i2c_device_config_t device_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CST820_ADDRESS,
        .scl_speed_hz = 400000,
    };
    esp_err_t err = i2c_master_bus_add_device(ws_i2c_bus(), &device_cfg, &s_touch);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CST820 add device failed: %s", esp_err_to_name(err));
        return;
    }
    uint8_t device_id = 0;
    err = touch_read_register(CST820_REG_DEVICE_ID, &device_id);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CST820 probe failed: %s; BOOT still wakes the screen", esp_err_to_name(err));
        return;
    }
    err = touch_write_register(CST820_REG_DIS_AUTO_SLEEP, 0x01);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CST820 auto-sleep off failed: %s", esp_err_to_name(err));
    }
    s_touch_ready = true;
    ESP_LOGI(TAG, "CST820 ready: id=0x%02x", device_id);
}

board_input_event_t board_input_poll(void)
{
    const int64_t now = esp_timer_get_time();

    const bool boot_raw = gpio_get_level(WS_BOOT) == 0;
    if (boot_raw != s_boot_raw) {
        s_boot_raw = boot_raw;
        s_boot_change_us = now;
    }
    if (now - s_boot_change_us >= BOOT_DEBOUNCE_US) {
        s_boot_held = boot_raw;
    }

    bool screen = s_screen_raw;
    if (!read_screen(&screen)) {
        return BOARD_INPUT_NONE;
    }
    const bool pressed = screen && !s_screen_raw;
    const bool released = !screen && s_screen_raw;
    s_screen_raw = screen;

    if (pressed) {
        s_down_us = now;
        s_tap_armed = true;
    }
    if (released && s_tap_armed) {
        s_tap_armed = false;
        if (now - s_down_us <= TAP_MAX_US) {
            return BOARD_INPUT_TAP;
        }
    }
    return BOARD_INPUT_NONE;
}

bool board_input_boot_held(void)
{
    return s_boot_held;
}
