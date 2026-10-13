// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>

// Gets the device off a Wi-Fi link that shows as connected but passes no traffic. On 2026-10-06 the
// board stayed associated (RSSI -56..-62, no disconnect event) while nothing reached even the
// router, until a reset. Every 10 s while Wi-Fi is up it pings the router; 3 misses in a row, or 3
// Connector dials in a row that never got a connection, make it drop and rejoin Wi-Fi. If the
// router still doesn't answer after that, the device restarts.
void net_watchdog_start(void);

// The Connector reports each dial: opened = the websocket came up; http = handshake status (0 when
// the TCP/TLS connection itself failed).
void net_watchdog_dial_result(bool opened, int http);
