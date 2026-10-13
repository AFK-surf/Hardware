// SPDX-License-Identifier: Apache-2.0
#include "device_config.h"

#include <stdatomic.h>
#include <stddef.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef COMMA_DEV_SECRETS
#define COMMA_DEV_SECRETS 0
#endif
// secrets.h files from before the rename used CUE_* names. The optional ones would be skipped
// silently (Connector off), so stop the build instead.
#if COMMA_DEV_SECRETS && (defined(CUE_VOICE_SERVER) || defined(CUE_VOICE_GROUP) || defined(CUE_VOICE_KEY) || \
                          defined(CUE_WORKSPACE_ID) || defined(CUE_CONNECTOR_URL) || defined(CUE_CONNECTOR_TOKEN))
#error "secrets.h uses the old CUE_* names: rename them to COMMA_* (see secrets.h.example)"
#endif

#define NS "comma"
// Written last: a config is only complete once this key is 1.
#define KEY_PROVISIONED "provisioned"
#define ACCESS_NS "connector"
#define ACCESS_KEY "allow_ops"

static const char *TAG = "config";

static SemaphoreHandle_t s_lock;
static device_config_t s_cfg;
static atomic_bool s_provisioned;
static atomic_uint s_generation;
static bool s_allow_operations;  // protected by s_lock, together with credentials and NVS writes

static esp_err_t save_access(bool allow)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(ACCESS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(h, ACCESS_KEY, allow);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static bool same_identity(const device_config_t *a, const device_config_t *b)
{
    // Wi-Fi and display-name edits preserve consent. Token/key rotation requires new consent:
    // the firmware cannot prove that a new token belongs to the same device identity.
    return strcmp(a->server, b->server) == 0 && strcmp(a->connect_url, b->connect_url) == 0 &&
           strcmp(a->workspace_id, b->workspace_id) == 0 && strcmp(a->group_id, b->group_id) == 0 &&
           strcmp(a->connector_token, b->connector_token) == 0 && strcmp(a->voice_key, b->voice_key) == 0;
}

typedef struct {
    const char *key;
    size_t offset;
    size_t size;
    bool required;
} field_t;

#define FIELD(k, f, req) {k, offsetof(device_config_t, f), sizeof(((device_config_t *)0)->f), req}
static const field_t FIELDS[] = {
    FIELD("wifi_ssid", wifi_ssid, true),
    FIELD("wifi_pass", wifi_password, false),
    FIELD("server", server, true),
    FIELD("connect_url", connect_url, true),
    FIELD("workspace", workspace_id, true),
    FIELD("group", group_id, true),
    FIELD("conn_token", connector_token, true),
    FIELD("voice_key", voice_key, true),
    FIELD("device_name", device_name, true),
};

#if COMMA_DEV_SECRETS
static void copy(char *dst, size_t size, const char *src)
{
    strlcpy(dst, src, size);
}

static void load_dev_override(void)
{
    copy(s_cfg.wifi_ssid, sizeof(s_cfg.wifi_ssid), WIFI_SSID);
    copy(s_cfg.wifi_password, sizeof(s_cfg.wifi_password), WIFI_PASSWORD);
    copy(s_cfg.server, sizeof(s_cfg.server), COMMA_VOICE_SERVER);
    copy(s_cfg.group_id, sizeof(s_cfg.group_id), COMMA_VOICE_GROUP);
    copy(s_cfg.voice_key, sizeof(s_cfg.voice_key), COMMA_VOICE_KEY);
#ifdef COMMA_CONNECTOR_URL
    copy(s_cfg.connect_url, sizeof(s_cfg.connect_url), COMMA_CONNECTOR_URL);
#endif
#ifdef COMMA_CONNECTOR_TOKEN
    copy(s_cfg.connector_token, sizeof(s_cfg.connector_token), COMMA_CONNECTOR_TOKEN);
#endif
#ifdef COMMA_WORKSPACE_ID
    copy(s_cfg.workspace_id, sizeof(s_cfg.workspace_id), COMMA_WORKSPACE_ID);
#endif
    // No device_name: the Connector dials with the BLE name, Comma-XXXX.
    s_provisioned = true;
    ESP_LOGW(TAG, "using secrets.h (COMMA_DEV_SECRETS), NVS credentials ignored");
}
#endif

static void load_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "no stored credentials");
        return;
    }
    uint8_t provisioned = 0;
    if (nvs_get_u8(h, KEY_PROVISIONED, &provisioned) != ESP_OK || provisioned != 1) {
        nvs_close(h);
        ESP_LOGI(TAG, "credentials incomplete, not provisioned");
        return;
    }
    device_config_t cfg = {0};
    for (size_t i = 0; i < sizeof(FIELDS) / sizeof(FIELDS[0]); ++i) {
        size_t len = FIELDS[i].size;
        char *dst = (char *)&cfg + FIELDS[i].offset;
        esp_err_t err = nvs_get_str(h, FIELDS[i].key, dst, &len);
        if (err != ESP_OK && FIELDS[i].required) {
            ESP_LOGE(TAG, "stored config lacks %s (%s)", FIELDS[i].key, esp_err_to_name(err));
            nvs_close(h);
            return;
        }
    }
    nvs_close(h);
    s_cfg = cfg;
    s_provisioned = true;
    ESP_LOGI(TAG, "provisioned as \"%s\" on Wi-Fi \"%s\"", cfg.device_name, cfg.wifi_ssid);
}

