#include "wifi.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#define WIFI_UP_BIT BIT0
// A provisioning trial gives up after this many failed attempts.
#define TRIAL_ATTEMPTS 3

typedef enum {
    MODE_NONE = 0,  // disconnects are ignored
    MODE_TRIAL,
    MODE_KEEP,
} connect_mode_t;

static const char *TAG = "wifi";
static EventGroupHandle_t s_events;
static esp_netif_t *s_netif;
static bool s_started;
static atomic_bool s_up;
static atomic_int s_mode = MODE_NONE;
static atomic_int s_result = WIFI_RESULT_IDLE;
static atomic_int s_reason;
// Automatic reconnects of a MODE_KEEP connection are held: by provisioning while an app is
// connected, and by a scan while it runs. A connect attempt in flight blocks scanning, and an
// unreachable network would otherwise be retried forever.
static atomic_bool s_hold;
static atomic_bool s_scanning;
static int s_attempts;
static int s_auth_failures;
static int s_not_found;

static bool reason_is_auth(int reason)
{
    return reason == WIFI_REASON_AUTH_FAIL || reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
           reason == WIFI_REASON_HANDSHAKE_TIMEOUT || reason == WIFI_REASON_MIC_FAILURE;
}

static bool reason_is_not_found(int reason)
{
    return reason == WIFI_REASON_NO_AP_FOUND || reason == WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY ||
           reason == WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD ||
           reason == WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD;
}

static void on_trial_disconnect(int reason)
{
    ++s_attempts;
    if (reason_is_auth(reason)) {
        ++s_auth_failures;
    } else if (reason_is_not_found(reason)) {
        ++s_not_found;
    }
    wifi_result_t result = WIFI_RESULT_CONNECTING;
    if (s_auth_failures >= 2) {
        result = WIFI_RESULT_WRONG_PASSWORD;
    } else if (s_not_found >= 2) {
        result = WIFI_RESULT_NOT_FOUND;
    } else if (s_attempts >= TRIAL_ATTEMPTS) {
        result = WIFI_RESULT_FAILED;
    }
    if (result != WIFI_RESULT_CONNECTING) {
        s_mode = MODE_NONE;
        s_result = result;
        ESP_LOGW(TAG, "trial failed after %d attempts, reason %d", s_attempts, reason);
        return;
    }
    esp_wifi_connect();
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = data;
        const int reason = event ? event->reason : -1;
        s_up = false;
        s_reason = reason;
        xEventGroupClearBits(s_events, WIFI_UP_BIT);
        const connect_mode_t mode = s_mode;
        if (mode == MODE_KEEP) {
            if (s_result == WIFI_RESULT_UP) {
                s_result = WIFI_RESULT_CONNECTING;
            }
            if (s_hold || s_scanning) {
                ESP_LOGW(TAG, "disconnected, reason %d, reconnects held", reason);
                return;
            }
            ESP_LOGW(TAG, "disconnected, reason %d, reconnecting", reason);
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_wifi_connect();
        } else if (mode == MODE_TRIAL) {
            ESP_LOGW(TAG, "disconnected, reason %d", reason);
            on_trial_disconnect(reason);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = data;
        ESP_LOGI(TAG, "got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_up = true;
        s_result = WIFI_RESULT_UP;
        xEventGroupSetBits(s_events, WIFI_UP_BIT);
    }
}

esp_err_t wifi_init(void)
{
    if (s_started) {
        return ESP_OK;
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    // Credentials live in device_config; keep the driver from storing a second copy.
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    s_events = xEventGroupCreate();
    if (!s_events) {
        return ESP_ERR_NO_MEM;
    }
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_started = true;
    return ESP_OK;
}

void wifi_keep_retrying(void)
{
    if (s_mode == MODE_TRIAL) {
        s_mode = MODE_KEEP;
    }
}

void wifi_hold_reconnect(bool hold)
{
    if (s_hold == hold) {
        return;
    }
    s_hold = hold;
    if (!s_started || s_mode != MODE_KEEP || s_up) {
        return;
    }
    if (hold) {
        ESP_LOGI(TAG, "reconnects held");
        esp_wifi_disconnect();  // cancel an attempt in flight
    } else {
        ESP_LOGI(TAG, "reconnects resumed");
        esp_wifi_connect();
    }
}

void wifi_disconnect(void)
{
    s_mode = MODE_NONE;
    if (s_started) {
        esp_wifi_disconnect();
    }
    s_up = false;
    s_result = WIFI_RESULT_IDLE;
    if (s_events) {
        xEventGroupClearBits(s_events, WIFI_UP_BIT);
    }
}

esp_err_t wifi_connect(const char *ssid, const char *password, bool keep_retrying)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!ssid || !ssid[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!password) {
        password = "";
    }
    const size_t ssid_len = strlen(ssid);
    const size_t pass_len = strlen(password);
    if (ssid_len > 32 || pass_len > 64 || (pass_len > 0 && pass_len < 8)) {
        ESP_LOGE(TAG, "Wi-Fi credential does not fit the STA config");
        return ESP_ERR_INVALID_SIZE;
    }

    // Drop any current link first. Its disconnect event lands while the mode is NONE.
    if (s_mode != MODE_NONE || s_up) {
        wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    wifi_config_t cfg = {0};
    memcpy(cfg.sta.ssid, ssid, ssid_len);
    memcpy(cfg.sta.password, password, pass_len);
    cfg.sta.threshold.authmode = pass_len ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) {
        return err;
    }
    s_attempts = 0;
    s_auth_failures = 0;
    s_not_found = 0;
    s_reason = 0;
    s_result = WIFI_RESULT_CONNECTING;
    s_mode = keep_retrying ? MODE_KEEP : MODE_TRIAL;
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        s_mode = MODE_NONE;
        s_result = WIFI_RESULT_FAILED;
        return err;
    }
    ESP_LOGI(TAG, "connecting to %s%s", ssid, keep_retrying ? "" : " (trial)");
    return ESP_OK;
}

