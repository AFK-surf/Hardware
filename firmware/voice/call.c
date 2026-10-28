// SPDX-License-Identifier: Apache-2.0
#include "call.h"

#include "app_controls.h"
#include "board_input.h"
#include "comma_voice.h"
#include "device_config.h"
#include "display.h"
#include "firmware_config.h"
#include "heap_diag.h"
#include "playback.h"
#include "provisioning.h"
#include "voice_clean.h"
#include "wifi.h"
#if CONFIG_COMMA_SETTINGS_UI
#include "settings.h"
#include "wifi_switch.h"
#if CONFIG_COMMA_SERIAL_WIFI
#include "serial_console.h"
#endif
#define SETTINGS_OPEN() settings_active()
#else
#define SETTINGS_OPEN() false
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/temperature_sensor.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define NOTICE_US (4LL * 1000000)
// "Call ended" / "Call dropped" stays this long, then the logo.
#define CALL_OVER_US (3LL * 1000000)
// A tap while the last call is still closing dials as soon as it has closed (normally < 2 s).
#define DIAL_PENDING_US (6LL * 1000000)
#define WIFI_PENDING_US (20LL * 1000000)
// Taps this soon after a hang-up tap are ignored, so a double tap to hang up doesn't redial.
#define HANGUP_GUARD_US (500LL * 1000)
// "Speaking" shows at once; "Listening" only after the agent has been quiet this long, so the
// screen doesn't flicker between sentences.
#define LISTENING_AFTER_US (600LL * 1000)

// Chip temperature in the log: every 60 s idle, every 10 s in a call.
#define TEMP_IDLE_US (60LL * 1000000)
#define TEMP_CALL_US (10LL * 1000000)

static const char *TAG = "main";
static temperature_sensor_handle_t s_temp;

static void temp_init(void)
{
    const temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(20, 100);
    if (temperature_sensor_install(&cfg, &s_temp) != ESP_OK || temperature_sensor_enable(s_temp) != ESP_OK) {
        ESP_LOGW(TAG, "temperature sensor unavailable");
        s_temp = NULL;
    }
}

static void temp_log(int64_t now, bool in_call)
{
    static int64_t last;
    if (!s_temp || (last != 0 && now - last < (in_call ? TEMP_CALL_US : TEMP_IDLE_US))) {
        return;
    }
    last = now;
    float c = 0;
    if (temperature_sensor_get_celsius(s_temp, &c) == ESP_OK) {
        ESP_LOGI(TAG, "temp chip=%.1f C %s uptime_s=%lld", c, in_call ? "call" : "idle", (long long)(now / 1000000));
    }
}

