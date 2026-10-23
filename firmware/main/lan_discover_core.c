/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Modified for the T-Circle (2026-10-06), from muse-gadget-sdk esp32/main/net_discovery.c: the
// sanitising rule and the DNS name/record parsing are ported from it; the per-host merge, caps and
// result shape follow the Comma `lan_discover` contract. Name decoding now only follows compression
// pointers backwards, and records are matched to instances by exact name, not substring.

#include "lan_discover_core.h"

#include "b64.h"
#include "firmware_config.h"
#include "json_buf.h"
#include "lan_discover.h"
#include "memory_caps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_heap_caps.h"

#define MAX_INSTANCES 160
#define MAX_ADDRS 96
#define MAX_META_TYPES 32
#define TXT_RAW_MAX 1300
#define ARENA_BYTES CONFIG_COMMA_LAN_DISCOVER_ARENA_BYTES
#define NAME_BUF 256

#define DNS_A 1
#define DNS_PTR 12
#define DNS_TXT 16
#define DNS_SRV 33

#define META_QUERY "_services._dns-sd._udp.local"

typedef struct {
    const char *type;
    const char *name;
    uint16_t port;
    const uint8_t *txt;
    uint16_t txt_len;
} svc_t;

typedef struct {
    const char *server;
    const char *location;
    const char *st;
} ssdp_t;

typedef struct {
    uint32_t ip;  // network byte order
    uint8_t mac[6];
    bool has_mac;
    uint8_t sources;
    const char *hostname;
    uint8_t nsvc;
    uint8_t nssdp;
    svc_t svc[LD_MAX_SERVICES];
    ssdp_t ssdp[LD_MAX_SSDP];
} host_t;

// An mDNS service instance, from PTR, SRV and TXT records that can arrive in different datagrams.
typedef struct {
    const char *name;  // "Living Room._wled._tcp.local"
    uint16_t first_len;
    uint32_t src_ip;
    uint16_t port;
    const char *target;
    const uint8_t *txt;
    uint16_t txt_len;
} inst_t;

typedef struct {
    const char *name;
    uint32_t ip;
} addr_t;

typedef struct {
    const char *name;
    bool handed_out;
} meta_t;

// One allocation per probing run. Pair indices follow the stable (unsorted) host table.
// Even the largest profile is below the DevKit's measured ~36 KB largest free block.
#define PROBE_SLOTS (CONFIG_COMMA_LAN_DISCOVER_PROBES > 0 ? CONFIG_COMMA_LAN_DISCOVER_PROBES : 1)
#define PROBE_SENT 1
#define PROBE_ANSWERED 2
#define PROBE_STORED 4
#define PROBE_CUT 8
#define PROBE_FAILED 16 // a send attempt failed; retried after LD_PROBE_FAIL_RETRY_MS

typedef struct {
    char id[17];
    uint16_t port;
    uint16_t payload_len;
    uint8_t payload[CONFIG_COMMA_LAN_DISCOVER_PROBE_PAYLOAD];
} probe_t;

typedef struct {
    uint32_t first_ms; // elapsed time of the first successful send
    uint16_t offset, len;
    uint16_t next_ms;  // after a failed send: not before this elapsed time
    uint8_t sends, flags; // sends counts successful datagrams only
} probe_pair_t;

typedef struct {
    unsigned count;
    size_t used;
    int sent, answered, resent, send_failed, unsent;
    bool truncated;
    probe_t probes[PROBE_SLOTS];
    probe_pair_t pairs[LD_MAX_HOSTS][PROBE_SLOTS];
    uint8_t answers[CONFIG_COMMA_LAN_DISCOVER_PROBE_BUDGET];
} probe_state_t;

struct ld_ctx {
    uint32_t self_ip;
    uint32_t netmask;
    bool truncated;
    probe_state_t *probes;
    int n_arp, n_mdns, n_ssdp;  // answers seen, for the log
    int nhosts;
    host_t *hosts;  // LD_MAX_HOSTS
    int ninst;
    inst_t inst[MAX_INSTANCES];
    int naddr;
    addr_t addr[MAX_ADDRS];
    int nmeta;
    meta_t meta[MAX_META_TYPES];
    uint8_t *arena;
    size_t arena_used;
};

static void *psram_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, COMMA_BUFFER_CAPS);
    if (p) {
        memset(p, 0, n);
    }
    return p;
}

// ---- Strings -----------------------------------------------------------------------------------

// Printable ASCII stays; tab, CR and LF become spaces; everything else is dropped. Other hosts on the
// LAN write these strings, so they must not carry line breaks or control bytes into the agent's input.
static int clean_char(uint8_t c)
{
    if (c >= 0x20 && c <= 0x7E) {
        return c;
    }
    if (c == '\n' || c == '\r' || c == '\t') {
        return ' ';
    }
    return -1;
}

// Sanitised copy of n bytes into out (out_len includes the NUL). Sets *cut when it didn't fit.
static size_t clean_copy(char *out, size_t out_len, const uint8_t *s, size_t n, bool *cut)
{
    size_t o = 0;
    for (size_t i = 0; i < n; ++i) {
        int c = clean_char(s[i]);
        if (c < 0) {
            continue;
        }
        if (o + 1 >= out_len) {
            if (cut) {
                *cut = true;
            }
            break;
        }
        out[o++] = (char)c;
    }
    out[o] = 0;
    return o;
}

static const char *arena_dup(ld_ctx_t *ctx, const void *s, size_t n, bool nul)
{
    const size_t need = n + (nul ? 1 : 0);
    if (ctx->arena_used + need > ARENA_BYTES) {
        ctx->truncated = true;
        return NULL;
    }
    char *p = (char *)ctx->arena + ctx->arena_used;
    memcpy(p, s, n);
    if (nul) {
        p[n] = 0;
    }
    ctx->arena_used += need;
    return p;
}

