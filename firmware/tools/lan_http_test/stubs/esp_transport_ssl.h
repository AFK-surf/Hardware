// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "esp_transport.h"
esp_transport_handle_t esp_transport_ssl_init(void);
void esp_transport_ssl_crt_bundle_attach(esp_transport_handle_t t, int (*attach)(void *));
void esp_transport_ssl_set_common_name(esp_transport_handle_t t, const char *name);
