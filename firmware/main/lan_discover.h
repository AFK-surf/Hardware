#pragma once

#include "cJSON.h"

// The Connector's `lan_discover` (contract in docs/prompts/A5-firmware-lan-discovery.md):
//   params {sources?: ["arp","mdns","ssdp"], timeout_ms?: 1000..8000}
//   result {hosts: [{ip, mac?, hostname?, sources, services: [{type, name, port, txt?}],
//                    ssdp?: [{server, location, st}]}], truncated}
// ARP sweep of the own subnet, mDNS (a fixed list of service types plus whatever the DNS-SD meta
// query turns up, then reverse lookups for hosts still without a name), SSDP ssdp:all and Yeelight's
// own search (port 1982, reported as ST "wifi_bulb"). All of them run at once for timeout_ms.
#define LAN_DISCOVER_TIMEOUT_MIN_MS 1000
#define LAN_DISCOVER_TIMEOUT_MAX_MS 8000
#define LAN_DISCOVER_TIMEOUT_DEFAULT_MS 3000

// Blocks for about timeout_ms. Returns the whole reply frame for `id` (a `response`, or an `error`),
// NUL-terminated, in PSRAM; the caller frees it. NULL only when out of memory.
char *lan_discover_run(const char *id, const cJSON *params);
