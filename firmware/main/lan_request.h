// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_timer.h"

// Optional lifetime of a request received from a Connector socket. The deadline starts at
// receipt, not when the LAN worker eventually dequeues it. NULL is used by standalone tests.
typedef struct {
    int64_t deadline_us;
    const char *(*cancelled)(void *ctx);
    void *ctx;
} lan_request_t;

static inline const char *lan_request_check(const lan_request_t *request)
{
    if (!request) return NULL;
    const char *reason = request->cancelled ? request->cancelled(request->ctx) : NULL;
    if (reason) return reason;
    return esp_timer_get_time() >= request->deadline_us ? "LAN request timed out (including queue time)" : NULL;
}

static inline int64_t lan_request_deadline(const lan_request_t *request, int64_t local_deadline)
{
    return request && request->deadline_us < local_deadline ? request->deadline_us : local_deadline;
}
