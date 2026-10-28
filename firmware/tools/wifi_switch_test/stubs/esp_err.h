// SPDX-License-Identifier: Apache-2.0
#pragma once
typedef int esp_err_t;
enum { ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_NO_MEM = 0x101, ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_STATE };
static inline const char *esp_err_to_name(esp_err_t err) { return err == ESP_OK ? "ESP_OK" : "error"; }