static const char *arena_str(ld_ctx_t *ctx, const char *s)
{
    return arena_dup(ctx, s, strlen(s), true);
}

static void ip_text(uint32_t ip, char out[16])
{
    const uint8_t *b = (const uint8_t *)&ip;
    snprintf(out, 16, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

// ---- Context -----------------------------------------------------------------------------------

ld_ctx_t *ld_create(uint32_t self_ip, uint32_t netmask)
{
    ld_ctx_t *ctx = psram_alloc(sizeof(*ctx));
    if (!ctx) {
        return NULL;
    }
    ctx->hosts = psram_alloc(sizeof(host_t) * LD_MAX_HOSTS);
    ctx->arena = heap_caps_malloc(ARENA_BYTES, COMMA_BUFFER_CAPS);
    if (!ctx->hosts || !ctx->arena) {
        ld_free(ctx);
        return NULL;
    }
    ctx->self_ip = self_ip;
    ctx->netmask = netmask;
    return ctx;
}

void ld_free(ld_ctx_t *ctx)
{
    if (ctx) {
        free(ctx->probes);
        free(ctx->hosts);
        free(ctx->arena);
        free(ctx);
    }
}

void ld_mark_truncated(ld_ctx_t *ctx)
{
    ctx->truncated = true;
}

static bool usable_ip(const ld_ctx_t *ctx, uint32_t ip)
{
    const uint8_t first = ((const uint8_t *)&ip)[0];
    return ip != 0 && ip != ctx->self_ip && ip != 0xFFFFFFFFu && first != 127 && first < 224;
}

static bool on_subnet(const ld_ctx_t *ctx, uint32_t ip)
{
    return ctx->netmask != 0 && (ip & ctx->netmask) == (ctx->self_ip & ctx->netmask);
}

static host_t *find_host(ld_ctx_t *ctx, uint32_t ip)
{
    for (int i = 0; i < ctx->nhosts; ++i) {
        if (ctx->hosts[i].ip == ip) {
            return &ctx->hosts[i];
        }
    }
    return NULL;
}

static host_t *host_for(ld_ctx_t *ctx, uint32_t ip)
{
    if (!usable_ip(ctx, ip)) {
        return NULL;
    }
    host_t *h = find_host(ctx, ip);
    if (h) {
        return h;
    }
    if (ctx->nhosts >= LD_MAX_HOSTS) {
        ctx->truncated = true;
        return NULL;
    }
    h = &ctx->hosts[ctx->nhosts++];
    memset(h, 0, sizeof(*h));
    h->ip = ip;
    return h;
}

// ---- Caller-supplied UDP probes ---------------------------------------------------------------

// Validate without allocating (out NULL); decode on the second pass into the per-run table. The
// encoded length is bounded before it is walked. Returns 1..PROBE_PAYLOAD bytes, or -1.
static int probe_decode(const char *s, uint8_t *out)
{
    const size_t n = strlen(s);
    if (n > B64_ENCODED_LEN(CONFIG_COMMA_LAN_DISCOVER_PROBE_PAYLOAD)) return -1;
    const long len = b64_decode(s, n, out, CONFIG_COMMA_LAN_DISCOVER_PROBE_PAYLOAD);
    return len > 0 ? (int)len : -1;
}

static const char *validate_probes(const cJSON *params, unsigned sources)
{
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(params, "udp_probes");
    if (!list || cJSON_IsNull(list)) return NULL;
    if (CONFIG_COMMA_LAN_DISCOVER_PROBES == 0) return "'udp_probes' is disabled on this board";
    if (!cJSON_IsArray(list) || cJSON_GetArraySize(list) < 1 ||
        cJSON_GetArraySize(list) > CONFIG_COMMA_LAN_DISCOVER_PROBES) {
        return "'udp_probes' must be a non-empty array within discover_probes.max_probes";
    }
    if (!(sources & LD_SRC_ARP)) return "'udp_probes' requires 'arp' in 'sources'";
    const cJSON *entry;
    cJSON_ArrayForEach(entry, list) {
        if (!cJSON_IsObject(entry)) return "each 'udp_probes' entry must be an object";
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(entry, "id");
        const cJSON *port = cJSON_GetObjectItemCaseSensitive(entry, "port");
        const cJSON *payload = cJSON_GetObjectItemCaseSensitive(entry, "payload_b64");
        if (!cJSON_IsString(id) || !id->valuestring[0] || strlen(id->valuestring) > 16 ||
            strspn(id->valuestring, "abcdefghijklmnopqrstuvwxyz0123456789_") != strlen(id->valuestring)) {
            return "probe 'id' must be 1..16 characters of [a-z0-9_]";
        }
        // Range-check before any integer cast, including non-finite/huge JSON numbers.
        if (!cJSON_IsNumber(port) || !(port->valuedouble >= 1 && port->valuedouble <= 65535) ||
            port->valuedouble != (double)port->valueint) {
            return "probe 'port' must be a whole number between 1 and 65535";
        }
        for (const cJSON *prev = list->child; prev != entry; prev = prev->next) {
            if (strcmp(cJSON_GetObjectItemCaseSensitive(prev, "id")->valuestring, id->valuestring) == 0)
                return "probe 'id' must be unique";
            if (cJSON_GetObjectItemCaseSensitive(prev, "port")->valueint == port->valueint)
                return "probe 'port' must be unique";
        }
        if (!cJSON_IsString(payload) || probe_decode(payload->valuestring, NULL) < 1) {
            return "probe 'payload_b64' must be standard padded base64 of 1..discover_probes.max_payload_bytes bytes";
        }
    }
    return NULL;
}

bool ld_probes_init(ld_ctx_t *ctx, const cJSON *params)
{
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(params, "udp_probes");
    if (!list || cJSON_IsNull(list)) return true;
    probe_state_t *ps = psram_alloc(sizeof(*ps));
    if (!ps) return false;
    ctx->probes = ps;
    const cJSON *entry;
    cJSON_ArrayForEach(entry, list) {
        probe_t *p = &ps->probes[ps->count++];
        strcpy(p->id, cJSON_GetObjectItemCaseSensitive(entry, "id")->valuestring);
        p->port = (uint16_t)cJSON_GetObjectItemCaseSensitive(entry, "port")->valueint;
        p->payload_len = (uint16_t)probe_decode(cJSON_GetObjectItemCaseSensitive(entry, "payload_b64")->valuestring,
                                                p->payload);
    }
    return true;
}

bool ld_has_probes(const ld_ctx_t *ctx)
{
    return ctx->probes != NULL;
}

// Probe coverage or answers only; `truncated` (hosts/entries/strings dropped) is set separately.
void ld_mark_probes_truncated(ld_ctx_t *ctx)
{
    if (ctx->probes) {
        ctx->probes->truncated = true;
    }
}

static bool probe_ip(const ld_ctx_t *ctx, uint32_t ip)
{
    return usable_ip(ctx, ip) && ((const uint8_t *)&ip)[0] != 0 && on_subnet(ctx, ip) &&
           ip != (ctx->self_ip & ctx->netmask) && ip != (ctx->self_ip | ~ctx->netmask);
}

// Priority: untried first sends, then first sends whose attempt failed, then resends. Failed pairs
// wait LD_PROBE_FAIL_RETRY_MS so they neither repeat within one batch nor starve untried pairs.
static bool probe_due(const probe_pair_t *pair, int level, uint32_t elapsed_ms)
{
    if ((pair->flags & PROBE_ANSWERED) || elapsed_ms < pair->next_ms) return false;
    const bool failed = pair->flags & PROBE_FAILED;
    switch (level) {
    case 0: return pair->sends == 0 && !failed;
    case 1: return pair->sends == 0 && failed;
    default: return pair->sends == 1 && elapsed_ms - pair->first_ms >= LD_PROBE_RETRY_MS;
    }
}

bool ld_probe_next(ld_ctx_t *ctx, uint32_t elapsed_ms, ld_probe_send_t *out)
{
    probe_state_t *ps = ctx->probes;
    if (!ps) return false;
    // A bounded scan (64 * 8 * 3) needs no queue or per-send allocation.
    for (int level = 0; level < 3; ++level) {
        for (int h = 0; h < ctx->nhosts; ++h) {
            const host_t *host = &ctx->hosts[h];
            if (!(host->sources & LD_SRC_ARP) || !probe_ip(ctx, host->ip)) continue;
            for (unsigned p = 0; p < ps->count; ++p) {
                probe_pair_t *pair = &ps->pairs[h][p];
                if (!probe_due(pair, level, elapsed_ms)) continue;
                *out = (ld_probe_send_t){.ip = host->ip, .port = ps->probes[p].port,
                                        .payload = ps->probes[p].payload, .len = ps->probes[p].payload_len,
                                        .host_index = (unsigned)h, .probe_index = p};
                return true;
            }
        }
    }
    return false;
}

void ld_probe_sent(ld_ctx_t *ctx, const ld_probe_send_t *send, uint32_t elapsed_ms, bool success)
{
    probe_state_t *ps = ctx->probes;
    probe_pair_t *pair = &ps->pairs[send->host_index][send->probe_index];
    if (success) {
        if (!pair->sends) pair->first_ms = elapsed_ms;
        pair->sends++;
        pair->flags |= PROBE_SENT;
        ps->sent++;
        if (pair->sends == 2) ps->resent++;
    } else {
        // Not lost yet: tried again on a later tick while the send window lasts.
        pair->flags |= PROBE_FAILED;
        const uint32_t next = elapsed_ms + LD_PROBE_FAIL_RETRY_MS;
        pair->next_ms = next > UINT16_MAX ? UINT16_MAX : (uint16_t)next;
        ps->send_failed++;
    }
}

void ld_add_probe_answer(ld_ctx_t *ctx, uint32_t ip, uint16_t port, const uint8_t *data, size_t len)
{
    probe_state_t *ps = ctx->probes;
    const host_t *host = find_host(ctx, ip);
    if (!ps || !host) return;
    for (unsigned p = 0; p < ps->count; ++p) {
        if (ps->probes[p].port != port) continue;
        probe_pair_t *pair = &ps->pairs[host - ctx->hosts][p];
        if (!(pair->flags & PROBE_SENT) || (pair->flags & PROBE_ANSWERED)) return;
        // The first matched datagram wins even when the budget drops it. Later smaller answers
        // cannot replace it, and a received answer never triggers a resend.
        pair->flags |= PROBE_ANSWERED;
        ps->answered++;
        size_t keep = len < CONFIG_COMMA_LAN_DISCOVER_PROBE_ANSWER ? len : CONFIG_COMMA_LAN_DISCOVER_PROBE_ANSWER;
        if (keep > CONFIG_COMMA_LAN_DISCOVER_PROBE_BUDGET - ps->used) {
            ld_mark_probes_truncated(ctx);
            return;
        }
        pair->offset = (uint16_t)ps->used;
        pair->len = (uint16_t)keep;
        pair->flags |= PROBE_STORED;
        if (keep < len) {
            pair->flags |= PROBE_CUT;
            ld_mark_probes_truncated(ctx);
        }
        if (keep) memcpy(ps->answers + ps->used, data, keep);
        ps->used += keep;
        return;
    }
}

void ld_probes_finish(ld_ctx_t *ctx)
{
    probe_state_t *ps = ctx->probes;
    if (!ps) return;
    ps->unsent = 0;
    for (int h = 0; h < ctx->nhosts; ++h) {
        if (!(ctx->hosts[h].sources & LD_SRC_ARP) || !probe_ip(ctx, ctx->hosts[h].ip)) continue;
        for (unsigned p = 0; p < ps->count; ++p) {
            const probe_pair_t *pair = &ps->pairs[h][p];
            if (!pair->sends) ps->unsent++;
            if (!pair->sends || (pair->sends < 2 && !(pair->flags & PROBE_ANSWERED))) {
                ld_mark_probes_truncated(ctx);
            }
        }
    }
}

void ld_probe_counts(const ld_ctx_t *ctx, ld_probe_counts_t *out)
{
    const probe_state_t *ps = ctx->probes;
    *out = ps ? (ld_probe_counts_t){.sent = ps->sent, .answered = ps->answered, .resent = ps->resent,
                                    .send_failed = ps->send_failed, .unsent = ps->unsent,
                                    .truncated = ps->truncated}
              : (ld_probe_counts_t){0};
}

uint32_t ld_ble_wait_limit(int timeout_ms)
{
    const int left = LD_REQUEST_DEADLINE_MS - LD_REPLY_MARGIN_MS - timeout_ms;
    return left <= 0 ? 0 : (left > LD_BLE_WAIT_MAX_MS ? LD_BLE_WAIT_MAX_MS : (uint32_t)left);
}

uint32_t ld_wait_while(bool (*busy)(void *ctx), void *ctx, void (*sleep_ms)(uint32_t), uint32_t limit_ms,
                       uint32_t step_ms, bool *still_busy)
{
    uint32_t waited = 0;
    while (busy(ctx)) {
        if (waited >= limit_ms) {
            *still_busy = true;
            return waited;
        }
        const uint32_t step = limit_ms - waited < step_ms ? limit_ms - waited : step_ms;
        sleep_ms(step);
        waited += step;
    }
    *still_busy = false;
    return waited;
}

// Encoded straight into the bounded JSON builder, 48 bytes at a time: no answer-sized string on the
// task stack. Every piece but the last is a multiple of 3 bytes, so the pieces join into one encoding.
static void write_probe_bytes(jbuf_t *b, const uint8_t *data, size_t n)
{
    char enc[B64_ENCODED_LEN(48)];
    for (size_t i = 0; i < n; i += 48) {
        jbuf_add(b, enc, b64_encode(data + i, n - i < 48 ? n - i : 48, enc));
    }
}

static void write_probes(const ld_ctx_t *ctx, jbuf_t *b, const host_t *host)
{
    const probe_state_t *ps = ctx->probes;
    if (!ps) return;
    bool open = false;
    for (unsigned p = 0; p < ps->count; ++p) {
        const probe_pair_t *pair = &ps->pairs[host - ctx->hosts][p];
        if (!(pair->flags & PROBE_STORED)) continue;
        jbuf_str(b, open ? "," : ",\"probes\":[");
        open = true;
        jbuf_str(b, "{\"id\":");
        jbuf_json_str(b, ps->probes[p].id);
        jbuf_str(b, ",\"payload_b64\":\"");
        write_probe_bytes(b, ps->answers + pair->offset, pair->len);
        jbuf_str(b, pair->flags & PROBE_CUT ? "\",\"cut\":true}" : "\"}");
    }
    if (open) jbuf_str(b, "]");
}

// NULL on success, or the error for the caller.
const char *ld_parse_params(const cJSON *params, unsigned *sources, int *timeout_ms)
{
    *sources = LD_SRC_ARP | LD_SRC_MDNS | LD_SRC_SSDP;
    *timeout_ms = LAN_DISCOVER_TIMEOUT_DEFAULT_MS;
    if (!params || cJSON_IsNull(params)) {
        return NULL;
    }
    if (!cJSON_IsObject(params)) {
        return "params must be an object";
    }
    const cJSON *src = cJSON_GetObjectItemCaseSensitive(params, "sources");
    if (src && !cJSON_IsNull(src)) {
        if (!cJSON_IsArray(src) || cJSON_GetArraySize(src) == 0) {
            return "'sources' must be a non-empty array of \"arp\", \"mdns\" and \"ssdp\"";
        }
        *sources = 0;
        const cJSON *item;
        cJSON_ArrayForEach(item, src)
        {
            const char *s = cJSON_IsString(item) ? item->valuestring : "";
            if (strcmp(s, "arp") == 0) {
                *sources |= LD_SRC_ARP;
            } else if (strcmp(s, "mdns") == 0) {
                *sources |= LD_SRC_MDNS;
            } else if (strcmp(s, "ssdp") == 0) {
                *sources |= LD_SRC_SSDP;
            } else {
                return "'sources' may only contain \"arp\", \"mdns\" and \"ssdp\"";
            }
        }
    }
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(params, "timeout_ms");
    if (t && !cJSON_IsNull(t)) {
        if (!cJSON_IsNumber(t)) {
            return "'timeout_ms' must be a number (1000-8000)";
        }
        const double v = t->valuedouble;
        *timeout_ms = v < LAN_DISCOVER_TIMEOUT_MIN_MS   ? LAN_DISCOVER_TIMEOUT_MIN_MS
                      : v > LAN_DISCOVER_TIMEOUT_MAX_MS ? LAN_DISCOVER_TIMEOUT_MAX_MS
                                                        : (int)v;
    }
    return validate_probes(params, *sources);
}

// ---- ARP ---------------------------------------------------------------------------------------

void ld_add_arp(ld_ctx_t *ctx, uint32_t ip, const uint8_t mac[6])
{
    host_t *h = host_for(ctx, ip);
    if (!h) {
        if (probe_ip(ctx, ip)) ld_mark_probes_truncated(ctx);
        return;
    }
    if (!(h->sources & LD_SRC_ARP)) {
        ctx->n_arp++;
    }
    h->sources |= LD_SRC_ARP;
    memcpy(h->mac, mac, 6);
    h->has_mac = true;
}

// ---- mDNS --------------------------------------------------------------------------------------

// Decode the name at pos. Labels are joined with '.', sanitised; *first_len is the length of the
// first label in out. Compression pointers must point backwards, so a crafted packet can't loop.
// Returns the bytes the name takes at pos, or -1.
static int dns_name(const uint8_t *pkt, size_t len, size_t pos, char *out, size_t out_len, size_t *first_len)
{
    size_t o = 0;
    int consumed = -1;
    int jumps = 0;
    size_t p = pos;
    bool first = true;
    if (first_len) {
        *first_len = 0;
    }
    for (;;) {
        if (p >= len) {
            return -1;
        }
        const uint8_t l = pkt[p];
        if (l == 0) {
            if (consumed < 0) {
                consumed = (int)(p + 1 - pos);
            }
            break;
        }
        if ((l & 0xC0) == 0xC0) {
            if (p + 1 >= len) {
                return -1;
            }
            const size_t target = ((size_t)(l & 0x3F) << 8) | pkt[p + 1];
            if (consumed < 0) {
                consumed = (int)(p + 2 - pos);
            }
            if (target >= p || ++jumps > 16) {
                return -1;
            }
            p = target;
            continue;
        }
        if (l & 0xC0) {
            return -1;
        }
        if (p + 1 + l > len) {
            return -1;
        }
        if (!first && o + 1 < out_len) {
            out[o++] = '.';
        }
        for (size_t i = 0; i < l; ++i) {
            int c = clean_char(pkt[p + 1 + i]);
            if (c >= 0 && o + 1 < out_len) {
                out[o++] = (char)c;
            }
        }
        if (first) {
            if (first_len) {
                *first_len = o;
            }
            first = false;
        }
        p += 1 + (size_t)l;
    }
    out[o] = 0;
    return consumed;
}

size_t ld_dns_question(uint8_t *buf, size_t len, const char *name, uint16_t type, uint16_t qclass)
{
    size_t o = 0;
    const char *p = name;
    while (*p) {
        const char *dot = strchr(p, '.');
        const size_t l = dot ? (size_t)(dot - p) : strlen(p);
        if (l == 0 || l > 63 || o + 1 + l >= len) {
            return 0;
        }
        buf[o++] = (uint8_t)l;
        memcpy(buf + o, p, l);
        o += l;
        p += l + (dot ? 1 : 0);
    }
    if (o + 5 > len) {
        return 0;
    }
    buf[o++] = 0;
    buf[o++] = (uint8_t)(type >> 8);
    buf[o++] = (uint8_t)type;
    buf[o++] = (uint8_t)(qclass >> 8);
    buf[o++] = (uint8_t)qclass;
    return o;
}

// "Name._type._tcp.local": the first label is followed by one that starts with '_'.
static bool is_instance_name(const char *name, size_t first_len)
{
    return first_len > 0 && name[first_len] == '.' && name[first_len + 1] == '_';
}

static inst_t *instance_for(ld_ctx_t *ctx, const char *name, size_t first_len, uint32_t src_ip)
{
    for (int i = 0; i < ctx->ninst; ++i) {
        if (strcasecmp(ctx->inst[i].name, name) == 0) {
            return &ctx->inst[i];
        }
    }
    if (ctx->ninst >= MAX_INSTANCES) {
        ctx->truncated = true;
        return NULL;
    }
    const char *copy = arena_str(ctx, name);
    if (!copy) {
        return NULL;
    }
    inst_t *in = &ctx->inst[ctx->ninst++];
    memset(in, 0, sizeof(*in));
    in->name = copy;
    in->first_len = (uint16_t)first_len;
    in->src_ip = src_ip;
    return in;
}

static void add_addr(ld_ctx_t *ctx, const char *name, uint32_t ip)
{
    for (int i = 0; i < ctx->naddr; ++i) {
        if (ctx->addr[i].ip == ip && strcasecmp(ctx->addr[i].name, name) == 0) {
            return;
        }
    }
    if (ctx->naddr >= MAX_ADDRS) {
        ctx->truncated = true;
        return;
    }
    const char *copy = arena_str(ctx, name);
    if (copy) {
        ctx->addr[ctx->naddr++] = (addr_t){.name = copy, .ip = ip};
    }
}

static void add_meta_type(ld_ctx_t *ctx, const char *type)
{
    for (int i = 0; i < ctx->nmeta; ++i) {
        if (strcasecmp(ctx->meta[i].name, type) == 0) {
            return;
        }
    }
    if (ctx->nmeta >= MAX_META_TYPES) {
        return;  // only steers follow-up queries; nothing in the result is cut
    }
    const char *copy = arena_str(ctx, type);
    if (copy) {
        ctx->meta[ctx->nmeta++] = (meta_t){.name = copy};
    }
}

// "13.1.168.192.in-addr.arpa" -> 192.168.1.13 (network byte order).
static bool reverse_ip(const char *name, uint32_t *ip)
{
    unsigned a, b, c, d;
    char rest[16];
    if (sscanf(name, "%u.%u.%u.%u.%15s", &d, &c, &b, &a, rest) != 5 || strcasecmp(rest, "in-addr.arpa") != 0 ||
        a > 255 || b > 255 || c > 255 || d > 255) {
        return false;
    }
    uint8_t *o = (uint8_t *)ip;
    o[0] = (uint8_t)a;
    o[1] = (uint8_t)b;
    o[2] = (uint8_t)c;
    o[3] = (uint8_t)d;
    return true;
}

void ld_add_mdns(ld_ctx_t *ctx, uint32_t src_ip, const uint8_t *pkt, size_t len)
{
    if (len < 12 || !(pkt[2] & 0x80)) {
        return;  // too short, or a query
    }
    ctx->n_mdns++;
    const unsigned qd = (pkt[4] << 8) | pkt[5];
    const unsigned rr = ((pkt[6] << 8) | pkt[7]) + ((pkt[8] << 8) | pkt[9]) + ((pkt[10] << 8) | pkt[11]);
    size_t pos = 12;
    char name[NAME_BUF];
    char target[NAME_BUF];
    for (unsigned q = 0; q < qd; ++q) {
        int n = dns_name(pkt, len, pos, name, sizeof(name), NULL);
        if (n < 0 || pos + (size_t)n + 4 > len) {
            return;
        }
        pos += (size_t)n + 4;
    }
    for (unsigned r = 0; r < rr; ++r) {
        size_t first_len;
        int n = dns_name(pkt, len, pos, name, sizeof(name), &first_len);
        if (n < 0 || pos + (size_t)n + 10 > len) {
            return;
        }
        pos += (size_t)n;
        const uint16_t type = (pkt[pos] << 8) | pkt[pos + 1];
        const uint32_t ttl = ((uint32_t)pkt[pos + 4] << 24) | ((uint32_t)pkt[pos + 5] << 16) | (pkt[pos + 6] << 8) |
                             pkt[pos + 7];
        const size_t rdlen = (pkt[pos + 8] << 8) | pkt[pos + 9];
        pos += 10;
        if (pos + rdlen > len) {
            return;
        }
        const size_t rd = pos;
        pos += rdlen;
        if (ttl == 0) {
            continue;  // goodbye: the record is going away
        }
        if (type == DNS_PTR) {
            size_t target_first;
            if (dns_name(pkt, len, rd, target, sizeof(target), &target_first) < 0) {
                continue;
            }
            uint32_t ip;
            if (strcasecmp(name, META_QUERY) == 0) {
                add_meta_type(ctx, target);
            } else if (reverse_ip(name, &ip)) {
                add_addr(ctx, target, ip);
            } else if (name[0] == '_' && is_instance_name(target, target_first)) {
                instance_for(ctx, target, target_first, src_ip);
            }
        } else if (type == DNS_SRV && rdlen >= 7 && is_instance_name(name, first_len)) {
            if (dns_name(pkt, len, rd + 6, target, sizeof(target), NULL) < 0) {
                continue;
            }
            inst_t *in = instance_for(ctx, name, first_len, src_ip);
            if (in) {
                in->port = (pkt[rd + 4] << 8) | pkt[rd + 5];
                if (!in->target || strcasecmp(in->target, target) != 0) {
                    in->target = arena_str(ctx, target);
                }
            }
        } else if (type == DNS_TXT && is_instance_name(name, first_len)) {
            inst_t *in = instance_for(ctx, name, first_len, src_ip);
            if (in && !in->txt && rdlen > 0) {
                const size_t keep = rdlen > TXT_RAW_MAX ? TXT_RAW_MAX : rdlen;
                in->txt = (const uint8_t *)arena_dup(ctx, pkt + rd, keep, false);
                in->txt_len = in->txt ? (uint16_t)keep : 0;
            }
        } else if (type == DNS_A && rdlen == 4) {
            uint32_t ip;
            memcpy(&ip, pkt + rd, 4);
            add_addr(ctx, name, ip);
        }
    }
}

bool ld_next_new_type(ld_ctx_t *ctx, char *out, size_t out_len)
{
    for (int i = 0; i < ctx->nmeta; ++i) {
        if (!ctx->meta[i].handed_out) {
            ctx->meta[i].handed_out = true;
            snprintf(out, out_len, "%s", ctx->meta[i].name);
            return true;
        }
    }
    return false;
}

static const addr_t *addr_named(const ld_ctx_t *ctx, const char *name, bool want_subnet)
{
    const addr_t *any = NULL;
    for (int i = 0; i < ctx->naddr; ++i) {
        if (strcasecmp(ctx->addr[i].name, name) != 0) {
            continue;
        }
        if (!want_subnet || on_subnet(ctx, ctx->addr[i].ip)) {
            return &ctx->addr[i];
        }
        any = &ctx->addr[i];
    }
    return want_subnet ? NULL : any;
}

// Fold the mDNS instances and addresses into hosts. Safe to run more than once.
static void resolve_mdns(ld_ctx_t *ctx)
{
    for (int i = 0; i < ctx->ninst; ++i) {
        const inst_t *in = &ctx->inst[i];
        // The SRV target's address on our subnet when there is one (a sleep proxy answers for other
        // hosts), otherwise whoever answered.
        uint32_t ip = in->src_ip;
        const addr_t *a = in->target ? addr_named(ctx, in->target, true) : NULL;
        if (a) {
            ip = a->ip;
        }
        host_t *h = host_for(ctx, ip);
        if (!h) {
            continue;
        }
        h->sources |= LD_SRC_MDNS;
        if (!h->hostname && in->target) {
            h->hostname = in->target;
        }
        // "Name" and "_type._tcp" out of "Name._type._tcp.local".
        char inst_name[NAME_BUF];
        char type[NAME_BUF];
        snprintf(inst_name, sizeof(inst_name), "%.*s", (int)in->first_len, in->name);
        snprintf(type, sizeof(type), "%s", in->name + in->first_len + 1);
        size_t tl = strlen(type);
        if (tl > 6 && strcasecmp(type + tl - 6, ".local") == 0) {
            type[tl - 6] = 0;
        }
        bool dup = false;
        for (int s = 0; s < h->nsvc; ++s) {
            if (strcasecmp(h->svc[s].type, type) == 0 && strcmp(h->svc[s].name, inst_name) == 0) {
                // A later run may have learnt the SRV or TXT since.
                h->svc[s].port = in->port ? in->port : h->svc[s].port;
                if (!h->svc[s].txt) {
                    h->svc[s].txt = in->txt;
                    h->svc[s].txt_len = in->txt_len;
                }
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }
        if (h->nsvc >= LD_MAX_SERVICES) {
            ctx->truncated = true;
            continue;
        }
        const char *t = arena_str(ctx, type);
        const char *n = arena_str(ctx, inst_name);
        if (!t || !n) {
            continue;
        }
        h->svc[h->nsvc++] = (svc_t){.type = t, .name = n, .port = in->port, .txt = in->txt, .txt_len = in->txt_len};
    }
    // Names for hosts found some other way: an A record or a reverse lookup with their address.
    for (int i = 0; i < ctx->naddr; ++i) {
        host_t *h = find_host(ctx, ctx->addr[i].ip);
        if (h && !h->hostname) {
            h->hostname = ctx->addr[i].name;
        }
    }
}

size_t ld_hosts_without_name(ld_ctx_t *ctx, uint32_t *ips, size_t max)
{
    resolve_mdns(ctx);
    size_t n = 0;
    for (int i = 0; i < ctx->nhosts && n < max; ++i) {
        if (!ctx->hosts[i].hostname) {
            ips[n++] = ctx->hosts[i].ip;
        }
    }
    return n;
}

// ---- SSDP --------------------------------------------------------------------------------------

void ld_add_ssdp(ld_ctx_t *ctx, uint32_t src_ip, const char *pkt, size_t len, const char *st_override)
{
    if (len < 8 || strncasecmp(pkt, "HTTP/1.", 7) != 0) {
        return;
    }
    char location[LD_STR_MAX + 1] = "";
    char st[LD_STR_MAX + 1] = "";
    char server[LD_STR_MAX + 1] = "";
    bool cut = false;
    const char *end = pkt + len;
    const char *line = memchr(pkt, '\n', len);
    while (line && line < end) {
        line++;
        const char *eol = memchr(line, '\n', (size_t)(end - line));
        const char *stop = eol ? eol : end;
        const char *colon = memchr(line, ':', (size_t)(stop - line));
        if (colon) {
            const size_t key_len = (size_t)(colon - line);
            char *dst = NULL;
            if (key_len == 8 && strncasecmp(line, "LOCATION", 8) == 0) {
                dst = location;
            } else if (key_len == 2 && strncasecmp(line, "ST", 2) == 0) {
                dst = st;
            } else if (key_len == 6 && strncasecmp(line, "SERVER", 6) == 0) {
                dst = server;
            }
            if (dst) {
                const char *v = colon + 1;
                while (v < stop && (*v == ' ' || *v == '\t')) {
                    v++;
                }
                const char *ve = stop;
                while (ve > v && (ve[-1] == '\r' || ve[-1] == ' ' || ve[-1] == '\t')) {
                    ve--;
                }
                clean_copy(dst, LD_STR_MAX + 1, (const uint8_t *)v, (size_t)(ve - v), &cut);
            }
        }
        line = eol;
    }
    if (st_override) {
        snprintf(st, sizeof(st), "%s", st_override);
    }
    if (!st[0] && !location[0]) {
        return;
    }
    ctx->n_ssdp++;
    host_t *h = host_for(ctx, src_ip);
    if (!h) {
        return;
    }
    if (cut) {
        ctx->truncated = true;
    }
    h->sources |= LD_SRC_SSDP;
    for (int i = 0; i < h->nssdp; ++i) {
        if (strcmp(h->ssdp[i].st, st) == 0 && strcmp(h->ssdp[i].location, location) == 0) {
            return;
        }
    }
    if (h->nssdp >= LD_MAX_SSDP) {
        ctx->truncated = true;
        return;
    }
    const char *a = arena_str(ctx, server);
    const char *b = arena_str(ctx, location);
    const char *c = arena_str(ctx, st);
    if (a && b && c) {
        h->ssdp[h->nssdp++] = (ssdp_t){.server = a, .location = b, .st = c};
    }
}

// ---- Result ------------------------------------------------------------------------------------

void ld_counts(ld_ctx_t *ctx, int *hosts, int *arp, int *mdns, int *ssdp, bool *truncated)
{
    resolve_mdns(ctx);
    *hosts = ctx->nhosts;
    *arp = ctx->n_arp;
    *mdns = ctx->n_mdns;
    *ssdp = ctx->n_ssdp;
    *truncated = ctx->truncated;
}

// TXT rdata as an object: "key=value" -> "key":"value", a bare "key" -> "key":"". The first of a
// repeated key wins (RFC 6763 §6.4).
static void write_txt(ld_ctx_t *ctx, jbuf_t *b, const uint8_t *txt, size_t len)
{
    struct {
        const uint8_t *p;
        size_t len;
    } keys[LD_MAX_TXT_KEYS];
    int nkeys = 0;
    size_t p = 0;
    bool open = false;
    while (p < len) {
        const size_t l = txt[p++];
        if (p + l > len) {
            ctx->truncated = true;  // cut off (TXT_RAW_MAX) or malformed
            break;
        }
        const uint8_t *s = txt + p;
        p += l;
        if (l == 0) {
            continue;
        }
        const uint8_t *eq = memchr(s, '=', l);
        const size_t kl = eq ? (size_t)(eq - s) : l;
        char key[LD_STR_MAX + 1];
        char value[LD_STR_MAX + 1];
        if (clean_copy(key, sizeof(key), s, kl, NULL) == 0) {
            continue;
        }
        bool seen = false;
        for (int k = 0; k < nkeys; ++k) {
            if (keys[k].len == kl && strncasecmp((const char *)keys[k].p, (const char *)s, kl) == 0) {
                seen = true;
                break;
            }
        }
        if (seen) {
            continue;
        }
        if (nkeys >= LD_MAX_TXT_KEYS) {
            ctx->truncated = true;
            break;
        }
        keys[nkeys].p = s;
        keys[nkeys].len = kl;
        nkeys++;
        clean_copy(value, sizeof(value), eq ? eq + 1 : s, eq ? l - kl - 1 : 0, NULL);
        jbuf_str(b, open ? "," : ",\"txt\":{");
        open = true;
        jbuf_json_str(b, key);
        jbuf_str(b, ":");
        jbuf_json_str(b, value);
    }
    if (open) {
        jbuf_str(b, "}");
    }
}

static void write_host(ld_ctx_t *ctx, jbuf_t *b, const host_t *h)
{
    char ip[16];
    ip_text(h->ip, ip);
    jbuf_str(b, "{\"ip\":");
    jbuf_json_str(b, ip);
    if (h->has_mac) {
        char mac[18];
        snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", h->mac[0], h->mac[1], h->mac[2], h->mac[3],
                 h->mac[4], h->mac[5]);
        jbuf_str(b, ",\"mac\":");
        jbuf_json_str(b, mac);
    }
    if (h->hostname) {
        jbuf_str(b, ",\"hostname\":");
        jbuf_json_str(b, h->hostname);
    }
    jbuf_str(b, ",\"sources\":[");
    static const struct {
        uint8_t bit;
        const char *name;
    } srcs[] = {{LD_SRC_ARP, "\"arp\""}, {LD_SRC_MDNS, "\"mdns\""}, {LD_SRC_SSDP, "\"ssdp\""}};
    bool comma = false;
    for (size_t i = 0; i < sizeof(srcs) / sizeof(srcs[0]); ++i) {
        if (h->sources & srcs[i].bit) {
            jbuf_str(b, comma ? "," : "");
            jbuf_str(b, srcs[i].name);
            comma = true;
        }
    }
    jbuf_str(b, "],\"services\":[");
    for (int i = 0; i < h->nsvc; ++i) {
        const svc_t *s = &h->svc[i];
        char port[8];
        snprintf(port, sizeof(port), "%u", s->port);
        jbuf_str(b, i ? ",{\"type\":" : "{\"type\":");
        jbuf_json_str(b, s->type);
        jbuf_str(b, ",\"name\":");
        jbuf_json_str(b, s->name);
        jbuf_str(b, ",\"port\":");
        jbuf_str(b, port);
        if (s->txt) {
            write_txt(ctx, b, s->txt, s->txt_len);
        }
        jbuf_str(b, "}");
    }
    jbuf_str(b, "]");
    if (h->nssdp) {
        jbuf_str(b, ",\"ssdp\":[");
        for (int i = 0; i < h->nssdp; ++i) {
            jbuf_str(b, i ? ",{\"server\":" : "{\"server\":");
            jbuf_json_str(b, h->ssdp[i].server);
            jbuf_str(b, ",\"location\":");
            jbuf_json_str(b, h->ssdp[i].location);
            jbuf_str(b, ",\"st\":");
            jbuf_json_str(b, h->ssdp[i].st);
            jbuf_str(b, "}");
        }
        jbuf_str(b, "]");
    }
    write_probes(ctx, b, h);
    jbuf_str(b, "}");
}

static uint32_t host_order(uint32_t ip)
{
    const uint8_t *b = (const uint8_t *)&ip;
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}

// {"hosts":[...],"truncated":...} for at most max hosts of order. A host that would take the object
// past LD_MAX_JSON is rolled back whole and ends the list. Returns the hosts written. Writing again
// with max = that count reproduces the same bytes: nothing written depends on the builder.
static int write_result(ld_ctx_t *ctx, jbuf_t *b, const int *order, int max)
{
    const size_t base = b->len;
    jbuf_str(b, "{\"hosts\":[");
    int written = 0;
    for (int i = 0; i < max; ++i) {
        const size_t before = b->len;
        if (written) {
            jbuf_str(b, ",");
        }
        write_host(ctx, b, &ctx->hosts[order[i]]);
        if (b->full || b->len - base + (ctx->probes ? 64 : 32) > LD_MAX_JSON) {
            b->len = before;
            b->full = false; // roll back the whole host, then append the result footer
            if (b->p) {
                b->p[before] = 0;
            }
            ctx->truncated = true;
            ld_mark_probes_truncated(ctx);
            break;
        }
        written++;
    }
    jbuf_str(b, ctx->truncated ? "],\"truncated\":true" : "],\"truncated\":false");
    if (ctx->probes && ctx->probes->truncated) jbuf_str(b, ",\"probes_truncated\":true");
    jbuf_str(b, "}");
    return written;
}

char *ld_result_frame(ld_ctx_t *ctx, const char *prefix, const char *suffix)
{
    resolve_mdns(ctx);
    int order[LD_MAX_HOSTS];
    for (int i = 0; i < ctx->nhosts; ++i) {
        order[i] = i;
    }
    for (int i = 1; i < ctx->nhosts; ++i) {
        const int v = order[i];
        int j = i;
        while (j > 0 && host_order(ctx->hosts[order[j - 1]].ip) > host_order(ctx->hosts[v].ip)) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = v;
    }
    // Measure first, then write once into an exactly sized buffer: no growth by doubling, no
    // realloc holding old and new copies at once, and no second copy for the reply envelope.
    jbuf_t m = {.limit = LD_MAX_JSON, .measure = true};
    const int kept = write_result(ctx, &m, order, ctx->nhosts);
    const size_t need = strlen(prefix) + m.len + strlen(suffix);
    jbuf_t b = {.limit = need};
    b.p = heap_caps_realloc(NULL, need + 1, COMMA_BUFFER_CAPS);
    if (!b.p) {
        return NULL;
    }
    b.cap = need + 1;
    jbuf_str(&b, prefix);
    write_result(ctx, &b, order, kept);
    jbuf_str(&b, suffix);
    if (b.oom || b.full || b.len != need) {
        jbuf_free(&b);
    }
    return b.p;
}

char *ld_result_json(ld_ctx_t *ctx)
{
    return ld_result_frame(ctx, "", "");
}
