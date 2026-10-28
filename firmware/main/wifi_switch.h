// SPDX-License-Identifier: Apache-2.0
#pragma once

// Move the device to another Wi-Fi network from the device itself (ui/settings.c), keeping its Comma
// credentials. The new network is tried first, as a trial (wifi_connect without keep_retrying); only
// once it has an IP is it stored, with every other field of the stored config unchanged, so
// device_config_save() keeps the operation consent of a Wi-Fi-only change. A trial that fails or
// takes too long goes straight back to the stored network. Never while pairing mode runs: the app
// owns Wi-Fi then. The password is held only until the trial ends and is never logged.

#include <stdbool.h>

#include "esp_err.h"
#include "wifi.h"

typedef enum {
    WIFI_SWITCH_IDLE = 0,
    WIFI_SWITCH_TRYING,
    WIFI_SWITCH_SAVED,     // on the new network, and it is stored
    WIFI_SWITCH_REVERTED,  // the new network was given up; back on the stored one
} wifi_switch_state_t;

// Start a trial. Fails when one is running, the device is not provisioned or pairing mode is on.
esp_err_t wifi_switch_start(const char *ssid, const char *password);

// Drive the trial. The UI loop calls this every pass, whether or not the settings are open.
wifi_switch_state_t wifi_switch_poll(void);

// Why the last trial was given up: WIFI_RESULT_WRONG_PASSWORD, _NOT_FOUND or _FAILED (no answer in
// time, or the new network could not be stored). WIFI_RESULT_IDLE after wifi_switch_cancel().
wifi_result_t wifi_switch_failure(void);

// Give up the trial in flight and go back to the stored network.
void wifi_switch_cancel(void);

// Forget a finished trial's result (back to WIFI_SWITCH_IDLE). A running trial is not affected.
void wifi_switch_reset(void);
