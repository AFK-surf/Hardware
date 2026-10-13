// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Where http_request and udp_request may go: RFC 1918, link-local (169.254/16) and the device's own
// subnet when it is /16 or smaller. Loopback, the device itself, 0/8, multicast/broadcast/reserved,
// the own subnet's network and broadcast addresses and public addresses are refused. Resolve once
// and connect to that IP, so a second lookup can't rebind the name.
//
// err must hold LAN_DEST_ERR_MAX bytes; it gets a message meant for the agent.
#define LAN_DEST_ERR_MAX 400

// Strict dotted quad, or DNS (IPv4 only). ip in network byte order.
bool lan_dest_resolve(const char *host, uint32_t *ip, char *err);

// ip (network byte order) is a LAN destination.
bool lan_dest_check(uint32_t ip, char *err);
