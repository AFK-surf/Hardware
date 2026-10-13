// SPDX-License-Identifier: Apache-2.0
// The LAN destination rule shared by http_request and udp_request.

#include "lan_dest.h"

#include "wifi.h"

#include <stdio.h>
#include <string.h>

#include "lwip/inet.h"
#include "lwip/netdb.h"

// Strict a.b.c.d (no octal, hex or short forms). Network byte order.
static bool parse_ipv4(const char *s, uint32_t *out)
{
    uint32_t v = 0;
    for (int part = 0; part < 4; ++part) {
        if (*s < '0' || *s > '9' || (s[0] == '0' && s[1] >= '0' && s[1] <= '9')) {
            return false;
        }
        unsigned n = 0;
        while (*s >= '0' && *s <= '9') {
            n = n * 10 + (unsigned)(*s++ - '0');
            if (n > 255) {
                return false;
            }
        }
        v = (v << 8) | n;
        if (part < 3 && *s++ != '.') {
            return false;
        }
    }
    if (*s) {
        return false;
    }
    *out = htonl(v);
    return true;
}

bool lan_dest_resolve(const char *host, uint32_t *ip, char *err)
{
    if (parse_ipv4(host, ip)) {
        return true;
    }
    const struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) {
        snprintf(err, LAN_DEST_ERR_MAX, "could not resolve %s", host);
        return false;
    }
    *ip = ((const struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(res);
    return true;
}

static int prefix_len(uint32_t mask)
{
    int n = 0;
    while (mask & 0x80000000u) {
        n++;
        mask <<= 1;
    }
    return n;
}

bool lan_dest_check(uint32_t ip, char *err)
{
    uint32_t self_n = 0;
    uint32_t mask_n = 0;
    if (!wifi_ip_info(&self_n, &mask_n)) {
        snprintf(err, LAN_DEST_ERR_MAX, "the device is not on a network right now");
        return false;
    }
    const uint32_t a = ntohl(ip);
    const uint32_t self = ntohl(self_n);
    const uint32_t mask = ntohl(mask_n);
    char shown[16];
    snprintf(shown, sizeof(shown), "%u.%u.%u.%u", (unsigned)(a >> 24), (unsigned)(a >> 16) & 0xFF,
             (unsigned)(a >> 8) & 0xFF, (unsigned)a & 0xFF);
    const char *why = NULL;
    // Only a reasonably small own subnet counts as LAN, so a /8 of public space doesn't.
    const bool own_subnet = prefix_len(mask) >= 16 && (a & mask) == (self & mask);
    const bool is_private = (a >> 24) == 10 || (a >> 20) == 0xAC1 || (a >> 16) == 0xC0A8 || (a >> 16) == 0xA9FE;
    if ((a >> 24) == 127) {
        why = "a loopback address";
    } else if (a == self) {
        why = "the T-Circle itself";
    } else if ((a >> 24) == 0) {
        why = "not a valid destination";
    } else if (a >= 0xE0000000u) {
        why = "a multicast, broadcast or reserved address";
    } else if (own_subnet && prefix_len(mask) <= 30 && (a == (self & mask) || a == (self | ~mask))) {
        why = "the network or broadcast address of the local subnet";
    } else if (!is_private && !own_subnet) {
        snprintf(err, LAN_DEST_ERR_MAX,
                 "%s is a public address. The T-Circle only reaches the local network (private, link-local or its "
                 "own subnet); use web.http_request for the internet",
                 shown);
        return false;
    }
    if (why) {
        snprintf(err, LAN_DEST_ERR_MAX, "%s is %s; the T-Circle only reaches other devices on the local network", shown, why);
        return false;
    }
    return true;
}
