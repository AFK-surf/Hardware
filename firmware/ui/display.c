#include "memory_caps.h"
// Round screen shared by the display boards. Idle screen is the Cue logo, still; calls and setup
// show text cards. The screen redraws only when what it shows changes, and sleeps (light off, panel
// in SLPIN) after DISPLAY_SLEEP_US without activity. The panel itself is the board's
// (board_display.h).

#include "display.h"

#include <stdatomic.h>
#include <string.h>

#include "board.h"
#include "board_display.h"
#include "cue_logo.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ui_card.h"

#define DISPLAY_SLEEP_US (15LL * 1000000)
#define POLL_MS 20
// A frame that did not reach the panel is drawn again after this.
#define RETRY_MS 500

_Static_assert(CUE_LOGO_SIZE == UI_CARD_SIZE, "logo size must match panel");

extern const uint8_t logo_start[] asm("_binary_cue_logo_bin_start");
extern const uint8_t logo_end[] asm("_binary_cue_logo_bin_end");

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
    for (int i = 0; i < CUE_LOGO_BYTES; ++i) {
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
    if (logo_end - logo_start != CUE_LOGO_BYTES) {
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

static void set_content(bool card_on, const char *label, const char *big, const char *body)
{
    ui_card_t card = {0};
    if (card_on) {
        strlcpy(card.label, label ? label : "", sizeof(card.label));
        strlcpy(card.big, big ? big : "", sizeof(card.big));
        strlcpy(card.body, body ? body : "", sizeof(card.body));
    }
    taskENTER_CRITICAL(&s_mux);
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