#if CONFIG_COMMA_HEAP_DIAGNOSTICS
// Test builds only: internal and PSRAM heap, and the stack left in the voice and screen tasks, every
// 10 s in a call and every 60 s otherwise. (The uplink task logs its own in the voice stats.)
static void diag_sample(int64_t now, bool in_call)
{
    static int64_t last;
    if (last != 0 && now - last < (in_call ? 10LL : 60LL) * 1000000) {
        return;
    }
    last = now;
    comma_heap_log(in_call ? "ui_call" : "ui_idle");
    static const char *const tasks[] = {"display", "play", "sender"};
    unsigned left[3];
    for (int i = 0; i < 3; ++i) {
        TaskHandle_t task = xTaskGetHandle(tasks[i]);
        left[i] = task ? (unsigned)uxTaskGetStackHighWaterMark(task) : 0;
    }
    ESP_LOGI("heap", "psram free=%u largest=%u min=%u stack_free display=%u play=%u sender=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM), left[0], left[1], left[2]);
}
#endif

// The call screen redraws only when its text changes: Calling, then the call time (once a second)
// with Speaking or Listening, then how the call ended.
typedef struct {
    bool shown;          // a call screen (or its end card) is up
    int64_t up_us;       // when the agent first spoke after the ringback; 0 until then
    int64_t quiet_us;    // when the agent went quiet; 0 while speaking
    bool speaking;
    int64_t over_until;  // end card shown until then
} call_screen_t;

static void format_time(int64_t us, char *out, size_t len)
{
    const int s = (int)(us / 1000000);
    snprintf(out, len, "%d:%02d", s / 60, s % 60);
}

static void call_screen_update(call_screen_t *cs, int64_t now, bool live)
{
    if (live) {
        cs->shown = true;
        cs->over_until = 0;
        if (comma_voice_state() != COMMA_VOICE_UP || playback_ringing()) {
            display_show_card("Calling", NULL, "Comma");
            return;
        }
        if (cs->up_us == 0) {
            cs->up_us = now;
        }
        if (playback_speaking()) {
            cs->speaking = true;
            cs->quiet_us = 0;
        } else if (cs->quiet_us == 0) {
            cs->quiet_us = now;
        } else if (now - cs->quiet_us >= LISTENING_AFTER_US) {
            cs->speaking = false;
        }
        char t[16];
        format_time(now - cs->up_us, t, sizeof(t));
        display_show_card("Comma", t, cs->speaking ? "Speaking" : "Listening");
        return;
    }
    if (!cs->shown) {
        return;
    }
    if (cs->over_until == 0 && comma_voice_last_end() == COMMA_VOICE_END_NONE && !comma_voice_can_start()) {
        // Over, but not yet known how: wait for the tone (the mic closes first).
        return;
    }
    if (cs->over_until == 0) {
        const bool dropped = comma_voice_last_end() == COMMA_VOICE_END_DROPPED;
        char t[16] = "";
        if (cs->up_us != 0) {
            format_time(now - cs->up_us, t, sizeof(t));
        }
        display_show_card(dropped ? "Call dropped" : "Call ended", t, dropped ? "Tap to call again" : NULL);
        cs->over_until = now + CALL_OVER_US;
    } else if (now >= cs->over_until) {
        cs->shown = false;
        cs->over_until = 0;
        if (!provisioning_active() && !SETTINGS_OPEN()) {
            display_show_logo();
        }
    }
}

static void call_screen_reset(call_screen_t *cs)
{
    memset(cs, 0, sizeof(*cs));
}

#if CONFIG_COMMA_BOOT_PRESS_CALLS
// A short BOOT press is the call key, like a tap. A press is told apart from the start of a hold only
// when BOOT is released: before APP_PAIRING_HOLD_US it was a press; from then on it was app_poll's
// pairing or reset hold, and no call key. A key already down at boot counts only once released.
typedef struct {
    bool down;
    bool ignore;
    int64_t down_us;
} boot_press_t;

static bool boot_pressed(boot_press_t *bp, int64_t now, bool boot)
{
    bool press = false;
    if (boot && !bp->down) {
        bp->down_us = now;
    } else if (!boot && bp->down) {
        const int64_t held = now - bp->down_us;
        press = !bp->ignore && held < APP_PAIRING_HOLD_US;
        if (press) {
            ESP_LOGI(TAG, "BOOT press (%lld ms): call key", (long long)(held / 1000));
        }
        bp->ignore = false;
    }
    bp->down = boot;
    return press;
}
#endif

void call_run_ui(const char *help)
{
    ESP_LOGI(TAG, "%s", help);
    app_controls_init(board_input_boot_held());
#if CONFIG_COMMA_SERIAL_WIFI
    serial_console_start();
#endif
    int64_t notice_until = 0;
    bool prov_was_active = provisioning_active();
    call_screen_t call_screen = {0};
    int64_t dial_pending_until = 0;
    int64_t wifi_pending_until = 0;
    int64_t hung_up_at = 0;
#if CONFIG_COMMA_BOOT_PRESS_CALLS
    boot_press_t boot_press = {.down = board_input_boot_held(), .ignore = board_input_boot_held()};
#endif

    for (;;) {
        const int64_t now = esp_timer_get_time();
        const board_input_event_t gesture = board_input_poll();
        bool call_key = gesture == BOARD_INPUT_TAP;
#if CONFIG_COMMA_BOOT_PRESS_CALLS
        const bool boot_key = boot_pressed(&boot_press, now, board_input_boot_held());
        call_key = boot_key || call_key;
#endif
        const comma_voice_state_t state = comma_voice_state();
        // A call that is over but still closing its socket (hang-up tone playing) is not "in call":
        // a tap then dials again instead of hanging up.
        const bool ending = comma_voice_ending();
        const bool in_call = (state == COMMA_VOICE_CONNECTING || state == COMMA_VOICE_UP) && !ending;
        temp_log(now, in_call);
#if CONFIG_COMMA_HEAP_DIAGNOSTICS
        diag_sample(now, in_call);
#endif
        if (call_key) {
            display_wake();
        }
        call_screen_update(&call_screen, now, in_call);

        const bool boot_tracking = app_poll(now, in_call, board_input_boot_held());
        const bool prov = provisioning_active();
        if (prov_was_active && !prov) {
            comma_voice_credentials_changed();
        }
        prov_was_active = prov;

#if CONFIG_COMMA_SETTINGS_UI
        // The on-screen settings. A long press on the idle screen opens them; while they are open they
        // take the touch input and the short BOOT press (Back), so nothing dials, including the tap
        // or press that closes them. A call or pairing mode closes them.
        wifi_switch_poll();
#if CONFIG_COMMA_SERIAL_WIFI
        serial_console_poll();
#endif
        const bool settings_were_open = settings_active();
        if (settings_were_open && (in_call || ending || prov)) {
            settings_close();
        } else if (settings_were_open) {
#if CONFIG_COMMA_BOOT_PRESS_CALLS
            settings_poll(now, gesture, boot_key, boot_tracking);
#else
            settings_poll(now, gesture, false, boot_tracking);
#endif
        } else if (gesture == BOARD_INPUT_LONG_PRESS && !in_call && !ending && !prov && !boot_tracking &&
                   dial_pending_until == 0 && wifi_pending_until == 0 && device_config_provisioned()) {
            call_screen_reset(&call_screen);
            notice_until = 0;
            settings_open(now);
            settings_poll(now, BOARD_INPUT_NONE, false, false);
        }
        if (settings_were_open || settings_active()) {
            call_key = false;
        }
#endif

        if (notice_until && now > notice_until) {
            notice_until = 0;
            if (!prov && !boot_tracking && !in_call && !SETTINGS_OPEN()) {
                display_show_logo();
            }
        }

        // One tap on a dark screen wakes it and dials: the device has one job, the ringback says a
        // call is starting, and a second tap hangs up.
        bool dial = false;
        if (wifi_pending_until != 0) {
            if (in_call || prov || boot_tracking || !device_config_provisioned() || call_key) {
                wifi_pending_until = 0;
                if (!in_call && !prov) display_show_logo();
                call_key = false;  // a second tap cancels, without starting another wait
            } else if (now >= wifi_pending_until) {
                wifi_pending_until = 0;
                display_show_card("No Wi-Fi", NULL, "Still trying to connect");
                notice_until = now + NOTICE_US;
            } else if (wifi_is_up() && comma_voice_can_start()) {
                wifi_pending_until = 0;
                dial = true;
            }
        }
        if (dial_pending_until != 0 && (now > dial_pending_until || in_call || prov || boot_tracking)) {
            dial_pending_until = 0;
        }
        if (dial_pending_until != 0 && !ending && comma_voice_can_start()) {
            ESP_LOGI(TAG, "dialing the tap made while the last call was closing");
            dial_pending_until = 0;
            dial = true;
        }
        if (!call_key || boot_tracking) {
            // Not a tap.
        } else if (hung_up_at != 0 && now - hung_up_at < HANGUP_GUARD_US) {
            ESP_LOGI(TAG, "tap %lld ms after hanging up, ignored", (long long)((now - hung_up_at) / 1000));
        } else if (in_call) {
            comma_voice_end();
            hung_up_at = now;
        } else if (ending) {
            ESP_LOGI(TAG, "tap while the last call is closing: dialing once it has closed");
            dial_pending_until = now + DIAL_PENDING_US;
        } else if (prov) {
            // BLE never runs during a call. A tap leaves pairing mode on a provisioned device.
            if (device_config_provisioned()) {
                provisioning_stop();
            }
        } else {
            dial = true;
        }

        if (!dial) {
            // Nothing to dial.
        } else if (!comma_voice_can_start()) {
            ESP_LOGW(TAG, "a call cannot start yet");
            if (comma_voice_close_code() == 4401) {
                display_show_card("Key rejected", NULL, "Hold BOOT 3 s and set up again in the app");
                notice_until = now + NOTICE_US;
            }
        } else if (!device_config_provisioned()) {
            ESP_LOGW(TAG, "not provisioned");
#if CONFIG_COMMA_SETTINGS_UI
        } else if (wifi_is_off()) {
            display_show_card("Wi-Fi is off", NULL, "Hold the screen to open Settings");
            notice_until = now + NOTICE_US;
#endif
        } else if (!wifi_is_up()) {
            // Keep polling input while Wi-Fi reconnects: pairing/reset holds and cancellation
            // must work even when the stored access point is unavailable.
            wifi_pending_until = now + WIFI_PENDING_US;
            notice_until = 0;
            call_screen_reset(&call_screen);
            display_show_card("Waiting for Wi-Fi", NULL, "Tap to cancel");
        } else {
            call_screen_reset(&call_screen);
            if (comma_voice_start() != ESP_OK) {
                ESP_LOGE(TAG, "voice session did not start");
                playback_tone(PLAYBACK_TONE_DROPPED);
                display_show_card("Call failed", NULL, "Tap to try again");
                notice_until = esp_timer_get_time() + NOTICE_US;
            } else {
                call_screen_update(&call_screen, esp_timer_get_time(), true);
            }
        }

        display_keep_awake(in_call || ending || prov || boot_tracking || notice_until != 0 ||
                           call_screen.shown || dial_pending_until != 0 || wifi_pending_until != 0 ||
                           SETTINGS_OPEN());
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void call_voice_clean_init(void)
{
    // Before the display and Wi-Fi take their share of internal RAM; the AFE goes in PSRAM anyway.
    const esp_err_t clean = voice_clean_init();
    if (clean != ESP_OK) {
        ESP_LOGE(TAG, "noise suppression unavailable: %s; calls send the raw mic", esp_err_to_name(clean));
    }
}

void call_init(void)
{
    temp_init();
    playback_init();
}
