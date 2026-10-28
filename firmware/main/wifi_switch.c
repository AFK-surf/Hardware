// SPDX-License-Identifier: Apache-2.0
#include "wifi_switch.h"

#include <stdlib.h>
#include <string.h>

#include "device_config.h"
#include "provisioning.h"

#include "esp_log.h"
#include "esp_timer.h"

// wifi.c ends a trial after a few failed attempts; this bounds one that neither joins nor fails.
#define TRIAL_TIMEOUT_US (45LL * 1000000)

static const char *TAG = "wifi_switch";

static wifi_switch_state_t s_state = WIFI_SWITCH_IDLE;
static wifi_result_t s_failure = WIFI_RESULT_IDLE;
static char s_ssid[33];
static char s_password[65];
static int64_t s_since_us;

static void wipe(void *p, size_t n)
{
    volatile unsigned char *v = p;
    while (n--) {
        *v++ = 0;
    }
}

static void free_config(device_config_t *cfg)
{
    if (cfg) {
        wipe(cfg, sizeof(*cfg));
        free(cfg);
    }
}

// Back to the stored network, which keep-retries as before the trial.
static void revert(wifi_result_t why)
{
    wipe(s_password, sizeof(s_password));
    s_failure = why;
    s_state = WIFI_SWITCH_REVERTED;
    device_config_t *cfg = calloc(1, sizeof(*cfg));
    if (cfg && device_config_get(cfg)) {
        ESP_LOGW(TAG, "\"%s\" given up (result %d); back to \"%s\"", s_ssid, (int)why, cfg->wifi_ssid);
        const esp_err_t err = wifi_connect(cfg->wifi_ssid, cfg->wifi_password, true);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "stored network: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGE(TAG, "no stored network to go back to");
    }
    free_config(cfg);
}

esp_err_t wifi_switch_start(const char *ssid, const char *password)
{
    if (s_state == WIFI_SWITCH_TRYING || !device_config_provisioned() || provisioning_active()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!password) {
        password = "";
    }
    const size_t pass_len = strlen(password);
    if (!ssid || !ssid[0] || strlen(ssid) >= sizeof(s_ssid) || pass_len >= sizeof(s_password) ||
        (pass_len > 0 && pass_len < 8)) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(s_ssid, ssid, sizeof(s_ssid));
    strlcpy(s_password, password, sizeof(s_password));
    s_failure = WIFI_RESULT_IDLE;
    ESP_LOGI(TAG, "trying \"%s\"; the stored network is kept until it is up", s_ssid);
    s_state = WIFI_SWITCH_TRYING;
    s_since_us = esp_timer_get_time();
    const esp_err_t err = wifi_connect(s_ssid, s_password, false);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "trial did not start: %s", esp_err_to_name(err));
        revert(WIFI_RESULT_FAILED);
        return err;
    }
    return ESP_OK;
}

// The trial has an IP: store the new network with the rest of the config as it is.
static void keep_new_network(void)
{
    // Reconnect forever from now on, as for any stored network.
    wifi_keep_retrying();
    device_config_t *cfg = calloc(1, sizeof(*cfg));
    esp_err_t err = ESP_ERR_NO_MEM;
    if (cfg && device_config_get(cfg)) {
        strlcpy(cfg->wifi_ssid, s_ssid, sizeof(cfg->wifi_ssid));
        strlcpy(cfg->wifi_password, s_password, sizeof(cfg->wifi_password));
        err = device_config_save(cfg);
    } else if (cfg) {
        err = ESP_ERR_INVALID_STATE;
    }
    free_config(cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "new network not stored: %s", esp_err_to_name(err));
        revert(WIFI_RESULT_FAILED);
        return;
    }
    wipe(s_password, sizeof(s_password));
    s_state = WIFI_SWITCH_SAVED;
    ESP_LOGI(TAG, "now on \"%s\", stored; Comma credentials unchanged", s_ssid);
}

wifi_switch_state_t wifi_switch_poll(void)
{
    if (s_state != WIFI_SWITCH_TRYING) {
        return s_state;
    }
    if (provisioning_active()) {
        // Pairing mode took over (a BOOT hold): it brings its own network handling.
        revert(WIFI_RESULT_FAILED);
        return s_state;
    }
    const wifi_result_t result = wifi_result();
    if (result == WIFI_RESULT_UP) {
        keep_new_network();
    } else if (result != WIFI_RESULT_CONNECTING) {
        revert(result == WIFI_RESULT_IDLE ? WIFI_RESULT_FAILED : result);
    } else if (esp_timer_get_time() - s_since_us > TRIAL_TIMEOUT_US) {
        revert(WIFI_RESULT_FAILED);
    }
    return s_state;
}

wifi_result_t wifi_switch_failure(void)
{
    return s_failure;
}

void wifi_switch_cancel(void)
{
    if (s_state == WIFI_SWITCH_TRYING) {
        ESP_LOGI(TAG, "cancelled");
        revert(WIFI_RESULT_IDLE);
    }
}

void wifi_switch_reset(void)
{
    if (s_state != WIFI_SWITCH_TRYING) {
        s_state = WIFI_SWITCH_IDLE;
        s_failure = WIFI_RESULT_IDLE;
    }
}
