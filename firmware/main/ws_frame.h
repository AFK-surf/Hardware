// SPDX-License-Identifier: Apache-2.0
#pragma once

// Whole WebSocket frames out of the pieces esp_websocket_client hands its event handler: one
// WEBSOCKET_EVENT_DATA per socket read, with the piece's offset in the frame. The Connector and the
// voice call each keep one ws_frame_t over a buffer of their own.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_websocket_client.h"

#define WS_OP_TEXT 0x1
#define WS_OP_BIN 0x2
#define WS_OP_CLOSE 0x8
#define WS_OP_PING 0x9
#define WS_OP_PONG 0xA

typedef enum {
    WS_FRAME_PENDING,  // no whole frame yet; a piece out of order drops its frame
    WS_FRAME_DONE,     // buf holds the whole frame: opcode, len bytes
    WS_FRAME_TOO_BIG,  // the first piece of a frame longer than capacity: the frame is dropped
    WS_FRAME_CONTROL,  // a PING or PONG (event->op_code), never assembled
} ws_frame_result_t;

typedef struct {
    uint8_t *buf;     // capacity + 1 bytes: room for a NUL after a whole frame
    size_t capacity;
    uint8_t opcode;   // of the frame being assembled, or just completed
    size_t len;       // its payload length
    size_t got;
    bool assembling;
} ws_frame_t;

// Takes one WEBSOCKET_EVENT_DATA. Pieces of a frame continued in another frame (FIN clear) are
// never joined: the server sends every message as one frame.
ws_frame_result_t ws_frame_feed(ws_frame_t *f, const esp_websocket_event_data_t *event);

// A new connection: forget a frame left half assembled.
static inline void ws_frame_reset(ws_frame_t *f)
{
    f->assembling = false;
}
