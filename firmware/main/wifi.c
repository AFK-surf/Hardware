// SPDX-License-Identifier: Apache-2.0
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
#include "wifi_scan_retry.h"

#define WIFI_UP_BIT BIT0
#define SCAN_DONE_BIT BIT1
// A scan of all channels takes about 2-3 s; this bounds a scan that never reports done.
#define SCAN_TIMEOUT_MS 10000
// A connect attempt to a network that is not found gives up after about 2.5 s.
#define SETTLE_TIMEOUT_MS 4000
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
static atomic_uint s_ip_generation;
static atomic_int s_mode = MODE_NONE;
static atomic_int s_result = WIFI_RESULT_IDLE;
// Automatic reconnects of a MODE_KEEP connection are held: by provisioning while an app is
// connected, and by a scan while it runs. A connect attempt in flight blocks scanning, and an
// unreachable network would otherwise be retried forever.
static atomic_bool s_hold;
static atomic_bool s_scanning;
static atomic_bool s_off;  // wifi_turn_off() until the next wifi_connect()
// A connect attempt is in flight: from esp_wifi_connect() until the driver reports it connected or
// disconnected. The driver refuses or aborts a scan meanwhile.
static atomic_bool s_connecting;
static atomic_bool s_scan_failed;  // the last scan done event reported a failure
static int s_attempts;
static int s_auth_failures;
static int s_not_found;

static esp_err_t start_connect(void)
{
    s_connecting = true;
    const esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        s_connecting = false;
    }
    return err;
}

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
    start_connect();
}

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = data;
        const int reason = event ? event->reason : -1;
        s_up = false;
        s_connecting = false;
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
            // A scan, a hold or a new connection may have started during the delay.
            if (s_hold || s_scanning || s_off || s_mode != MODE_KEEP || s_up) {
                return;
            }
            start_connect();
        } else if (mode == MODE_TRIAL) {
            ESP_LOGW(TAG, "disconnected, reason %d", reason);
            on_trial_disconnect(reason);
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        const wifi_event_sta_scan_done_t *event = data;
        s_scan_failed = event && event->status != 0;
        xEventGroupSetBits(s_events, SCAN_DONE_BIT);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        // Logged for every join: a network can have more than one BSSID, and a stuck link may follow
        // a move between them.
        const wifi_event_sta_connected_t *event = data;
        s_connecting = false;
        if (event) {
            ESP_LOGI(TAG, "associated bssid=%02x:%02x:%02x:%02x:%02x:%02x ch=%u", event->bssid[0], event->bssid[1],
                     event->bssid[2], event->bssid[3], event->bssid[4], event->bssid[5], event->channel);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = data;
        ESP_LOGI(TAG, "got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_up = true;
        s_ip_generation++;
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
        start_connect();
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

    s_off = false;
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
    s_result = WIFI_RESULT_CONNECTING;
    s_mode = keep_retrying ? MODE_KEEP : MODE_TRIAL;
    err = start_connect();
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

static int by_rssi(const void *a, const void *b)
{
    return ((const wifi_ap_t *)b)->rssi - ((const wifi_ap_t *)a)->rssi;
}

static void scan_settle(void *ctx)
{
    (void)ctx;
    esp_wifi_disconnect();  // cancels an attempt in flight; its disconnect event clears s_connecting
    for (int waited = 0; s_connecting && waited < SETTLE_TIMEOUT_MS; waited += 50) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (s_connecting) {
        ESP_LOGW(TAG, "connect attempt still in flight; scanning anyway");
        s_connecting = false;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
}

static esp_err_t scan_run(void *ctx)
{
    (void)ctx;
    const wifi_scan_config_t scan_cfg = {.show_hidden = false};
    xEventGroupClearBits(s_events, SCAN_DONE_BIT);
    s_scan_failed = false;
    const esp_err_t err = esp_wifi_scan_start(&scan_cfg, false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan refused: %s", esp_err_to_name(err));
        return err;
    }
    const EventBits_t bits =
        xEventGroupWaitBits(s_events, SCAN_DONE_BIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(SCAN_TIMEOUT_MS));
    if (!(bits & SCAN_DONE_BIT)) {
        esp_wifi_scan_stop();
        return ESP_ERR_TIMEOUT;
    }
    if (s_scan_failed) {
        ESP_LOGW(TAG, "scan aborted");
    }
    return ESP_OK;
}

static bool scan_aborted(void *ctx)
{
    (void)ctx;
    return s_scan_failed;
}

static esp_err_t read_results(wifi_ap_t *out, size_t max, size_t *count);

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
    const wifi_scan_ops_t ops = {.settle = scan_settle, .run = scan_run, .aborted = scan_aborted};
    esp_err_t err = wifi_scan_with_retry(&ops, reconnecting || s_connecting);
    if (err == ESP_OK) {
        // Before any reconnect: a connect attempt scans too, and that clears these results.
        err = read_results(out, max, count);
    } else {
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(err));
        esp_wifi_clear_ap_list();
    }
    s_scanning = false;
    if (reconnecting && !s_hold && s_mode == MODE_KEEP && !s_up) {
        start_connect();
    }
    return err;
}

static esp_err_t read_results(wifi_ap_t *out, size_t max, size_t *count)
{
    esp_err_t err;
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

bool wifi_link_info(char *ssid, size_t ssid_len, int8_t *rssi)
{
    wifi_ap_record_t ap;
    if (!s_up || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return false;
    }
    if (ssid && ssid_len) {
        strlcpy(ssid, (const char *)ap.ssid, ssid_len);
    }
    if (rssi) {
        *rssi = ap.rssi;
    }
    return true;
}

void wifi_turn_off(void)
{
    wifi_disconnect();
    s_off = true;
    ESP_LOGI(TAG, "turned off");
}

bool wifi_is_off(void)
{
    return s_off;
}

unsigned wifi_ip_generation(void)
{
    return s_ip_generation;
}

uint32_t wifi_gateway(void)
{
    esp_netif_ip_info_t info;
    if (!s_up || !s_netif || esp_netif_get_ip_info(s_netif, &info) != ESP_OK) {
        return 0;
    }
    return info.gw.addr;
}

bool wifi_rejoin(void)
{
    if (!s_started || s_mode != MODE_KEEP || s_hold || s_scanning) {
        return false;
    }
    // The disconnect event reconnects a MODE_KEEP link on its own.
    return esp_wifi_disconnect() == ESP_OK;
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
