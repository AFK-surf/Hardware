// SPDX-License-Identifier: Apache-2.0
#include "ws_frame.h"

#include <string.h>

ws_frame_result_t ws_frame_feed(ws_frame_t *f, const esp_websocket_event_data_t *event)
{
    if (!event) {
        return WS_FRAME_PENDING;
    }
    // Before the length check: the client reports an empty PONG as a zero-length piece.
    if (event->op_code == WS_OP_PING || event->op_code == WS_OP_PONG) {
        return WS_FRAME_CONTROL;
    }
    if (!event->data_ptr || event->data_len <= 0) {
        return WS_FRAME_PENDING;
    }
    const size_t piece = (size_t)event->data_len;
    const size_t offset = event->payload_offset > 0 ? (size_t)event->payload_offset : 0;
    const size_t total = event->payload_len > 0 ? (size_t)event->payload_len : piece;
    if (offset == 0) {
        f->opcode = event->op_code;
        f->len = total;
        f->got = 0;
        f->assembling = total <= f->capacity;
        if (!f->assembling) {
            return WS_FRAME_TOO_BIG;
        }
    }
    if (!f->assembling || offset != f->got || piece > f->len - f->got) {
        f->assembling = false;
        return WS_FRAME_PENDING;
    }
    memcpy(f->buf + offset, event->data_ptr, piece);
    f->got += piece;
    if (f->got < f->len || !event->fin) {
        return WS_FRAME_PENDING;
    }
    f->assembling = false;
    return WS_FRAME_DONE;
}
