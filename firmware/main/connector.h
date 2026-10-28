// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>

// Comma Connector (docs/connector-protocol.md): holds a /v1/connect socket so the device shows up
// in Settings › Devices. Runs in its own task with its own websocket client and shares nothing
// with the voice call but Wi-Fi. Waits for an IP itself, so it can be started right after
// wifi_init(). Safe to call more than once. Credentials come from device_config; when new ones
// are saved (pairing mode) it reconnects with them.
void connector_start(void);

// The Connector socket is connected (for the on-screen settings).
bool connector_online(void);
