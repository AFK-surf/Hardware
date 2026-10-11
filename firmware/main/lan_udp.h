#include "firmware_config.h"
#pragma once

#include "cJSON.h"

// The Connector's `udp_request` (contract in docs/prompts/C2-backend-lan-request-tool.md):
//   params {host, port, payload_b64, timeout_ms?, max_responses?}
//   result {responses: [{from, payload_b64}], timed_out}
// Sends one datagram from a fresh socket and collects answers until max_responses have arrived or
// timeout_ms has passed (timed_out: true then). The device carries bytes only; the cloud builds and
// parses the protocol (miIO and the like), so device keys never reach the board.
//
// Same destinations as http_request (lan_dest.h): no broadcast or multicast. Only answers from the
// destination's address count; datagrams from any other host are ignored. `from` is "ip:port".
#define LAN_UDP_TIMEOUT_DEFAULT_MS 2000
#define LAN_UDP_TIMEOUT_MIN_MS 100
#define LAN_UDP_TIMEOUT_MAX_MS 10000
#define LAN_UDP_MAX_RESPONSES 16
// One datagram that fits a Wi-Fi frame without IP fragmentation.
#define LAN_UDP_MAX_PAYLOAD 1472
// Longest answer kept; a longer datagram is cut to this by the socket.
#define LAN_UDP_MAX_ANSWER CONFIG_COMMA_LAN_UDP_MAX_ANSWER

// Blocks for up to timeout_ms. Returns the whole reply frame for `id` (a `response`, or an `error`),
// NUL-terminated; the caller frees it. NULL only when out of memory.
char *lan_udp_run(const char *id, const cJSON *params);
