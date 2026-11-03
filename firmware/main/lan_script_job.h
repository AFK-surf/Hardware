// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "firmware_config.h"

#include "cJSON.h"
#include "lan_request.h"

// The Connector's `lan_script` (docs/connector-protocol.md §4, docs/lan-script.md):
//   params {source, args?, timeout_ms?}
//   result {result, logs, logs_truncated?} or {error, logs, logs_truncated?}
// Runs source as the body of `function (args)` in Micro QuickJS (components/lan_script). The script
// reaches the network only through this gateway's own http_request, udp_request and lan_discover,
// with their destinations and limits, and tcp_request (lan_tcp.h), and stops at timeout_ms, the request's deadline, or when access
// is turned off. A script that throws is still a `response` (with `error`): only bad params, a
// refused request or no memory for the runtime are `error` frames.
#define LAN_SCRIPT_TIMEOUT_DEFAULT_MS 10000
#define LAN_SCRIPT_TIMEOUT_MIN_MS 1000
#define LAN_SCRIPT_TIMEOUT_MAX_MS 30000
#define LAN_SCRIPT_HEAP_BYTES CONFIG_COMMA_LAN_SCRIPT_HEAP_BYTES
#define LAN_SCRIPT_MAX_SOURCE CONFIG_COMMA_LAN_SCRIPT_MAX_SOURCE
#define LAN_SCRIPT_MAX_RESULT CONFIG_COMMA_LAN_SCRIPT_MAX_RESULT
// console.log lines kept, in all.
#define LAN_SCRIPT_MAX_LOGS 4096

// The queue's deadline for a request: its timeout_ms (clamped), in seconds rounded up.
int lan_script_timeout_seconds(const cJSON *params);
// Blocks for up to the script's timeout. Returns the whole reply frame for `id`, NUL-terminated; the
// caller frees it. NULL only when out of memory.
// session is the socket the request came in on: a script's file (params "file") comes over it.
char *lan_script_run_request(const char *id, const cJSON *params, const lan_request_t *request, unsigned session);
