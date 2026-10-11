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

// Modified for the T-Circle (2026-10-06), from muse-gadget-sdk esp32/main/net_discovery.c: the ARP
// sweep-and-harvest, the mDNS service list and the SSDP M-SEARCH are ported from it. Changes: all
// probes run at once within one timeout instead of one after another; ARP goes through
// tcpip_api_call because this build has no lwIP core locking; mDNS also asks the DNS-SD meta query
// and reverse names; Yeelight's own search (port 1982) is added; results go to lan_discover_core.c.

#include "lan_discover.h"

#include "lan_discover_core.h"
#include "wifi.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/etharp.h"
#include "lwip/sockets.h"
#include "lwip/priv/tcpip_priv.h"
#include "lwip/tcpip.h"

#define SRC_ARP LD_SRC_ARP
#define SRC_MDNS LD_SRC_MDNS
#define SRC_SSDP LD_SRC_SSDP

// The sweep covers at most this many addresses around the device's own (a /24).
#define ARP_SWEEP_MAX 254
#define ARP_TICK_MS 50
#define ARP_BATCH_MAX 16
#define LOOP_TICK_MS 25
#define RECV_BUF 2048

#define MDNS_PORT 5353
#define SSDP_PORT 1900
#define YEELIGHT_PORT 1982
#define DNS_PTR 12
#define DNS_CLASS_IN_QU 0x8001  // IN, and ask for a unicast answer
#define REVERSE_MAX 64
#define REVERSE_PER_PACKET 12

static const char *TAG = "lan_discover";

// Common smart-home and computer service types. Others turn up through the DNS-SD meta query.
static const char *const MDNS_TYPES[] = {
    "_http._tcp.local",    "_hap._tcp.local",        "_googlecast._tcp.local", "_airplay._tcp.local",
    "_raop._tcp.local",    "_sonos._tcp.local",      "_hue._tcp.local",        "_miio._udp.local",
    "_wled._tcp.local",    "_esphomelib._tcp.local", "_shelly._tcp.local",     "_matter._tcp.local",
    "_ipp._tcp.local",     "_printer._tcp.local",    "_smb._tcp.local",        "_companion-link._tcp.local",
    "_spotify-connect._tcp.local",
};

// ---- Params and frames -------------------------------------------------------------------------

static char *error_frame(const char *id, const char *message)
{
    ld_sb_t b = {0};
    ld_sb_str(&b, "{\"id\":");
    ld_sb_json(&b, id);
    ld_sb_str(&b, ",\"type\":\"error\",\"error\":");
    ld_sb_json(&b, message);
    ld_sb_str(&b, "}");
    if (b.oom) {
        ld_sb_free(&b);
    }
    return b.p;
}

// NULL on success, or the error for the caller.
static const char *parse_params(const cJSON *params, unsigned *sources, int *timeout_ms)
{
    *sources = SRC_ARP | SRC_MDNS | SRC_SSDP;
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
                *sources |= SRC_ARP;
            } else if (strcmp(s, "mdns") == 0) {
                *sources |= SRC_MDNS;
            } else if (strcmp(s, "ssdp") == 0) {
                *sources |= SRC_SSDP;
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
    return NULL;
}

// ---- ARP (on the tcpip thread: no core locking in this build) ----------------------------------

typedef struct {
    struct tcpip_api_call_data call;
    struct netif *nif;
    uint32_t first;  // host byte order
    int count;
    uint32_t skip;  // the device's own address, host byte order
    int sent;
    int failed;
} arp_send_t;

static err_t arp_send_on_tcpip(struct tcpip_api_call_data *call)
{
    arp_send_t *a = (arp_send_t *)call;
    for (int i = 0; i < a->count; ++i) {
        const uint32_t host = a->first + (uint32_t)i;
        if (host == a->skip) {
            continue;
        }
        ip4_addr_t target = {.addr = lwip_htonl(host)};
        if (etharp_request(a->nif, &target) == ERR_OK) {
            a->sent++;
        } else {
            a->failed++;  // usually pbuf exhaustion; the host may still show up later
        }
    }
    return ERR_OK;
}

typedef struct {
    struct tcpip_api_call_data call;
    struct netif *nif;
    int n;
    struct {
        uint32_t ip;  // network byte order
        uint8_t mac[6];
    } e[ARP_TABLE_SIZE];
} arp_dump_t;

static err_t arp_dump_on_tcpip(struct tcpip_api_call_data *call)
{
    arp_dump_t *d = (arp_dump_t *)call;
    d->n = 0;
    for (size_t i = 0; i < ARP_TABLE_SIZE; ++i) {
        ip4_addr_t *ip;
        struct netif *nif;
        struct eth_addr *mac;
        if (etharp_get_entry(i, &ip, &nif, &mac) && nif == d->nif && ip && mac) {
            d->e[d->n].ip = ip->addr;
            memcpy(d->e[d->n].mac, mac->addr, 6);
            d->n++;
        }
    }
    return ERR_OK;
}

