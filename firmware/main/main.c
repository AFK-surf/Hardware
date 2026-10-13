// SPDX-License-Identifier: Apache-2.0
#include "heap_diag.h"
#include "memory_caps.h"
#include "app_controls.h"
#include "board.h"
#include "connector.h"
#include "connector_json.h"
#include "device_config.h"
#include "net_watchdog.h"
#include "provisioning.h"
#include "wifi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define PAIRING_HOLD_US APP_PAIRING_HOLD_US
#define RESET_HOLD_US APP_RESET_HOLD_US
static const char *TAG = "main";
static bool boot_tracking, boot_wait_release;
static int64_t boot_down_us;
static int shown_left;

void app_controls_init(bool boot)
{
    boot_tracking = false;
    boot_wait_release = boot;
    if (boot) ESP_LOGI(TAG, "BOOT held at boot, ignored until released");
}

static void factory_reset(void)
{
    ESP_LOGW(TAG, "factory reset");
    board_show_card("Resetting", NULL, "Erasing Wi-Fi and credentials");
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_wifi_stop();
    device_config_erase_all();
    esp_restart();
}

bool app_poll(int64_t now, bool in_call, bool boot)
{
    // Start the Connector once there are credentials: at boot, or as soon as setup saves them,
    // while BLE is still up so the app can see connector_up in `status`.
    if (device_config_provisioned()) {
        connector_start();
        net_watchdog_start();
    }

    // BOOT hold. A hold that starts during a call is the call's push-to-talk, not this.
    if (boot_wait_release) {
        boot_wait_release = boot;
    } else if (boot && !boot_tracking && !in_call) {
        board_wake();
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
                board_show_card("Let go to pair", big, "Keep holding to reset");
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
                board_show_idle();
            }
        }
    }

    return boot_tracking;
}

void app_main(void)
{
    connector_json_init();
    ESP_LOGI(TAG, "board %s; PSRAM %u bytes", CONFIG_COMMA_BOARD_ID,
             (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
    ESP_ERROR_CHECK(device_config_init());
    board_init();
    comma_heap_log("boot_before_wifi");
    ESP_ERROR_CHECK(wifi_init());
    board_start_feedback();
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
    if (xTaskCreate(board_run_ui, "ui", 6144, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "ui task create failed");
    }
}
