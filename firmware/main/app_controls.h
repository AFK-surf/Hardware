#pragma once
#include <stdbool.h>
#include <stdint.h>
void app_controls_init(bool boot);
// Called by each board's UI loop. Returns whether a BOOT hold is being tracked.
bool app_poll(int64_t now, bool in_call, bool boot);
