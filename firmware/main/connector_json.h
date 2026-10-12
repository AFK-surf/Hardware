#pragma once

#include "cJSON.h"

#include <stdbool.h>

// Install once at boot, before starting tasks that parse Connector frames.
void connector_json_init(void);

// While on, every heap allocation this task fails is logged with its size, caps and the free and
// largest block for those caps (the hook connector_json_init installs). Independent of
// CONFIG_COMMA_HEAP_DIAGNOSTICS.
void comma_log_failed_allocs(bool on);

// Malformed JSON is logged and ignored. Only a real allocation failure invokes on_oom.
cJSON *connector_json_parse(const char *json, void (*on_oom)(const char *json));
