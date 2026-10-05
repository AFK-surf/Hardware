#pragma once

#include <stdbool.h>
#include "esp_err.h"

// Start STA and keep retrying. Safe to call once.
esp_err_t wifi_start(void);

// False when secrets.h still has the placeholder SSID.
bool wifi_has_credentials(void);

bool wifi_is_up(void);

// Block until the station has an IP, or the timeout elapses.
esp_err_t wifi_wait(uint32_t timeout_ms);