esp_err_t device_config_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        return err;
    }
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
#if COMMA_DEV_SECRETS
    load_dev_override();
#else
    load_nvs();
    nvs_handle_t access;
    uint8_t allow = 0;
    if (s_provisioned && nvs_open(ACCESS_NS, NVS_READONLY, &access) == ESP_OK) {
        nvs_get_u8(access, ACCESS_KEY, &allow);
        nvs_close(access);
    }
    s_allow_operations = allow == 1;
#endif
    return ESP_OK;
}

bool device_config_provisioned(void)
{
    return s_provisioned;
}

bool device_config_dev_override(void)
{
    return COMMA_DEV_SECRETS;
}

bool device_config_get(device_config_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool ok = s_provisioned;
    if (ok) {
        *out = s_cfg;
    } else {
        memset(out, 0, sizeof(*out));
    }
    xSemaphoreGive(s_lock);
    return ok;
}

esp_err_t device_config_save(const device_config_t *cfg)
{
    if (COMMA_DEV_SECRETS) {
        // Still store it, so turning the override off keeps the provisioned values.
        ESP_LOGW(TAG, "saving to NVS, but secrets.h stays in effect until COMMA_DEV_SECRETS is 0");
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    // Revoke durably BEFORE replacing credentials, including on a power cut or failed save.
    // Access writes use this same mutex, so an old socket cannot re-enable consent mid-save.
    if (!s_provisioned || !same_identity(&s_cfg, cfg) || COMMA_DEV_SECRETS) {
        s_allow_operations = false;
        esp_err_t revoke = save_access(false);
        if (revoke != ESP_OK) {
            xSemaphoreGive(s_lock);
            return revoke;
        }
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        xSemaphoreGive(s_lock);
        return err;
    }
    // Clear the flag first so a power cut mid-write leaves the device unprovisioned, not half set.
    err = nvs_set_u8(h, KEY_PROVISIONED, 0);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    for (size_t i = 0; err == ESP_OK && i < sizeof(FIELDS) / sizeof(FIELDS[0]); ++i) {
        err = nvs_set_str(h, FIELDS[i].key, (const char *)cfg + FIELDS[i].offset);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(h, KEY_PROVISIONED, 1);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(err));
        xSemaphoreGive(s_lock);
        return err;
    }
    if (!COMMA_DEV_SECRETS) {
        s_cfg = *cfg;
        s_provisioned = true;
    }
    ++s_generation;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "saved \"%s\" on Wi-Fi \"%s\"", cfg->device_name, cfg->wifi_ssid);
    return ESP_OK;
}

unsigned device_config_generation(void)
{
    return s_generation;
}

bool device_config_operations_allowed(unsigned generation)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool allowed = s_provisioned && generation == s_generation && s_allow_operations;
    xSemaphoreGive(s_lock);
    return allowed;
}

esp_err_t device_config_set_operations_allowed(unsigned generation, bool allow)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_ERR_INVALID_STATE;
    if (s_provisioned && generation == s_generation) {
        // A compiled-in identity may change on every flash, so dev consent lasts only this boot.
        err = COMMA_DEV_SECRETS ? ESP_OK : save_access(allow);
        if (err == ESP_OK) s_allow_operations = allow;
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t device_config_erase_all(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(&s_cfg, 0, sizeof(s_cfg));
    s_provisioned = false;
    s_allow_operations = false;
    ++s_generation;
    // nvs_flash_erase deinitializes the default partition first.
    const esp_err_t err = nvs_flash_erase();
    xSemaphoreGive(s_lock);
    ESP_LOGW(TAG, "NVS erased: %s", esp_err_to_name(err));
    return err;
}
