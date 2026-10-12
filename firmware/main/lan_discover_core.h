#pragma once

// The part of `lan_discover` that needs no network: params, UDP probe state, mDNS/SSDP parsing and merging with
// the ARP table into one entry per host, the caps, and the result JSON. lan_discover.c feeds it;
// tools/lan_discover_test runs it on the Mac.
//
// Announcement text comes from other hosts on the LAN, so it is sanitised as it is parsed (printable
// ASCII only; tab, CR and LF become spaces) and cut to LD_STR_MAX bytes. Probe bytes remain binary
// until base64 encoding; they never pass through the text sanitizer.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "firmware_config.h"
#include "cJSON.h"

// The caps agreed with the backend (docs/prompts/A5-firmware-lan-discovery.md). A dropped host,
// service, SSDP entry or string sets "truncated": true. Probe coverage or answers that were lost
// (unsent, not resent, cut, over the answer budget) set only "probes_truncated": true.
#define LD_MAX_HOSTS 64
#define LD_MAX_SERVICES CONFIG_COMMA_LAN_DISCOVER_MAX_SERVICES  // per host
#define LD_MAX_SSDP CONFIG_COMMA_LAN_DISCOVER_MAX_SSDP          // per host
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

// Validate all params before any network use. Missing/null probes preserve the old result bytes.
const char *ld_parse_params(const cJSON *params, unsigned *sources, int *timeout_ms);
// Call once, on a fresh context, after successful ld_parse_params. False only on allocation failure.
bool ld_probes_init(ld_ctx_t *ctx, const cJSON *params);
bool ld_has_probes(const ld_ctx_t *ctx);
// Sets probes_truncated only. A no-op without probes.
void ld_mark_probes_truncated(ld_ctx_t *ctx);
#define LD_PROBE_RETRY_MS 500
// A failed sendto is retried no sooner than this, and only while the runner's send window lasts.
#define LD_PROBE_FAIL_RETRY_MS 50

typedef struct {
    uint32_t ip; // network byte order, ARP-confirmed on the own subnet
    uint16_t port;
    const uint8_t *payload;
    size_t len;
    unsigned host_index, probe_index; // opaque pair identity, pass unchanged to ld_probe_sent
} ld_probe_send_t;
// Runner owns the pacing/deadline. Complete each returned send before asking for another.
bool ld_probe_next(ld_ctx_t *ctx, uint32_t elapsed_ms, ld_probe_send_t *out);
void ld_probe_sent(ld_ctx_t *ctx, const ld_probe_send_t *send, uint32_t elapsed_ms, bool success);
// len may be answer-cap+1 when the socket cut a larger datagram. Data is binary, never a string.
void ld_add_probe_answer(ld_ctx_t *ctx, uint32_t ip, uint16_t port, const uint8_t *data, size_t len);
// After the final ARP harvest, mark pairs whose initial send/retry did not fit the time window.
void ld_probes_finish(ld_ctx_t *ctx);
// sent includes successful resends; answered includes first matched datagrams dropped by the budget;
// send_failed counts failed sendto attempts (each retried later); unsent counts host/probe pairs
// still without a successful send at ld_probes_finish.
typedef struct {
    int sent, answered, resent, send_failed, unsent;
    bool truncated; // probes_truncated
} ld_probe_counts_t;
void ld_probe_counts(const ld_ctx_t *ctx, ld_probe_counts_t *out);

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
// The same object between prefix and suffix (the reply envelope), measured first and written into
// one exactly sized allocation. The object alone stays within LD_MAX_JSON.
char *ld_result_frame(ld_ctx_t *ctx, const char *prefix, const char *suffix);

// Append a JSON string literal of s (any bytes; control characters escaped) to a PSRAM buffer that
// ld_sb_* manage. Exposed for the reply envelope.
typedef struct {
    char *p;
    size_t len;
    size_t cap;
    size_t limit; // maximum content bytes, excluding NUL; zero means unbounded (envelopes)
    bool oom;
    bool full; // content limit reached, distinct from allocation failure
    bool measure; // count len only; p stays NULL
} ld_sb_t;
void ld_sb_add(ld_sb_t *b, const char *s, size_t n);
void ld_sb_str(ld_sb_t *b, const char *s);
void ld_sb_json(ld_sb_t *b, const char *s);
void ld_sb_free(ld_sb_t *b);

// Boards without PSRAM (CONFIG_COMMA_FREE_BLE_FOR_DISCOVERY) wait for setup BLE to end before
// discovery allocates. The backend gives a lan_discover request 15 s (salix_env protocol.ex); the
// wait leaves the scan and a reply margin inside that, and is never longer than 10 s.
#define LD_BLE_WAIT_MAX_MS 10000
#define LD_REQUEST_DEADLINE_MS 15000
#define LD_REPLY_MARGIN_MS 2000
uint32_t ld_ble_wait_limit(int timeout_ms);
// Sleep in step_ms steps while busy() is true, at most limit_ms. Returns the time slept and sets
// *still_busy when it gave up.
uint32_t ld_wait_while(bool (*busy)(void), void (*sleep_ms)(uint32_t), uint32_t limit_ms, uint32_t step_ms,
                       bool *still_busy);

// Encode a DNS question for name ("a.b.local"), type and class into buf. Returns its length, or 0
// when it doesn't fit.
size_t ld_dns_question(uint8_t *buf, size_t len, const char *name, uint16_t type, uint16_t qclass);
