// SPDX-License-Identifier: Apache-2.0
// Screen shared by the display boards. Idle screen is the Comma logo, still; calls and setup
// show text cards. The screen redraws only when what it shows changes, and sleeps (light off, panel
// in SLPIN) after DISPLAY_SLEEP_US without activity. The panel itself is the board's
// (board_display.h).

#include "display.h"

#include <stdatomic.h>
#include <string.h>

#include "board.h"
#include "board_display.h"
#include "comma_logo.h"
#include "memory_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ui_card.h"
#if CONFIG_COMMA_SETTINGS_UI
#include "esp_heap_caps.h"
#include "ui_view.h"
#endif

#define DISPLAY_SLEEP_US (15LL * 1000000)
#define POLL_MS 20
// A frame that did not reach the panel is drawn again after this.
#define RETRY_MS 500

_Static_assert(COMMA_LOGO_W * COMMA_LOGO_H % 2 == 0, "logo is packed two pixels per byte");

extern const uint8_t logo_start[] asm("_binary_comma_logo_bin_start");
extern const uint8_t logo_end[] asm("_binary_comma_logo_bin_end");

static const char *TAG = "display";

static uint16_t s_lut[16];
static bool s_ready = false;

static void build_lut(void)
{
    for (int i = 0; i < 16; ++i) {
        uint8_t v = (uint8_t)(i * 17);
        uint16_t c = ((uint16_t)(v & 0xf8) << 8) | ((uint16_t)(v & 0xfc) << 3) | (v >> 3);
        s_lut[i] = (uint16_t)((c << 8) | (c >> 8));
    }
}

static void expand_logo(uint16_t *dst)
{
    for (int i = 0; i < COMMA_LOGO_BYTES; ++i) {
        uint8_t b = logo_start[i];
        dst[2 * i] = s_lut[b >> 4];
        dst[2 * i + 1] = s_lut[b & 0x0f];
    }
}

esp_err_t display_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }
    if (logo_end - logo_start != COMMA_LOGO_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    build_lut();
    const esp_err_t err = board_display_init();
    if (err != ESP_OK) {
        return err;
    }
    s_ready = true;
    return ESP_OK;
}

// ---- What to show, and when to sleep ----------------------------------------------------------

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static ui_card_t s_card;
static bool s_card_on;
static atomic_bool s_dirty;
static atomic_bool s_wake_req;
static atomic_bool s_keep_awake;
static _Atomic int64_t s_activity_us;
static atomic_bool s_asleep;
#if CONFIG_COMMA_SETTINGS_UI
// A view replaces the card while s_view_on. Two copies in PSRAM: the one asked for, and the one the
// display task draws from.
static ui_view_t *s_view, *s_view_drawn;
static bool s_view_on;
static bool s_drawing_view;
#endif

static void set_content(bool card_on, const char *label, const char *big, const char *body)
{
    ui_card_t card = {0};
    if (card_on) {
        strlcpy(card.label, label ? label : "", sizeof(card.label));
        strlcpy(card.big, big ? big : "", sizeof(card.big));
        strlcpy(card.body, body ? body : "", sizeof(card.body));
    }
    taskENTER_CRITICAL(&s_mux);
#if CONFIG_COMMA_SETTINGS_UI
    if (s_view_on) {
        s_view_on = false;
        s_dirty = true;
    }
#endif
    // Same content as on screen: no redraw, but it still counts as activity.
    if (card_on != s_card_on || memcmp(&card, &s_card, sizeof(card)) != 0) {
        s_card_on = card_on;
        s_card = card;
        s_dirty = true;
    }
    taskEXIT_CRITICAL(&s_mux);
    display_wake();
}

void display_show_card(const char *label, const char *big, const char *body)
{
    set_content(true, label, big, body);
}

void display_show_logo(void)
{
    set_content(false, NULL, NULL, NULL);
}

