#pragma once

#include <stdbool.h>
#include <stddef.h>

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

// Chunk mode (params "response_mode": "chunks", docs/connector-protocol.md §4): the body is binary
// and streams out as `response_chunk` frames of at most LAN_HTTP_CHUNK_BYTES raw bytes each, up to
// LAN_HTTP_CHUNK_MAX_TOTAL in all; the device never holds more than one frame of it. Advertised in
// metadata as capabilities.lan_gateway.response_chunks.
#define LAN_HTTP_CHUNK_BYTES (32 * 1024)
#define LAN_HTTP_CHUNK_MAX_TOTAL (4 * 1024 * 1024)
// A big body can take a while to stream, so chunk mode allows a longer timeout_seconds. The server
// waits at most 120 s for the final frame.
#define LAN_HTTP_CHUNK_TIMEOUT_MAX_S 110

// Where chunk frames go. All calls are made from inside lan_http_run, in order.
typedef struct {
    // Non-NULL: refuse this chunk-mode request with that message, before anything touches the
    // network (e.g. "busy: voice call active"). NULL (or no callback): go ahead.
    const char *(*refuse)(void *ctx);
    // Raw body bytes the next frame should carry: 1..LAN_HTTP_CHUNK_BYTES.
    size_t (*frame_bytes)(void *ctx);
    // Sends one whole frame (JSON text, len bytes) carrying raw_bytes of the body, blocking until it
    // has been handed to the socket. False when it can't (socket closed, no progress for 10 s, access
    // turned off): err says why, and the transfer ends with an `error` frame.
    bool (*send)(void *ctx, const char *frame, size_t len, size_t raw_bytes, char *err, size_t err_size);
    void *ctx;
} lan_http_sink_t;

// Blocks for up to the request's timeout. Returns the whole reply frame for `id` (a `response`, or
// an `error` with a short message), NUL-terminated, in PSRAM; the caller frees it and sends it after
// any chunk frames. NULL only when out of memory. sink may be NULL; a chunk-mode request is then
// refused.
char *lan_http_run(const char *id, const cJSON *params, const lan_http_sink_t *sink);
