#pragma once

#include "cJSON.h"

// Install once at boot, before starting tasks that parse Connector frames.
void connector_json_init(void);

// Malformed JSON is logged and ignored. Only a real allocation failure invokes on_oom.
cJSON *connector_json_parse(const char *json, void (*on_oom)(const char *json));
