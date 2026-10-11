#pragma once
#include "firmware_config.h"
#if defined(ESP_PLATFORM) && CONFIG_COMMA_HEAP_DIAGNOSTICS
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
static inline void comma_heap_log(const char *point)
{
    ESP_LOGI("heap", "%s free=%u largest=%u min=%u task=%s stack_free=%u", point,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
        pcTaskGetName(NULL), (unsigned)uxTaskGetStackHighWaterMark(NULL));
}
#else
static inline void comma_heap_log(const char *point) { (void)point; }
#endif
