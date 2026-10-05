#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    WIFI_RESULT_IDLE = 0,      // no connect requested
    WIFI_RESULT_CONNECTING,
    WIFI_RESULT_UP,
    WIFI_RESULT_WRONG_PASSWORD,
    WIFI_RESULT_NOT_FOUND,
    WIFI_RESULT_FAILED,        // other disconnect reasons, or no IP in time
} wifi_result_t;

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure;
} wifi_ap_t;

// Bring up the STA interface without connecting. Needs NVS (device_config_init) first.
esp_err_t wifi_init(void);

// Connect to a network. keep_retrying: reconnect forever (normal use). Otherwise this is a
// trial (provisioning): after a few failed attempts it stops and wifi_result() says why.
esp_err_t wifi_connect(const char *ssid, const char *password, bool keep_retrying);

// Turn a successful trial into a normal connection that reconnects forever.
void wifi_keep_retrying(void);

// Drop the link and stop retrying.
void wifi_disconnect(void);

wifi_result_t wifi_result(void);
// Last disconnect reason (wifi_err_reason_t), 0 if none.
int wifi_last_reason(void);

// Blocking scan. Fills up to max unique SSIDs, strongest first. Fails with ESP_ERR_WIFI_STATE
// while a connect attempt is in progress.
esp_err_t wifi_scan(wifi_ap_t *out, size_t max, size_t *count);

// Modem sleep must be on while BLE runs (Wi-Fi/BLE coexistence). Calls want it off.
void wifi_set_power_save(bool on);

bool wifi_is_up(void);

// "a.b.c.d", or "" when down.
void wifi_ip_string(char *out, size_t len);

// Block until the station has an IP, or the timeout elapses.
esp_err_t wifi_wait(uint32_t timeout_ms);
