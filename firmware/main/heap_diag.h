// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "firmware_config.h"
#if defined(ESP_PLATFORM) && CONFIG_COMMA_HEAP_DIAGNOSTICS
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
// free/largest/min count every internal heap, including the classic ESP32's 32-bit-only IRAM heap
// (about 38 KB on the DevKit, reported as a 36864-byte largest block) that byte buffers cannot use.
// free8/largest8 are the internal memory a char buffer can actually get.
static inline void comma_heap_log(const char *point)
{
    ESP_LOGI("heap", "%s free=%u largest=%u min=%u free8=%u largest8=%u task=%s stack_free=%u", point,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        pcTaskGetName(NULL), (unsigned)uxTaskGetStackHighWaterMark(NULL));
}
#else
static inline void comma_heap_log(const char *point) { (void)point; }
#endif

