#pragma once
#include <stdbool.h>
#include <stdint.h>
// BOOT held this long, then released, enters pairing mode; held RESET long, factory reset.
#define APP_PAIRING_HOLD_US (3LL * 1000000)
#define APP_RESET_HOLD_US (10LL * 1000000)
void app_controls_init(bool boot);
// Called by each board's UI loop. Returns whether a BOOT hold is being tracked.
bool app_poll(int64_t now, bool in_call, bool boot);
