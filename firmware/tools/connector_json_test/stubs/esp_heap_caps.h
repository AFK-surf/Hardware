#pragma once
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
typedef void (*esp_alloc_failed_hook_t)(size_t, uint32_t, const char *);
int heap_caps_register_failed_alloc_callback(esp_alloc_failed_hook_t callback);
#define ESP_ERROR_CHECK(expr) assert((expr) == 0)
