#pragma once
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
typedef void (*esp_alloc_failed_hook_t)(size_t, uint32_t, const char *);
int heap_caps_register_failed_alloc_callback(esp_alloc_failed_hook_t callback);
size_t heap_caps_get_free_size(uint32_t caps);
size_t heap_caps_get_largest_free_block(uint32_t caps);
#define ESP_ERROR_CHECK(expr) assert((expr) == 0)
