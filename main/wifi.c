#include "wifi.h"

#include <string.h>

#include "secrets.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#define WIFI_UP_BIT BIT0

static const char *TAG = "wifi";
static EventGroupHandle_t s_events;
static bool s_started;
static bool s_up;

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = data;
        s_up = false;
        xEventGroupClearBits(s_events, WIFI_UP_BIT);
        ESP_LOGW(TAG, "disconnected, reason %d, reconnecting", event ? event->reason : -1);
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = data;
        ESP_LOGI(TAG, "got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_up = true;
        xEventGroupSetBits(s_events, WIFI_UP_BIT);
    }
}

bool wifi_has_credentials(void)
{
    return WIFI_SSID[0] != '\0' && strcmp(WIFI_SSID, "changeme") != 0;
}

esp_err_t wifi_start(void)
{
    if (s_started) {
        return ESP_OK;
    }
    if (!wifi_has_credentials()) {
        ESP_LOGW(TAG, "WIFI_SSID is still the placeholder");
        return ESP_ERR_INVALID_STATE;
    }
    if (strlen(WIFI_SSID) > 32 || strlen(WIFI_PASSWORD) > 64) {
        ESP_LOGE(TAG, "Wi-Fi credential does not fit the STA config");
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    s_events = xEventGroupCreate();
    if (!s_events) {
        return ESP_ERR_NO_MEM;
    }
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL));

    wifi_config_t cfg = {0};
    memcpy(cfg.sta.ssid, WIFI_SSID, strlen(WIFI_SSID));
    memcpy(cfg.sta.password, WIFI_PASSWORD, strlen(WIFI_PASSWORD));
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_connect());
    s_started = true;
    ESP_LOGI(TAG, "connecting to %s", WIFI_SSID);
    return ESP_OK;
}

bool wifi_is_up(void)
{
    return s_up;
}

esp_err_t wifi_wait(uint32_t timeout_ms)
{
    if (!s_started) {
        esp_err_t err = wifi_start();
        if (err != ESP_OK) {
            return err;
        }
    }
    if (s_up) {
        return ESP_OK;
    }
    EventBits_t bits = xEventGroupWaitBits(s_events, WIFI_UP_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms));
    return (bits & WIFI_UP_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}
