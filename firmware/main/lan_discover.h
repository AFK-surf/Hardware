// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "cJSON.h"
#include "lan_request.h"

// The Connector's `lan_discover` (contract in docs/connector-protocol.md):
//   params {sources?: ["arp","mdns","ssdp"], timeout_ms?: 1000..8000,
//           udp_probes?: [{id, port, payload_b64}]}
//   result {hosts: [{ip, mac?, hostname?, sources, services: [{type, name, port, txt?}],
//                    ssdp?: [{server, location, st}], probes?: [{id, payload_b64, cut?}]}],
//           truncated, probes_truncated?}
// ARP sweep of the own subnet, mDNS (a fixed list of service types plus whatever the DNS-SD meta
// query turns up, then reverse lookups for hosts still without a name), SSDP ssdp:all and Yeelight's
// own search (port 1982, reported as ST "wifi_bulb"). All of them run at once for timeout_ms.
#define LAN_DISCOVER_TIMEOUT_MIN_MS 1000
#define LAN_DISCOVER_TIMEOUT_MAX_MS 8000
#define LAN_DISCOVER_TIMEOUT_DEFAULT_MS 3000

// Blocks for about timeout_ms. Returns the whole reply frame for `id` (a `response`, or an `error`),
// NUL-terminated, in PSRAM; the caller frees it. NULL only when out of memory.
char *lan_discover_run(const char *id, const cJSON *params);
// Connector variant: bounds queue + BLE wait + scan, checking cancellation throughout.
char *lan_discover_run_request(const char *id, const cJSON *params, const lan_request_t *request);