static void arp_harvest(ld_ctx_t *ctx, arp_dump_t *dump)
{
    tcpip_api_call(arp_dump_on_tcpip, &dump->call);
    for (int i = 0; i < dump->n; ++i) {
        ld_add_arp(ctx, dump->e[i].ip, dump->e[i].mac);
    }
}

// ---- UDP ---------------------------------------------------------------------------------------

static int udp_socket(uint16_t bind_port, bool join_mdns)
{
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) {
        return -1;
    }
    if (bind_port) {
        int on = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(bind_port), .sin_addr.s_addr = INADDR_ANY};
        if (bind(s, (struct sockaddr *)&a, sizeof(a)) != 0) {
            // Something else holds 5353. An ephemeral port still gets unicast ("legacy") answers.
            ESP_LOGW(TAG, "bind %u failed (%d), using an ephemeral port", bind_port, errno);
        }
    }
    if (join_mdns) {
        struct ip_mreq mr = {.imr_multiaddr.s_addr = inet_addr("224.0.0.251"), .imr_interface.s_addr = INADDR_ANY};
        setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr, sizeof(mr));
        uint8_t ttl = 255;  // RFC 6762 §11
        setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    }
    return s;
}

static void send_to(int s, const char *group, uint16_t port, const void *data, size_t len)
{
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = inet_addr(group)};
    if (sendto(s, data, len, 0, (struct sockaddr *)&to, sizeof(to)) < 0) {
        ESP_LOGD(TAG, "sendto %s:%u failed (%d)", group, port, errno);
    }
}

// One PTR question per packet, as Muse does: some small responders only read the first question.
static void mdns_ask(int s, const char *name)
{
    uint8_t q[300] = {0};
    q[5] = 1;  // QDCOUNT
    const size_t n = ld_dns_question(q + 12, sizeof(q) - 12, name, DNS_PTR, DNS_CLASS_IN_QU);
    if (n) {
        send_to(s, "224.0.0.251", MDNS_PORT, q, 12 + n);
    }
}

// Reverse names for hosts that have none yet, several questions per packet (computers answer these;
// a small device that only reads the first question just doesn't name itself).
static void mdns_ask_reverse(int s, ld_ctx_t *ctx)
{
    uint32_t ips[REVERSE_MAX];
    const size_t n = ld_hosts_without_name(ctx, ips, REVERSE_MAX);
    uint8_t q[12 + REVERSE_PER_PACKET * 36];
    for (size_t i = 0; i < n;) {
        memset(q, 0, 12);
        size_t off = 12;
        int count = 0;
        for (; i < n && count < REVERSE_PER_PACKET; ++i) {
            const uint8_t *b = (const uint8_t *)&ips[i];
            char name[32];
            snprintf(name, sizeof(name), "%u.%u.%u.%u.in-addr.arpa", b[3], b[2], b[1], b[0]);
            const size_t len = ld_dns_question(q + off, sizeof(q) - off, name, DNS_PTR, DNS_CLASS_IN_QU);
            if (!len) {
                break;
            }
            off += len;
            count++;
        }
        if (count == 0) {
            break;
        }
        q[5] = (uint8_t)count;
        send_to(s, "224.0.0.251", MDNS_PORT, q, off);
    }
    if (n) {
        ESP_LOGI(TAG, "asked reverse names for %u hosts", (unsigned)n);
    }
}

static void ssdp_search(int ssdp, int yeelight, int timeout_ms)
{
    if (ssdp >= 0) {
        int mx = timeout_ms / 1000 - 1;
        mx = mx < 1 ? 1 : (mx > 5 ? 5 : mx);
        char msg[160];
        const int n = snprintf(msg, sizeof(msg),
                               "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\n"
                               "MX: %d\r\nST: ssdp:all\r\n\r\n",
                               mx);
        send_to(ssdp, "239.255.255.250", SSDP_PORT, msg, (size_t)n);
    }
    if (yeelight >= 0) {
        static const char msg[] = "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1982\r\nMAN: \"ssdp:discover\"\r\n"
                                  "ST: wifi_bulb\r\n\r\n";
        send_to(yeelight, "239.255.255.250", YEELIGHT_PORT, msg, sizeof(msg) - 1);
    }
}