#if CONFIG_COMMA_SETTINGS_UI
void display_show_view(const ui_view_t *view)
{
    if (!s_view) {
        ui_view_t *a = heap_caps_calloc(1, sizeof(*a), COMMA_BUFFER_CAPS);
        ui_view_t *b = heap_caps_calloc(1, sizeof(*b), COMMA_BUFFER_CAPS);
        if (!a || !b) {
            heap_caps_free(a);
            heap_caps_free(b);
            ESP_LOGE(TAG, "no memory for a view");
            return;
        }
        s_view_drawn = b;
        s_view = a;
    }
    taskENTER_CRITICAL(&s_mux);
    if (!s_view_on || memcmp(view, s_view, sizeof(*view)) != 0) {
        *s_view = *view;
        s_view_on = true;
        s_dirty = true;
    }
    taskEXIT_CRITICAL(&s_mux);
    display_wake();
}
#endif

void display_wake(void)
{
    s_activity_us = esp_timer_get_time();
    s_wake_req = true;
}

void display_keep_awake(bool on)
{
    if (on && !s_keep_awake) {
        s_activity_us = esp_timer_get_time();
    }
    s_keep_awake = on;
    if (on) {
        s_wake_req = true;
    }
}

bool display_asleep(void)
{
    return s_asleep;
}

static bool draw(bool card_on, const ui_card_t *card)
{
    uint16_t *fb = board_display_frame();
    if (!fb) {
        ESP_LOGE(TAG, "framebuffer still busy, frame skipped");
        return false;
    }
#if CONFIG_COMMA_SETTINGS_UI
    if (s_drawing_view) {
        ui_view_render(s_view_drawn, fb, s_lut);
    } else
#endif
    if (card_on) {
        ui_card_render(card, fb, s_lut);
    } else {
        expand_logo(fb);
    }
    const esp_err_t err = board_display_flush();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "frame not sent: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

static void panel_sleep(void)
{
    board_display_sleep();
    s_asleep = true;
    ESP_LOGI(TAG, "screen off");
}

static void panel_wake(void)
{
    board_display_wake();
    s_asleep = false;
    ESP_LOGI(TAG, "screen on");
}

void display_run(void)
{
    s_activity_us = esp_timer_get_time();
    bool lit = false;
    bool on_pending = false;
    for (;;) {
        const bool wake = atomic_exchange(&s_wake_req, false);
        const bool woke = wake && s_asleep;
        if (woke) {
            panel_wake();
            on_pending = true;
        }
        if (woke || s_dirty || !lit) {
            ui_card_t card;
            bool card_on;
            taskENTER_CRITICAL(&s_mux);
            card = s_card;
            card_on = s_card_on;
#if CONFIG_COMMA_SETTINGS_UI
            s_drawing_view = s_view_on;
            if (s_view_on) {
                *s_view_drawn = *s_view;
            }
#endif
            s_dirty = false;
            taskEXIT_CRITICAL(&s_mux);
            if (!s_asleep) {
                // Panel RAM survives SLPIN, but a wake redraws anyway: cheap, and it can't show a
                // stale frame. A change while asleep waits for the wake (s_dirty stays set).
                if (!draw(card_on, &card)) {
                    // Keep it pending and leave the light as it is: a lost frame never stalls the loop.
                    s_dirty = true;
                    vTaskDelay(pdMS_TO_TICKS(RETRY_MS));
                    continue;
                }
                if (on_pending) {
                    board_display_on();
                    on_pending = false;
                }
                if (!lit || woke) {
                    board_display_light(true);
                    lit = true;
                }
            } else {
                s_dirty = true;
            }
        }
        if (!s_asleep && !s_keep_awake && esp_timer_get_time() - s_activity_us >= DISPLAY_SLEEP_US) {
            panel_sleep();
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

// ---- board.h for display boards ---------------------------------------------------------------

static void display_task(void *arg)
{
    (void)arg;
    display_run();
}

void board_start_feedback(void)
{
    // The display task only draws into the framebuffer and hands it to the panel, so its stack can
    // live in PSRAM. That leaves 8 KB more internal RAM for the Connector, which has to start while
    // BLE still holds its controller memory.
    if (xTaskCreateWithCaps(display_task, "display", 8192, NULL, 3, NULL, COMMA_BUFFER_CAPS) != pdPASS) {
        ESP_LOGE("main", "display task create failed");
    }
}

void board_show_card(const char *label, const char *big, const char *body)
{
    display_show_card(label, big, body);
}
void board_show_idle(void) { display_show_logo(); }
void board_wake(void) { display_wake(); }
void board_pairing_state(board_pairing_state_t state) { (void)state; }
