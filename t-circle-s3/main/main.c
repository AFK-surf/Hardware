#include "board_audio.h"
#include "board_touch.h"
#include "connector.h"
#include "cue_voice.h"
#include "device_config.h"
#include "display.h"
#include "net_watchdog.h"
#include "playback.h"
#include "provisioning.h"
#include "wifi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/temperature_sensor.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// Holding BOOT (outside a call): a countdown shows from 3 s. Letting go before 10 s enters
// pairing mode; holding on to 10 s erases the credentials and restarts into pairing.
#define PAIRING_HOLD_US (3LL * 1000000)
#define RESET_HOLD_US (10LL * 1000000)
#define NOTICE_US (4LL * 1000000)
// "Call ended" / "Call dropped" stays this long, then the logo.
#define CALL_OVER_US (3LL * 1000000)
// A tap while the last call is still closing dials as soon as it has closed (normally < 2 s).
#define DIAL_PENDING_US (6LL * 1000000)
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

static void display_task(void *arg)
{
    (void)arg;
    display_run();
}

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
        if (cue_voice_state() != CUE_VOICE_UP || playback_ringing()) {
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
    if (cs->over_until == 0 && cue_voice_last_end() == CUE_VOICE_END_NONE && !cue_voice_can_start()) {
        // Over, but not yet known how: wait for the tone (the mic closes first).
        return;
    }
    if (cs->over_until == 0) {
        const bool dropped = cue_voice_last_end() == CUE_VOICE_END_DROPPED;
        char t[16] = "";
        if (cs->up_us != 0) {
            format_time(now - cs->up_us, t, sizeof(t));
        }
        display_show_card(dropped ? "Call dropped" : "Call ended", t, dropped ? "Tap to call again" : NULL);
        cs->over_until = now + CALL_OVER_US;
    } else if (now >= cs->over_until) {
        cs->shown = false;
        cs->over_until = 0;
        if (!provisioning_active()) {
            display_show_logo();
        }
    }
}

static void call_screen_reset(call_screen_t *cs)
{
    memset(cs, 0, sizeof(*cs));
}

static void factory_reset(void)
{
    ESP_LOGW(TAG, "factory reset");
    display_show_card("Resetting", NULL, "Erasing Wi-Fi and credentials");
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_wifi_stop();
    device_config_erase_all();
    esp_restart();
}

