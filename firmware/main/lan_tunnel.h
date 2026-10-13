// SPDX-License-Identifier: Apache-2.0
#include "firmware_config.h"
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"

// The Connector's `lan_tcp_dial`: one TCP connection from the T-Circle to a LAN host, relayed
// over the Connector socket. Same protocol as the backend's other Connector clients, so the
// backend needs no special case:
//
//   request  {"method":"lan_tcp_dial","params":{"host":"192.168.1.33","port":554,"timeout_seconds":15}}
//   answer   {"type":"response","result":{"status":"connected","remote_address":"ip:port",
//                                          "window":LAN_TUNNEL_WINDOW,"max_frame":LAN_TUNNEL_MAX_FRAME}}
//            or an `error` frame (refused, timeout, not permitted, busy)
//   then `stream` frames under the request id, both ways:
//            {"channel":"tcp","data":<base64>}  bytes
//            {"channel":"tcp","eof":true}       either side closed ("error" on failure)
//            {"channel":"tcp_ack","seq":<n>}    the receiver has written n bytes in total
//
// A sender keeps at most one window unacknowledged. Same LAN destinations as http_request
// (lan_dest.h). A tunnel ends on eof from either side, when the LAN socket closes, after
// LAN_TUNNEL_IDLE_S without data either way, or when the host's check() says so (Connector socket
// gone, access turned read-only, voice call started).

// Two at a time: an RTSP snapshot needs one; the second lets a retry or a second consumer start
// while the first is still closing. Each costs one lwIP socket (10 in all) and up to
// LAN_TUNNEL_SOCKET_WINDOW of Wi-Fi RX buffers.
#define LAN_TUNNEL_MAX_OPEN CONFIG_COMMA_LAN_TUNNELS
// Raw bytes per device→server data frame: ~11 KB of JSON, always under the 12 KB the Connector
// socket's writable gate guarantees, so a frame write never blocks holding the TX lock.
#define LAN_TUNNEL_MAX_FRAME CONFIG_COMMA_LAN_TUNNEL_MAX_FRAME
// The server ignores the device's max_frame and sends up to 32 KB per frame (DeviceRelay @max_frame).
#define LAN_TUNNEL_SERVER_FRAME_MAX (32 * 1024)
// Unacknowledged bytes per direction. Device→server it bounds data in flight (held by the server and
// the network, not the device): 128 KB over the ~250 ms Cloudflare round trip allows ~500 KB/s, above
// the ~245 KB/s the Connector socket managed in chunk-mode tests, so the window is never the bottleneck.
// Server→device it bounds what can queue here (PSRAM, as it arrives).
#define LAN_TUNNEL_WINDOW CONFIG_COMMA_LAN_TUNNEL_WINDOW
#define LAN_TUNNEL_DIAL_TIMEOUT_MAX_S 15
#ifndef LAN_TUNNEL_IDLE_S
#define LAN_TUNNEL_IDLE_S 300
#endif
// lwIP receive window of each tunnel's LAN socket (the global one is 64 KB for the voice socket).
// Received data that the device hasn't read holds Wi-Fi RX buffers (zero-copy RX), so this
// caps what a stalled tunnel pins at ~11 buffers. Still ~3 MB/s on the LAN.
// 0 turns the cut off (dev builds: -DLAN_TUNNEL_SOCKET_WINDOW=0).
#ifndef LAN_TUNNEL_SOCKET_WINDOW
#define LAN_TUNNEL_SOCKET_WINDOW (16 * 1024)
#endif

typedef enum {
    LAN_TUNNEL_GO = 0,
    LAN_TUNNEL_END,         // end with this reason: eof + error, or an error answer while dialing
    LAN_TUNNEL_END_SILENT,  // the Connector socket is gone: nothing can be sent
} lan_tunnel_verdict_t;

typedef struct {
    // Sends one whole frame on the Connector socket `session`, blocking until it is handed to the
    // socket. False when it can't (socket gone, no progress); the tunnel then ends.
    bool (*send)(void *ctx, unsigned session, const char *frame, size_t len);
    // Whether tunnels on `session` may go on (at dial time and on every pass of the loop).
    lan_tunnel_verdict_t (*check)(void *ctx, unsigned session, const char **reason);
    void *ctx;
} lan_tunnel_host_t;

#if CONFIG_COMMA_LAN_TUNNELS > 0
// Starts the tunnel task. host is copied.
bool lan_tunnel_start(const lan_tunnel_host_t *host);

// Connector task. Hands a lan_tcp_dial request (params may be NULL) to the tunnel task, which
// answers it. False when the hand-off queue is full: the caller answers busy.
bool lan_tunnel_dial(const char *id, const cJSON *params, unsigned session);

// Connector task. A `stream` frame: true when it belongs to the tunnel protocol (channel tcp or
// tcp_ack), whether or not a tunnel with that id exists.
bool lan_tunnel_frame(const char *id, const cJSON *msg, unsigned session);

// Tunnels dialing or open.
int lan_tunnel_count(void);

// One pass of the tunnel task's loop: waits up to wait_ms for work. Exposed for host tests.
void lan_tunnel_step(int wait_ms);

// Standard base64 with padding (RFC 4648 §4). encode writes 4 * ceil(n / 3) bytes, no NUL. decode is
// strict (padding required, no whitespace) and returns the decoded length, or -1.
size_t lan_tunnel_b64_encode(const unsigned char *in, size_t n, char *out);
long lan_tunnel_b64_decode(const char *in, size_t n, unsigned char *out, size_t cap);
#else
static inline int lan_tunnel_count(void) { return 0; }
static inline bool lan_tunnel_frame(const char *id, const cJSON *msg, unsigned session)
{ (void)id; (void)msg; (void)session; return false; }
#endif
