#pragma once

// The part of `lan_discover` that needs no network: parsing mDNS and SSDP answers, merging them with
// the ARP table into one entry per host, the caps, and the result JSON. lan_discover.c feeds it;
// tools/lan_discover_test runs it on the Mac.
//
// Every string here comes from other hosts on the LAN, so it is sanitised as it is parsed (printable
// ASCII only; tab, CR and LF become spaces) and cut to LD_STR_MAX bytes.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "firmware_config.h"

// The caps agreed with the backend (docs/prompts/A5-firmware-lan-discovery.md). Anything cut sets
// "truncated": true.
#define LD_MAX_HOSTS 64
#define LD_MAX_SERVICES 16  // per host
#define LD_MAX_SSDP 16      // per host
#define LD_MAX_TXT_KEYS 16  // per service
#define LD_STR_MAX 255      // bytes, every string
// Not in the contract: the whole result stays under this, so a busy network can't produce a reply
// the agent can't use. Hosts that don't fit are left out (truncated).
#define LD_MAX_JSON CONFIG_COMMA_LAN_DISCOVER_MAX_JSON

#define LD_SRC_ARP 0x1
#define LD_SRC_MDNS 0x2
#define LD_SRC_SSDP 0x4

typedef struct ld_ctx ld_ctx_t;

// self_ip and netmask in network byte order; answers from self are ignored. NULL when out of memory.
ld_ctx_t *ld_create(uint32_t self_ip, uint32_t netmask);
void ld_free(ld_ctx_t *ctx);

// An ARP table entry. ip in network byte order.
void ld_add_arp(ld_ctx_t *ctx, uint32_t ip, const uint8_t mac[6]);

// One mDNS datagram (answers and additional records are both read). src_ip in network byte order.
void ld_add_mdns(ld_ctx_t *ctx, uint32_t src_ip, const uint8_t *pkt, size_t len);

// One SSDP (or Yeelight) answer. st_override, when set, replaces the ST header: Yeelight's answers
// have none, and its search asks for "wifi_bulb".
void ld_add_ssdp(ld_ctx_t *ctx, uint32_t src_ip, const char *pkt, size_t len, const char *st_override);

// Something was cut outside the core (e.g. the ARP sweep didn't cover a large subnet).
void ld_mark_truncated(ld_ctx_t *ctx);

// Service types learnt from the DNS-SD meta query ("_services._dns-sd._udp.local") that haven't been
// handed out yet, one per call, as "_type._tcp.local". False when there are none.
bool ld_next_new_type(ld_ctx_t *ctx, char *out, size_t out_len);

// Up to max hosts (network byte order) that have no hostname yet, for reverse lookups.
size_t ld_hosts_without_name(ld_ctx_t *ctx, uint32_t *ips, size_t max);

// Counts for the log line.
void ld_counts(ld_ctx_t *ctx, int *hosts, int *arp, int *mdns, int *ssdp, bool *truncated);

// The result object, {"hosts":[...],"truncated":bool}, hosts sorted by address. NUL-terminated, in
// PSRAM; the caller frees it. NULL when out of memory.
char *ld_result_json(ld_ctx_t *ctx);

// Append a JSON string literal of s (any bytes; control characters escaped) to a PSRAM buffer that
// ld_sb_* manage. Exposed for the reply envelope.
typedef struct {
    char *p;
    size_t len;
    size_t cap;
    size_t limit; // maximum content bytes, excluding NUL; zero means unbounded (envelopes)
    bool oom;
    bool full; // content limit reached, distinct from allocation failure
} ld_sb_t;
void ld_sb_add(ld_sb_t *b, const char *s, size_t n);
void ld_sb_str(ld_sb_t *b, const char *s);
void ld_sb_json(ld_sb_t *b, const char *s);
void ld_sb_free(ld_sb_t *b);

// Encode a DNS question for name ("a.b.local"), type and class into buf. Returns its length, or 0
// when it doesn't fit.
size_t ld_dns_question(uint8_t *buf, size_t len, const char *name, uint16_t type, uint16_t qclass);
