#include "connector_json.h"

#include <stdbool.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static _Thread_local bool s_parsing;
static _Thread_local bool s_failed;
static _Thread_local bool s_log_failures;

void comma_log_failed_allocs(bool on)
{
    s_log_failures = on;
}

static void allocation_failed(size_t size, uint32_t caps, const char *function)
{
    // Another task or an interrupt must not turn a syntax error into an OOM error.
    if (!xPortInIsrContext() && s_parsing) {
        s_failed = true;
    }
    if (!xPortInIsrContext() && s_log_failures) {
        // The ROM printer allocates nothing, so this cannot fail again inside the hook.
        ESP_EARLY_LOGW("heap", "%s(%u) failed, caps 0x%x: free %u, largest %u for those caps", function,
                       (unsigned)size, (unsigned)caps, (unsigned)heap_caps_get_free_size(caps),
                       (unsigned)heap_caps_get_largest_free_block(caps));
    }
}

void connector_json_init(void)
{
    // IDF reports failure only after malloc's internal/PSRAM fallback has failed.
    // Keep cJSON's default hooks: custom hooks would disable its realloc fast path
    // globally, including in the voice and provisioning tasks.
    ESP_ERROR_CHECK(heap_caps_register_failed_alloc_callback(allocation_failed));
}

cJSON *connector_json_parse(const char *json, void (*on_oom)(const char *json))
{
    s_failed = false;
    s_parsing = true;
    cJSON *root = cJSON_Parse(json);
    s_parsing = false;
    if (!root) {
        if (s_failed) {
            on_oom(json);
        } else {
            ESP_LOGW("connector", "invalid JSON, ignored");
        }
    }
    return root;
}
