// SPDX-License-Identifier: Apache-2.0
#include "cst816_touch.h"

#include "board_input.h"

#include <stdatomic.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define CST816_ADDRESS 0x15
#define CST816_REG_FINGER_COUNT 0x02
#define CST816_REG_DEVICE_ID 0xA7

#define TAP_MAX_US (280 * 1000)
#define BOOT_DEBOUNCE_US (20 * 1000)

static const char *TAG = "touch";

static cst816_touch_config_t s_config;
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
    return i2c_master_transmit_receive(s_touch, &reg, 1, value, 1, 20);
}

static esp_err_t touch_write_register(uint8_t reg, uint8_t value)
{
    const uint8_t payload[] = {reg, value};
    return i2c_master_transmit(s_touch, payload, sizeof(payload), 20);
}

// False when nothing can be said about the finger this time.
static bool read_screen(bool *down)
{
    if (!s_touch_ready) {
        *down = false;
        return true;
    }
    uint8_t fingers = 0;
    if (touch_read_register(CST816_REG_FINGER_COUNT, &fingers) != ESP_OK) {
        *down = false;
        return s_config.failed_read_is_release;
    }
    *down = (fingers & 0x0f) > 0;
    return true;
}

static void quiet_input(gpio_num_t gpio)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
}

void cst816_touch_init(const cst816_touch_config_t *config)
{
    s_config = *config;
    quiet_input(config->boot);
    s_boot_raw = gpio_get_level(config->boot) == 0;
    s_boot_held = s_boot_raw;
    s_boot_change_us = esp_timer_get_time();

    if (config->interrupt != GPIO_NUM_NC) {
        quiet_input(config->interrupt);
    }
    if (config->reset != GPIO_NUM_NC) {
        const gpio_config_t rst_cfg = {
            .pin_bit_mask = 1ULL << config->reset,
            .mode = GPIO_MODE_OUTPUT,
        };
        gpio_config(&rst_cfg);
        gpio_set_level(config->reset, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
        gpio_set_level(config->reset, 1);
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    if (!config->bus) {
        ESP_LOGE(TAG, "%s: no I2C bus; BOOT still works", config->chip);
        return;
    }
    const i2c_device_config_t device_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CST816_ADDRESS,
        .scl_speed_hz = 400000,
    };
    esp_err_t err = i2c_master_bus_add_device(config->bus, &device_cfg, &s_touch);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s add device failed: %s", config->chip, esp_err_to_name(err));
        return;
    }
    uint8_t device_id = 0;
    err = touch_read_register(CST816_REG_DEVICE_ID, &device_id);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s probe failed: %s; BOOT still works", config->chip, esp_err_to_name(err));
        return;
    }
    err = touch_write_register(config->setup_reg, config->setup_value);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s setup register 0x%02x failed: %s", config->chip, config->setup_reg, esp_err_to_name(err));
    }
    s_touch_ready = true;
    ESP_LOGI(TAG, "%s ready: id=0x%02x", config->chip, device_id);
}

board_input_event_t board_input_poll(void)
{
    const int64_t now = esp_timer_get_time();

    const bool boot_raw = gpio_get_level(s_config.boot) == 0;
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
