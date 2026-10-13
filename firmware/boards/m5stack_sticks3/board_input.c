// M5Stack StickS3 keys (board_input.h). No touch screen: KEY1, the big key on the front, is the
// call key and reports BOARD_INPUT_TAP when a short press is released; KEY2, on the side, is the
// board's BOOT for app_poll() (3-10 s pairing, > 10 s reset; a short press only wakes the screen).
// Both active low, internal pull-ups on (as the reference), debounced 20 ms. A key already down at
// boot (it woke the board) counts only once released: KEY1 here, KEY2 in app_poll().

#include "board_input.h"
#include "board_pins.h"

#include <stdatomic.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_timer.h"

#define DEBOUNCE_US (20 * 1000)
// KEY1 held longer than this and released is not a call key press.
#define PRESS_MAX_US (1000 * 1000)

static const char *TAG = "keys";

typedef struct {
    gpio_num_t gpio;
    bool raw;
    bool held;  // debounced
    int64_t change_us;
} key_t;

static key_t s_key1 = {.gpio = STICK_KEY1};
static key_t s_key2 = {.gpio = STICK_KEY2};
static atomic_bool s_boot_held = false;
static bool s_key1_ignore = false;
static int64_t s_key1_down_us = 0;

static void key_init(key_t *key)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << key->gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&cfg) != ESP_OK) {
        ESP_LOGE(TAG, "GPIO%d config failed", key->gpio);
    }
    key->raw = gpio_get_level(key->gpio) == 0;
    key->held = key->raw;
    key->change_us = esp_timer_get_time();
}

// Returns +1 on a debounced press, -1 on a release, 0 otherwise.
static int key_poll(key_t *key, int64_t now)
{
    const bool raw = gpio_get_level(key->gpio) == 0;
    if (raw != key->raw) {
        key->raw = raw;
        key->change_us = now;
    }
    if (now - key->change_us < DEBOUNCE_US || raw == key->held) {
        return 0;
    }
    key->held = raw;
    return raw ? 1 : -1;
}

void board_input_init(void)
{
    key_init(&s_key1);
    key_init(&s_key2);
    s_key1_ignore = s_key1.held;
    s_boot_held = s_key2.held;
    ESP_LOGI(TAG, "KEY1 GPIO%d %s, KEY2 GPIO%d %s", STICK_KEY1, s_key1.held ? "down (ignored until released)" : "up",
             STICK_KEY2, s_key2.held ? "down" : "up");
}

board_input_event_t board_input_poll(void)
{
    const int64_t now = esp_timer_get_time();
    key_poll(&s_key2, now);
    s_boot_held = s_key2.held;

    const int edge = key_poll(&s_key1, now);
    if (edge > 0) {
        s_key1_down_us = now;
    } else if (edge < 0) {
        const bool ignore = s_key1_ignore;
        s_key1_ignore = false;
        const int64_t held = now - s_key1_down_us;
        if (ignore) {
            ESP_LOGI(TAG, "KEY1 released after being held at boot, ignored");
        } else if (held <= PRESS_MAX_US) {
            return BOARD_INPUT_TAP;
        } else {
            ESP_LOGI(TAG, "KEY1 held %lld ms, not a press", (long long)(held / 1000));
        }
    }
    return BOARD_INPUT_NONE;
}

bool board_input_boot_held(void)
{
    return s_boot_held;
}