static void ui_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "tap the screen to call, tap again to hang up; hold BOOT 3 s to pair, 10 s to reset");
    bool boot_tracking = false;
    // BOOT already down at boot (e.g. still held after a reset): ignore it until it is let go, so
    // one long hold erases once instead of every 10 s.
    bool boot_wait_release = board_touch_boot_held();
    if (boot_wait_release) {
        ESP_LOGI(TAG, "BOOT held at boot, ignored until released");
    }
    int64_t boot_down_us = 0;
    int shown_left = -1;
    int64_t notice_until = 0;
    bool prov_was_active = provisioning_active();
    call_screen_t call_screen = {0};
    int64_t dial_pending_until = 0;

    for (;;) {
        const int64_t now = esp_timer_get_time();
        const board_touch_gesture_t gesture = board_touch_poll();
        const cue_voice_state_t state = cue_voice_state();
        // A call that is over but still closing its socket (hang-up tone playing) is not "in call":
        // a tap then dials again instead of hanging up.
        const bool ending = cue_voice_ending();
        const bool in_call = (state == CUE_VOICE_CONNECTING || state == CUE_VOICE_UP) && !ending;
        temp_log(now, in_call);
        if (gesture == BOARD_TOUCH_CLICK) {
            display_wake();
        }
        call_screen_update(&call_screen, now, in_call);

        // Start the Connector once there are credentials: at boot, or as soon as setup saves them,
        // while BLE is still up so the app can see connector_up in `status`.
        if (device_config_provisioned()) {
            connector_start();
            net_watchdog_start();
        }

        const bool prov = provisioning_active();
        if (prov_was_active && !prov) {
            cue_voice_credentials_changed();
        }
        prov_was_active = prov;

        // BOOT hold. A hold that starts during a call is the call's push-to-talk, not this.
        const bool boot = board_touch_boot_held();
        if (boot_wait_release) {
            boot_wait_release = boot;
        } else if (boot && !boot_tracking && !in_call) {
            display_wake();
            boot_tracking = true;
            boot_down_us = now;
            shown_left = -1;
        } else if (boot && boot_tracking) {
            const int64_t held = now - boot_down_us;
            if (held >= RESET_HOLD_US) {
                factory_reset();
            } else if (held >= PAIRING_HOLD_US) {
                const int left = (int)((RESET_HOLD_US - held + 999999) / 1000000);
                if (left != shown_left) {
                    char big[4];
                    snprintf(big, sizeof(big), "%d", left);
                    display_show_card("Let go to pair", big, "Keep holding to reset");
                    shown_left = left;
                }
            }
        } else if (!boot && boot_tracking) {
            boot_tracking = false;
            if (now - boot_down_us >= PAIRING_HOLD_US) {
                if (provisioning_active()) {
                    provisioning_redraw();
                } else if (provisioning_start() != ESP_OK) {
                    ESP_LOGE(TAG, "pairing mode did not start");
                    display_show_logo();
                }
            }
        }

        if (notice_until && now > notice_until) {
            notice_until = 0;
            if (!provisioning_active()) {
                display_show_logo();
            }
        }

        // One tap on a dark screen wakes it and dials: the device has one job, the ringback says a
        // call is starting, and a second tap hangs up.
        bool dial = false;
        if (dial_pending_until != 0 && (now > dial_pending_until || in_call || prov)) {
            dial_pending_until = 0;
        }
        if (dial_pending_until != 0 && !ending && cue_voice_can_start()) {
            ESP_LOGI(TAG, "dialing the tap made while the last call was closing");
            dial_pending_until = 0;
            dial = true;
        }
        if (gesture != BOARD_TOUCH_CLICK || boot_tracking) {
            // Not a tap.
        } else if (in_call) {
            cue_voice_end();
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
        } else if (!cue_voice_can_start()) {
            ESP_LOGW(TAG, "a call cannot start yet");
            if (cue_voice_close_code() == 4401) {
                display_show_card("Key rejected", NULL, "Hold BOOT 3 s and set up again in the app");
                notice_until = now + NOTICE_US;
            }
        } else if (!device_config_provisioned()) {
            ESP_LOGW(TAG, "not provisioned");
        } else if (wifi_wait(20000) != ESP_OK) {
            ESP_LOGW(TAG, "Wi-Fi not up");
            display_show_card("No Wi-Fi", NULL, "Still trying to connect");
            notice_until = esp_timer_get_time() + NOTICE_US;
        } else {
            call_screen_reset(&call_screen);
            if (cue_voice_start() != ESP_OK) {
                ESP_LOGE(TAG, "voice session did not start");
                playback_tone(PLAYBACK_TONE_DROPPED);
                display_show_card("Call failed", NULL, "Tap to try again");
                notice_until = esp_timer_get_time() + NOTICE_US;
            } else {
                call_screen_update(&call_screen, esp_timer_get_time(), true);
            }
        }

        display_keep_awake(in_call || ending || prov || boot_tracking || notice_until != 0 ||
                           call_screen.shown || dial_pending_until != 0);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void app_main(void)
{
    const size_t psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "PSRAM %u bytes", (unsigned)psram);

    ESP_ERROR_CHECK(device_config_init());
    const esp_err_t audio = board_audio_init();
    if (audio != ESP_OK) {
        ESP_LOGE(TAG, "audio init failed: %s", esp_err_to_name(audio));
    }
    board_touch_init();
    temp_init();
    playback_init();
    ESP_ERROR_CHECK(display_init());
    ESP_ERROR_CHECK(wifi_init());

    // The display task only draws into the PSRAM framebuffer, so its stack can live there too. That
    // leaves 8 KB more internal RAM for the Connector, which has to start while BLE still holds
    // its controller memory.
    if (xTaskCreateWithCaps(display_task, "display", 8192, NULL, 3, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "display task create failed");
    }

    device_config_t *cfg = calloc(1, sizeof(*cfg));
    if (cfg && device_config_get(cfg)) {
        wifi_set_power_save(false);
        wifi_connect(cfg->wifi_ssid, cfg->wifi_password, true);
    } else if (provisioning_start() != ESP_OK) {
        ESP_LOGE(TAG, "provisioning did not start");
    }
    if (cfg) {
        memset(cfg, 0, sizeof(*cfg));
        free(cfg);
    }

    if (xTaskCreate(ui_task, "ui", 6144, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "ui task create failed");
    }
}
