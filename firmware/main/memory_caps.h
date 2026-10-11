#pragma once
#include "firmware_config.h"
#include "esp_heap_caps.h"
// These buffers/stacks never write flash. Preserve PSRAM placement when present;
// a no-PSRAM build uses byte-addressable internal RAM, never an unavailable heap.
#if !defined(ESP_PLATFORM) || defined(CONFIG_SPIRAM)
#define COMMA_BUFFER_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#else
#define COMMA_BUFFER_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#endif
