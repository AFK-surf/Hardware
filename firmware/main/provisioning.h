// SPDX-License-Identifier: Apache-2.0
#pragma once

// BLE provisioning: the Comma app gives the device its Wi-Fi network and Comma credentials.
// The GATT contract is in docs/ble-provisioning.md.
//
// BLE runs only while provisioning is active (unprovisioned, or pairing mode entered with a
// BOOT hold). It is shut down completely otherwise, and a call never starts while it runs.

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

// The name the device advertises as: "Comma-XXXX", the last two bytes of the Wi-Fi MAC.
// Works without BLE, so the Connector can fall back to it.
void provisioning_ble_name(char *out, size_t size);

// Start advertising as Comma-XXXX. Needs wifi_init() and display_init() first.
esp_err_t provisioning_start(void);

// Leave pairing mode. Asynchronous: provisioning_active() turns false once BLE is down.
// An unprovisioned device ignores this.
void provisioning_stop(void);

bool provisioning_active(void);

// NimBLE is up and holds its memory: from provisioning_start until BLE is shut down (before the
// "All set" screen finishes, so earlier than provisioning_active turns false).
bool provisioning_ble_on(void);

// Draw the current pairing screen again (after something else used the display).
void provisioning_redraw(void);

// For the Connector: report its state so a connected app sees it in `status`.
// state: "connector_connecting", "connector_up" or "connector_failed". device_id may be NULL.
void provisioning_set_connector_state(const char *state, const char *device_id);