// Read everything waiting on s.
static void drain(int s, int kind, ld_ctx_t *ctx, uint8_t *buf)
{
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        const int n = recvfrom(s, buf, RECV_BUF - 1, MSG_DONTWAIT, (struct sockaddr *)&from, &fl);
        if (n <= 0) {
            return;
        }
        buf[n] = 0;
        if (kind == MDNS_PORT) {
            ld_add_mdns(ctx, from.sin_addr.s_addr, buf, (size_t)n);
        } else {
            ld_add_ssdp(ctx, from.sin_addr.s_addr, (const char *)buf, (size_t)n,
                        kind == YEELIGHT_PORT ? "wifi_bulb" : NULL);
        }
    }
}

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

// ---- Run ---------------------------------------------------------------------------------------

char *lan_discover_run(const char *id, const cJSON *params)
{
    unsigned sources;
    int timeout_ms;
    const char *bad = parse_params(params, &sources, &timeout_ms);
    if (bad) {
        return error_frame(id, bad);
    }
    uint32_t self_ip, netmask;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    struct netif *nif = netif ? esp_netif_get_netif_impl(netif) : NULL;
    if (!wifi_ip_info(&self_ip, &netmask) || !nif) {
        return error_frame(id, "the T-Circle is not connected to Wi-Fi right now");
    }
    ld_ctx_t *ctx = ld_create(self_ip, netmask);
    uint8_t *buf = heap_caps_malloc(RECV_BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    arp_dump_t *dump = heap_caps_malloc(sizeof(arp_dump_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ctx || !buf || !dump) {
        ld_free(ctx);
        free(buf);
        free(dump);
        return error_frame(id, "the T-Circle is out of memory for discovery");
    }
    memset(dump, 0, sizeof(*dump));
    dump->nif = nif;

    const int64_t start = now_ms();
    const int64_t deadline = start + timeout_ms;

    // ARP: the own subnet, at most ARP_SWEEP_MAX addresses around the device's own.
    const uint32_t own = lwip_ntohl(self_ip);
    const uint32_t mask = lwip_ntohl(netmask);
    const uint32_t net = own & mask;
    const uint32_t bcast = net | ~mask;
    uint32_t sweep_first = net + 1;
    uint32_t sweep_count = bcast > net + 1 ? bcast - net - 1 : 0;
    if (sweep_count > ARP_SWEEP_MAX) {
        sweep_first = own - ARP_SWEEP_MAX / 2;
        if (sweep_first <= net) {
            sweep_first = net + 1;
        } else if (sweep_first + ARP_SWEEP_MAX > bcast) {
            sweep_first = bcast - ARP_SWEEP_MAX;
        }
        sweep_count = ARP_SWEEP_MAX;
        if (sources & SRC_ARP) {
            ld_mark_truncated(ctx);  // the rest of a large subnet isn't swept
        }
    }
    // Spread the sweep over the first ~55 % of the time, so replies have time to land.
    const int sweep_ms = timeout_ms * 55 / 100;
    const int ticks = sweep_ms / ARP_TICK_MS > 0 ? sweep_ms / ARP_TICK_MS : 1;
    int batch = (int)((sweep_count + (uint32_t)ticks - 1) / (uint32_t)ticks);
    batch = batch < 1 ? 1 : (batch > ARP_BATCH_MAX ? ARP_BATCH_MAX : batch);
    uint32_t swept = 0;
    int arp_sent = 0, arp_failed = 0;
    int64_t next_arp = start;

    int mdns = (sources & SRC_MDNS) ? udp_socket(MDNS_PORT, true) : -1;
    int ssdp = (sources & SRC_SSDP) ? udp_socket(0, false) : -1;
    int yeelight = (sources & SRC_SSDP) ? udp_socket(0, false) : -1;
    if (((sources & SRC_MDNS) && mdns < 0) || ((sources & SRC_SSDP) && (ssdp < 0 || yeelight < 0))) {
        ESP_LOGW(TAG, "out of sockets (mdns %d, ssdp %d, yeelight %d)", mdns, ssdp, yeelight);
        ld_mark_truncated(ctx);
    }
    if (mdns >= 0) {
        mdns_ask(mdns, "_services._dns-sd._udp.local");
        for (size_t i = 0; i < sizeof(MDNS_TYPES) / sizeof(MDNS_TYPES[0]); ++i) {
            mdns_ask(mdns, MDNS_TYPES[i]);
        }
    }
    ssdp_search(ssdp, yeelight, timeout_ms);
    // UDP can be lost: SSDP and Yeelight once more a third of the way in; reverse names after the
    // sweep (or halfway, without ARP), when most hosts are known.
    int64_t ssdp_again = start + timeout_ms / 3;
    int64_t reverse_at = (sources & SRC_ARP) ? start + sweep_ms + 150 : start + timeout_ms / 2;
    // Questions asked later than this wouldn't get answers in time.
    const int64_t last_query = deadline - (timeout_ms / 4 < 400 ? timeout_ms / 4 : 400);

    for (;;) {
        const int64_t now = now_ms();
        if (now >= deadline) {
            break;
        }
        if ((sources & SRC_ARP) && now >= next_arp) {
            arp_harvest(ctx, dump);
            if (swept < sweep_count) {
                const uint32_t n = sweep_count - swept < (uint32_t)batch ? sweep_count - swept : (uint32_t)batch;
                arp_send_t a = {.nif = nif, .first = sweep_first + swept, .count = (int)n, .skip = own};
                tcpip_api_call(arp_send_on_tcpip, &a.call);
                arp_sent += a.sent;
                arp_failed += a.failed;
                swept += n;
                next_arp = now + ARP_TICK_MS;
            } else {
                next_arp = now + 200;
            }
        }
        if (mdns >= 0 && now < last_query) {
            char type[LD_STR_MAX + 1];
            while (ld_next_new_type(ctx, type, sizeof(type))) {
                mdns_ask(mdns, type);
            }
            if (reverse_at && now >= reverse_at) {
                if (sources & SRC_ARP) {
                    arp_harvest(ctx, dump);
                }
                mdns_ask_reverse(mdns, ctx);
                reverse_at = 0;
            }
        }
        if (ssdp_again && now >= ssdp_again && now < last_query) {
            ssdp_search(ssdp, yeelight, timeout_ms);
            ssdp_again = 0;
        }

        fd_set fds;
        FD_ZERO(&fds);
        int maxfd = -1;
        const int socks[] = {mdns, ssdp, yeelight};
        for (int i = 0; i < 3; ++i) {
            if (socks[i] >= 0) {
                FD_SET(socks[i], &fds);
                maxfd = socks[i] > maxfd ? socks[i] : maxfd;
            }
        }
        const int64_t left = deadline - now_ms();
        const int wait = left < LOOP_TICK_MS ? (left > 0 ? (int)left : 0) : LOOP_TICK_MS;
        if (maxfd < 0) {
            vTaskDelay(pdMS_TO_TICKS(wait > 0 ? wait : 1));
            continue;
        }
        struct timeval tv = {.tv_sec = 0, .tv_usec = wait * 1000};
        if (select(maxfd + 1, &fds, NULL, NULL, &tv) <= 0) {
            continue;
        }
        if (mdns >= 0 && FD_ISSET(mdns, &fds)) {
            drain(mdns, MDNS_PORT, ctx, buf);
        }
        if (ssdp >= 0 && FD_ISSET(ssdp, &fds)) {
            drain(ssdp, SSDP_PORT, ctx, buf);
        }
        if (yeelight >= 0 && FD_ISSET(yeelight, &fds)) {
            drain(yeelight, YEELIGHT_PORT, ctx, buf);
        }
    }
    if (sources & SRC_ARP) {
        arp_harvest(ctx, dump);
    }
    if (mdns >= 0) {
        struct ip_mreq mr = {.imr_multiaddr.s_addr = inet_addr("224.0.0.251"), .imr_interface.s_addr = INADDR_ANY};
        setsockopt(mdns, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mr, sizeof(mr));
        close(mdns);
    }
    if (ssdp >= 0) {
        close(ssdp);
    }
    if (yeelight >= 0) {
        close(yeelight);
    }

    int hosts, n_arp, n_mdns, n_ssdp;
    bool truncated;
    ld_counts(ctx, &hosts, &n_arp, &n_mdns, &n_ssdp, &truncated);
    char *result = ld_result_json(ctx);
    ld_free(ctx);
    free(buf);
    free(dump);
    ESP_LOGI(TAG, "%s: %d hosts (arp %d of %u swept, %d failed; mdns %d answers; ssdp %d answers)%s, %d ms, %u B",
             id, hosts, n_arp, (unsigned)arp_sent, arp_failed, n_mdns, n_ssdp, truncated ? ", truncated" : "",
             (int)(now_ms() - start), result ? (unsigned)strlen(result) : 0);
    if (!result) {
        return error_frame(id, "the T-Circle is out of memory for the discovery result");
    }
    ld_sb_t b = {0};
    ld_sb_str(&b, "{\"id\":");
    ld_sb_json(&b, id);
    ld_sb_str(&b, ",\"type\":\"response\",\"result\":");
    ld_sb_str(&b, result);
    ld_sb_str(&b, "}");
    free(result);
    if (b.oom) {
        ld_sb_free(&b);
        return error_frame(id, "the T-Circle is out of memory for the discovery result");
    }
    return b.p;
}
