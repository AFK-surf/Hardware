#include "heap_diag.h"
// Plain ESP32 DevKit: no audio/display. GPIO0 BOOT; optional active-high LED.
#include "board.h"
#include "app_controls.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <stdint.h>

static const char *TAG = "board";
static atomic_int s_pairing;
static int64_t s_saved_until; // UI task owns the timer.

void board_init(void)
{
    gpio_config_t input = {.pin_bit_mask = 1ULL << CONFIG_COMMA_BOOT_GPIO,
        .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
    ESP_ERROR_CHECK(gpio_config(&input));
    if (CONFIG_COMMA_LED_GPIO >= 0) {
        gpio_config_t led = {.pin_bit_mask = 1ULL << CONFIG_COMMA_LED_GPIO,
                            .mode = GPIO_MODE_OUTPUT};
        ESP_ERROR_CHECK(gpio_config(&led));
        gpio_set_level(CONFIG_COMMA_LED_GPIO, 0);
    }
}
void board_start_feedback(void) {}
void board_wake(void) {}
void board_show_idle(void) {}
void board_show_card(const char *label, const char *big, const char *body)
{
    ESP_LOGI(TAG, "%s%s%s%s%s", label ? label : "", big ? " / " : "", big ? big : "",
             body ? " / " : "", body ? body : "");
}
void board_pairing_state(board_pairing_state_t state)
{
    if (atomic_exchange(&s_pairing, state) != state) {
        static const char *const names[] = {"normal: LED off", "pairing: LED slow blink",
            "phone connected: LED fast blink", "saved: LED solid for 3 s"};
        ESP_LOGI(TAG, "%s", names[state]);
    }
}
void board_run_ui(void *arg)
{
    (void)arg;
    app_controls_init(gpio_get_level(CONFIG_COMMA_BOOT_GPIO) == 0);
    int previous = BOARD_PAIRING_OFF;
    for (;;) {
        int64_t now = esp_timer_get_time();
        app_poll(now, false, gpio_get_level(CONFIG_COMMA_BOOT_GPIO) == 0);
        static int64_t last_heap;
        if (now - last_heap > 5000000) {
            last_heap = now;
            comma_heap_log("ui_sample");
        }
        const int state = s_pairing;
        if (state == BOARD_PAIRING_SAVED && previous != state) s_saved_until = now + 3000000;
        previous = state;
        bool on = now < s_saved_until;
        if (!on && state == BOARD_PAIRING_ADVERTISING) on = (now / 500000) % 2 == 0;
        if (!on && state == BOARD_PAIRING_CONNECTED) on = (now / 100000) % 2 == 0;
        if (CONFIG_COMMA_LED_GPIO >= 0) gpio_set_level(CONFIG_COMMA_LED_GPIO, on);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
