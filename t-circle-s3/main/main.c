#include "board_audio.h"
#include "board_touch.h"
#include "cue_voice.h"
#include "display.h"
#include "playback.h"
#include "wifi.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "main";

static void logo_task(void *arg)
{
    (void)arg;
    display_run_logo();
}

static void ui_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "tap the screen to call, tap again to hang up");
    for (;;) {
        board_touch_gesture_t gesture = board_touch_poll();
        const cue_voice_state_t state = cue_voice_state();
        display_set_animating(state == CUE_VOICE_CONNECTING || state == CUE_VOICE_UP);
        if (gesture != BOARD_TOUCH_CLICK) {
            // Not a tap.
        } else if (state == CUE_VOICE_CONNECTING || state == CUE_VOICE_UP) {
            cue_voice_end();
        } else if (!cue_voice_can_start()) {
            ESP_LOGW(TAG, "a call cannot start yet");
        } else if (!wifi_has_credentials()) {
            ESP_LOGE(TAG, "WIFI_SSID is still the placeholder");
        } else if (wifi_wait(20000) != ESP_OK) {
            ESP_LOGW(TAG, "Wi-Fi not up");
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

    const esp_err_t audio = board_audio_init();
    if (audio != ESP_OK) {
        ESP_LOGE(TAG, "audio init failed: %s", esp_err_to_name(audio));
    }
    board_touch_init();
    playback_init();
    ESP_ERROR_CHECK(display_init());
    if (wifi_has_credentials()) {
        wifi_start();
    } else {
        ESP_LOGW(TAG, "Wi-Fi credentials are not set");
    }

    if (xTaskCreate(logo_task, "logo", 8192, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "logo task create failed");
    }
    if (xTaskCreate(ui_task, "ui", 6144, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "ui task create failed");
    }
}
