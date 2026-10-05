#pragma once

#include "cJSON.h"

// The Connector's `http_request` (docs/connector-protocol.md §4), with the result shape of
// salix-connect's methodHTTPRequest. Only LAN destinations: RFC 1918, link-local (169.254/16) and
// the device's own subnet. Loopback, the device itself, multicast and public addresses are refused,
// because the cloud already reaches the internet and this device exists for the LAN.
//
// Device limits: timeout_seconds is capped at LAN_HTTP_TIMEOUT_MAX_S, max_bytes at
// LAN_HTTP_MAX_BYTES (longer bodies come back with "truncated": true). No redirects.
#define LAN_HTTP_TIMEOUT_MAX_S 30
#define LAN_HTTP_MAX_BYTES (64 * 1024)

// Blocks for up to the request's timeout. Returns the whole reply frame for `id` (a `response`, or
// an `error` with a short message), NUL-terminated, in PSRAM; the caller frees it. NULL only when
// out of memory.
char *lan_http_run(const char *id, const cJSON *params);
