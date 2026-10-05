#include "board_audio.h"
#include "board_touch.h"
#include "connector.h"
#include "cue_voice.h"
#include "device_config.h"
#include "display.h"
#include "playback.h"
#include "provisioning.h"
#include "wifi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static const char *TAG = "main";

static void logo_task(void *arg)
{
    (void)arg;
    display_run_logo();
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
    int64_t boot_down_us = 0;
    int shown_left = -1;
    int64_t notice_until = 0;
    bool prov_was_active = provisioning_active();

    for (;;) {
        const int64_t now = esp_timer_get_time();
        const board_touch_gesture_t gesture = board_touch_poll();
        const cue_voice_state_t state = cue_voice_state();
        const bool in_call = state == CUE_VOICE_CONNECTING || state == CUE_VOICE_UP;
        display_set_animating(in_call);

        // Start the Connector once there are credentials: at boot, or as soon as setup saves them,
        // while BLE is still up so the app can see connector_up in `status`.
        if (device_config_provisioned()) {
            connector_start();
        }

        const bool prov = provisioning_active();
        if (prov_was_active && !prov) {
            cue_voice_credentials_changed();
        }
        prov_was_active = prov;

        // BOOT hold. A hold that starts during a call is the call's push-to-talk, not this.
        const bool boot = board_touch_boot_held();
        if (boot && !boot_tracking && !in_call) {
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

        if (gesture != BOARD_TOUCH_CLICK || boot_tracking) {
            // Not a tap.
        } else if (in_call) {
            cue_voice_end();
        } else if (prov) {
            // BLE never runs during a call. A tap leaves pairing mode on a provisioned device.
            if (device_config_provisioned()) {
                provisioning_stop();
            }
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
        } else if (cue_voice_start() != ESP_OK) {
            ESP_LOGE(TAG, "voice session did not start");
        }
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
    playback_init();
    ESP_ERROR_CHECK(display_init());
    ESP_ERROR_CHECK(wifi_init());

    if (xTaskCreate(logo_task, "logo", 8192, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "logo task create failed");
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
