// SPDX-License-Identifier: Apache-2.0
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

// Hold (true) or resume automatic reconnects of a keep_retrying connection that is down. Holding
// cancels an attempt in flight; a connection that is up stays up. wifi_connect still makes its
// first attempt while held.
void wifi_hold_reconnect(bool hold);

// Drop the link and stop retrying.
void wifi_disconnect(void);

wifi_result_t wifi_result(void);

// Blocking scan (a few seconds). Fills up to max unique SSIDs, strongest first. Fails with
// ESP_ERR_WIFI_STATE only while a trial is in progress; a keep_retrying attempt is cancelled for
// the scan and resumed after it unless reconnects are held.
esp_err_t wifi_scan(wifi_ap_t *out, size_t max, size_t *count);

// Modem sleep must be on while BLE runs (Wi-Fi/BLE coexistence). Calls want it off.
void wifi_set_power_save(bool on);

bool wifi_is_up(void);

// The access point of the link while up: its SSID and signal. False when down.
bool wifi_link_info(char *ssid, size_t ssid_len, int8_t *rssi);

// Turned off on purpose (the on-screen settings): the link is dropped and nothing reconnects until
// the next wifi_connect(), which turns it on again. Not stored: a restart joins the stored network.
void wifi_turn_off(void);
bool wifi_is_off(void);

// Counts IP_EVENT_STA_GOT_IP: changes each time the link comes (back) up.
unsigned wifi_ip_generation(void);

// The router's address while up (network byte order), 0 otherwise.
uint32_t wifi_gateway(void);

// Drop and rejoin the stored network (only when it's the kept network, not during pairing or a
// scan). Returns false when nothing was done.
bool wifi_rejoin(void);

// "a.b.c.d", or "" when down.
void wifi_ip_string(char *out, size_t len);

// The station's IPv4 address and netmask, in network byte order. False when down.
bool wifi_ip_info(uint32_t *ip, uint32_t *netmask);

// Block until the station has an IP, or the timeout elapses.
esp_err_t wifi_wait(uint32_t timeout_ms);
