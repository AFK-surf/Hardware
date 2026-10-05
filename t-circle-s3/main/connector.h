#pragma once

#include <stdbool.h>

// Comma Connector (docs/connector-protocol.md): holds a /v1/connect socket so the device shows up
// in Settings › Devices. Runs in its own task with its own websocket client and shares nothing
// with the voice call but Wi-Fi. Waits for an IP itself, so it can be started right after
// wifi_start(). Safe to call once.
void connector_start(void);

bool connector_is_up(void);