wifi_result_t wifi_result(void)
{
    return s_result;
}

int wifi_last_reason(void)
{
    return s_reason;
}

static int by_rssi(const void *a, const void *b)
{
    return ((const wifi_ap_t *)b)->rssi - ((const wifi_ap_t *)a)->rssi;
}

esp_err_t wifi_scan(wifi_ap_t *out, size_t max, size_t *count)
{
    *count = 0;
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    // An app's trial is busy until it ends (at most ~30 s). Reconnects to a stored network that is
    // not up are cancelled for the scan instead: that network may be gone for good.
    if (s_mode == MODE_TRIAL && s_result == WIFI_RESULT_CONNECTING) {
        return ESP_ERR_WIFI_STATE;
    }
    const bool reconnecting = s_mode == MODE_KEEP && !s_up;
    s_scanning = true;
    if (reconnecting) {
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    const wifi_scan_config_t scan_cfg = {.show_hidden = false};
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err == ESP_ERR_WIFI_STATE && !s_up) {
        ESP_LOGW(TAG, "scan refused while connecting; cancelling and retrying");
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(500));
        err = esp_wifi_scan_start(&scan_cfg, true);
    }
    s_scanning = false;
    if (reconnecting && !s_hold && s_mode == MODE_KEEP && !s_up) {
        esp_wifi_connect();
    }
    if (err != ESP_OK) {
        return err;
    }
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 64) {
        n = 64;
    }
    wifi_ap_record_t *records = calloc(n ? n : 1, sizeof(*records));
    if (!records) {
        esp_wifi_clear_ap_list();
        return ESP_ERR_NO_MEM;
    }
    err = esp_wifi_scan_get_ap_records(&n, records);
    if (err != ESP_OK) {
        free(records);
        return err;
    }
    size_t used = 0;
    for (uint16_t i = 0; i < n; ++i) {
        const char *ssid = (const char *)records[i].ssid;
        if (!ssid[0]) {
            continue;
        }
        size_t j = 0;
        while (j < used && strcmp(out[j].ssid, ssid) != 0) {
            ++j;
        }
        if (j < used) {
            if (records[i].rssi > out[j].rssi) {
                out[j].rssi = records[i].rssi;
            }
            continue;
        }
        if (used == max) {
            // Records come strongest first, so the rest are weaker.
            continue;
        }
        strlcpy(out[used].ssid, ssid, sizeof(out[used].ssid));
        out[used].rssi = records[i].rssi;
        out[used].secure = records[i].authmode != WIFI_AUTH_OPEN && records[i].authmode != WIFI_AUTH_OWE;
        ++used;
    }
    free(records);
    qsort(out, used, sizeof(*out), by_rssi);
    *count = used;
    return ESP_OK;
}

void wifi_set_power_save(bool on)
{
    if (!s_started) {
        return;
    }
    const esp_err_t err = esp_wifi_set_ps(on ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "power save %s: %s", on ? "on" : "off", esp_err_to_name(err));
    }
}

bool wifi_is_up(void)
{
    return s_up;
}

void wifi_ip_string(char *out, size_t len)
{
    out[0] = '\0';
    esp_netif_ip_info_t info;
    if (s_up && s_netif && esp_netif_get_ip_info(s_netif, &info) == ESP_OK) {
        snprintf(out, len, IPSTR, IP2STR(&info.ip));
    }
}

bool wifi_ip_info(uint32_t *ip, uint32_t *netmask)
{
    esp_netif_ip_info_t info;
    if (!s_up || !s_netif || esp_netif_get_ip_info(s_netif, &info) != ESP_OK || info.ip.addr == 0) {
        return false;
    }
    *ip = info.ip.addr;
    *netmask = info.netmask.addr;
    return true;
}

esp_err_t wifi_wait(uint32_t timeout_ms)
{
    if (!s_started || s_mode == MODE_NONE) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_up) {
        return ESP_OK;
    }
    EventBits_t bits = xEventGroupWaitBits(s_events, WIFI_UP_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms));
    return (bits & WIFI_UP_BIT) ? ESP_OK : ESP_ERR_TIMEOUT;
}
