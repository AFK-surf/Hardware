// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "firmware_config.h"

#include "cJSON.h"
#include "lan_request.h"

// `tcp_request`, for LAN scripts only (lan.tcp, docs/lan-script.md): one TCP exchange.
//   params {host, port, send_b64?, send_file?, close_write?, timeout_ms?, idle_ms?, max_bytes?}
//   result {data_b64, closed, truncated, timed_out}
// Connects, sends send_b64 and then, with send_file, the lan_script request's file (lan_file.h),
// shuts the write side when close_write is true, then reads until the host
// closes, max_bytes arrive (truncated), idle_ms pass without data or timeout_ms pass in all
// (timed_out). Same destinations as http_request (lan_dest.h).
#define LAN_TCP_TIMEOUT_DEFAULT_MS 10000
#define LAN_TCP_TIMEOUT_MAX_MS 30000
#define LAN_TCP_IDLE_DEFAULT_MS 2000
#define LAN_TCP_MAX_BYTES CONFIG_COMMA_LAN_HTTP_MAX_BYTES

// Blocks for up to timeout_ms. Returns the whole reply frame for `id` (a `response`, or an `error`),
// NUL-terminated; the caller frees it. NULL only when out of memory.
// The lan_script request whose file send_file sends: its id and socket. NULL: no file.
typedef struct {
    const char *id;
    unsigned session;
} lan_tcp_file_t;

char *lan_tcp_run_request(const char *id, const cJSON *params, const lan_request_t *request,
                          const lan_tcp_file_t *file);
