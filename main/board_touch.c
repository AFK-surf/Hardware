// LilyGO T-Circle-S3 V1.1. CST816D on I2C0, BOOT on GPIO0.
// The screen is polled. FingerNum maps both press and release without an IRQ line.

#include "board_touch.h"

#include <stdatomic.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"

#define T_CIRCLE_TOUCH_SDA GPIO_NUM_11
#define T_CIRCLE_TOUCH_SCL GPIO_NUM_14
#define T_CIRCLE_TOUCH_ADDRESS 0x15
#define T_CIRCLE_BOOT_GPIO GPIO_NUM_0

#define CST816D_REG_FINGER_COUNT 0x02
#define CST816D_REG_DEVICE_ID 0xA7
#define CST816D_REG_IRQ_MODE 0xFA
#define CST816D_IRQ_PERIODIC 0x40

#define TAP_MAX_US (280 * 1000)
#define DOUBLE_GAP_US (450 * 1000)
#define BOOT_DEBOUNCE_US (20 * 1000)

static const char *TAG = "touch";

static i2c_master_bus_handle_t s_i2c_bus = NULL;
static i2c_master_dev_handle_t s_touch = NULL;
static bool s_touch_ready = false;

static bool s_screen_raw = false;
static atomic_bool s_screen_held = false;
static atomic_bool s_boot_held = false;
static bool s_boot_raw = false;
static int64_t s_boot_change_us = 0;

typedef enum {
    PHASE_IDLE = 0,
    PHASE_DOWN,
    PHASE_WAIT_SECOND,
    PHASE_SECOND,
} phase_t;

static phase_t s_phase = PHASE_IDLE;
static int64_t s_down_us = 0;
static int64_t s_up_us = 0;

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
    if (touch_read_register(CST816D_REG_FINGER_COUNT, &fingers) != ESP_OK) {
        return false;
    }
    *down = (fingers & 0x0f) > 0;
    return true;
}

void board_touch_init(void)
{
    gpio_config_t boot_cfg = {
        .pin_bit_mask = 1ULL << T_CIRCLE_BOOT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&boot_cfg);
    s_boot_raw = gpio_get_level(T_CIRCLE_BOOT_GPIO) == 0;
    s_boot_held = s_boot_raw;
    s_boot_change_us = esp_timer_get_time();

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = T_CIRCLE_TOUCH_SDA,
        .scl_io_num = T_CIRCLE_TOUCH_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CST816D I2C bus init failed: %s", esp_err_to_name(err));
        return;
    }

    i2c_device_config_t device_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = T_CIRCLE_TOUCH_ADDRESS,
        .scl_speed_hz = 400000,
    };
    err = i2c_master_bus_add_device(s_i2c_bus, &device_cfg, &s_touch);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CST816D add device failed: %s", esp_err_to_name(err));
        return;
    }

    uint8_t device_id = 0;
    err = touch_read_register(CST816D_REG_DEVICE_ID, &device_id);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "CST816D probe failed: %s", esp_err_to_name(err));
        return;
    }

    err = touch_write_register(CST816D_REG_IRQ_MODE, CST816D_IRQ_PERIODIC);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "CST816D periodic mode failed: %s", esp_err_to_name(err));
    }
    s_touch_ready = true;
    ESP_LOGI(TAG, "CST816D ready: id=0x%02x", device_id);
}

board_touch_gesture_t board_touch_poll(void)
{
    const int64_t now = esp_timer_get_time();

    const bool boot_raw = gpio_get_level(T_CIRCLE_BOOT_GPIO) == 0;
    if (boot_raw != s_boot_raw) {
        s_boot_raw = boot_raw;
        s_boot_change_us = now;
    }
    if (now - s_boot_change_us >= BOOT_DEBOUNCE_US) {
        s_boot_held = boot_raw;
    }

    bool screen = s_screen_raw;
    if (!read_screen(&screen)) {
        return BOARD_TOUCH_NONE;
    }

    const bool pressed = screen && !s_screen_raw;
    const bool released = !screen && s_screen_raw;
    s_screen_raw = screen;
    s_screen_held = screen;

    if (pressed) {
        const bool overdue_click = s_phase == PHASE_WAIT_SECOND && now - s_up_us > DOUBLE_GAP_US;
        s_down_us = now;
        s_phase = (s_phase == PHASE_WAIT_SECOND && !overdue_click) ? PHASE_SECOND : PHASE_DOWN;
        s_screen_raw = screen;
        s_screen_held = screen;
        if (overdue_click) {
            return BOARD_TOUCH_CLICK;
        }
    }

    if (released) {
        const int64_t held_us = now - s_down_us;
        if (s_phase == PHASE_SECOND) {
            s_phase = PHASE_IDLE;
            if (held_us <= TAP_MAX_US) {
                return BOARD_TOUCH_DOUBLE;
            }
        } else if (s_phase == PHASE_DOWN) {
            if (held_us <= TAP_MAX_US) {
                s_phase = PHASE_WAIT_SECOND;
                s_up_us = now;
            } else {
                s_phase = PHASE_IDLE;
            }
        }
    }

    if (s_phase == PHASE_WAIT_SECOND && !screen && now - s_up_us > DOUBLE_GAP_US) {
        s_phase = PHASE_IDLE;
        return BOARD_TOUCH_CLICK;
    }
    return BOARD_TOUCH_NONE;
}

bool board_touch_screen_held(void)
{
    return s_screen_held;
}

bool board_touch_boot_held(void)
{
    return s_boot_held;
}

bool board_touch_talk_held(void)
{
    return s_screen_held || s_boot_held;
}
