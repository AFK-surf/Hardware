// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "firmware_config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "lan_request.h"

// The file a LAN script sends (lan_script `file`, lan.tcp `send_file`, docs/lan-script.md): the
// board asks the server for it under the lan_script request's id, receives it in `stream` frames
//   {"channel":"file","data":<base64>}  at most LAN_FILE_FRAME bytes each
//   {"channel":"file","eof":true}       after the last byte
// and acknowledges what it wrote to the device with {"channel":"file_ack","seq":<bytes so far>}
// (0 asks for the file). The server keeps at most LAN_FILE_WINDOW bytes unacknowledged, which is
// what the board buffers. One file at a time: the LAN worker runs one script.
#define LAN_FILE_WINDOW CONFIG_COMMA_LAN_SCRIPT_FILE_WINDOW
#define LAN_FILE_FRAME CONFIG_COMMA_LAN_SCRIPT_FILE_FRAME

typedef bool (*lan_file_send_fn)(unsigned session, const char *frame, size_t len);

// The Connector's way to send a frame on a socket. Call once before anything else.
void lan_file_init(lan_file_send_fn send);

// Starts receiving request `id`'s file on socket `session` and asks the server for it. False when
// out of memory or a file is already open.
bool lan_file_open(const char *id, unsigned session);

// Connector task: takes a `file` stream frame for the open file. True when the frame was consumed.
bool lan_file_frame(const char *id, const cJSON *msg, unsigned session);

// Up to cap bytes of the file, waiting for them: > 0 bytes, 0 at the end of the file, < 0 when the
// request was cancelled, its deadline passed or the server overran the window (err says which).
long lan_file_read(uint8_t *out, size_t cap, const lan_request_t *request, char *err, size_t err_size);

// Tells the server `written` bytes in all reached the device.
void lan_file_ack(size_t written);

// Ends the transfer; later frames for it are dropped.
void lan_file_close(void);
