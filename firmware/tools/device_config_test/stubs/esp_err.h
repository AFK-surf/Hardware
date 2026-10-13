// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <assert.h>
typedef int esp_err_t;
enum { ESP_OK, ESP_ERR_NO_MEM, ESP_ERR_INVALID_STATE, ESP_ERR_NVS_NO_FREE_PAGES,
       ESP_ERR_NVS_NEW_VERSION_FOUND, ESP_ERR_NVS_NOT_FOUND, ESP_FAIL };
#define ESP_ERROR_CHECK(x) assert((x) == ESP_OK)
static inline const char *esp_err_to_name(esp_err_t err) { (void)err; return "test error"; }
