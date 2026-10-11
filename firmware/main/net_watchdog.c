#include "heap_diag.h"
#include "memory_caps.h"
#include "net_watchdog.h"

#include <stdatomic.h>
#include <string.h>
#include <sys/socket.h>

#include "voice_state.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "lwip/icmp.h"
#include "lwip/inet_chksum.h"
#include "lwip/ip.h"
#include "lwip/sockets.h"
#include "wifi.h"

#define PING_INTERVAL_US (10LL * 1000000)
#define PING_TIMEOUT_MS 2000
#define MISSES_TO_ACT 3
#define DIAL_FAILURES_TO_ACT 3
// After a rejoin, how long Wi-Fi may take to get an IP back before the device restarts.
#define REJOIN_LIMIT_US (60LL * 1000000)
// Dial failures alone may be the internet or the backend, which a rejoin can't fix: act on them at
// most this often. Router misses are always acted on.
#define DIAL_REJOIN_SPACING_US (10LL * 60 * 1000000)

static const char *TAG = "netdog";

static atomic_int s_dial_failures;
// The router has answered a ping at least once since boot. Until then a missing answer proves
// nothing (some routers drop ICMP), so misses neither trigger a rejoin nor a restart.
static bool s_router_pings;

typedef enum {
    IDLE = 0,
    REJOINING,  // dropped Wi-Fi ourselves; waiting for an IP and the router's answer
} stage_t;

typedef struct {
    uint8_t bssid[6];
    uint8_t channel;
    int8_t rssi;
    bool valid;
} link_t;

static void read_link(link_t *out)
{
    wifi_ap_record_t ap;
    memset(out, 0, sizeof(*out));
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        memcpy(out->bssid, ap.bssid, sizeof(out->bssid));
        out->channel = ap.primary;
        out->rssi = ap.rssi;
        out->valid = true;
    }
}

#define BSSID_FMT "%02x:%02x:%02x:%02x:%02x:%02x"
#define BSSID_ARGS(b) (b)[0], (b)[1], (b)[2], (b)[3], (b)[4], (b)[5]

// One ICMP echo to the router. Opens its own socket each time, so it holds no socket slot between
// probes.
static bool ping_gateway(uint32_t gw, uint16_t seq)
{
    int fd = socket(AF_INET, SOCK_RAW, IP_PROTO_ICMP);
    if (fd < 0) {
        ESP_LOGW(TAG, "ping socket failed");
        return true;  // can't tell; don't count it as a miss
    }
    struct timeval tv = {.tv_sec = PING_TIMEOUT_MS / 1000, .tv_usec = (PING_TIMEOUT_MS % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_addr.s_addr = gw};
    struct icmp_echo_hdr echo = {0};
    ICMPH_TYPE_SET(&echo, ICMP_ECHO);
    ICMPH_CODE_SET(&echo, 0);
    echo.id = htons(0x7c1e);
    echo.seqno = htons(seq);
    echo.chksum = inet_chksum(&echo, sizeof(echo));
    bool ok = false;
    if (sendto(fd, &echo, sizeof(echo), 0, (struct sockaddr *)&to, sizeof(to)) == sizeof(echo)) {
        const int64_t until = esp_timer_get_time() + PING_TIMEOUT_MS * 1000LL;
        uint8_t buf[64];
        while (!ok && esp_timer_get_time() < until) {
            struct sockaddr_in from;
            socklen_t from_len = sizeof(from);
            int n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &from_len);
            if (n < 0) {
                break;  // timed out
            }
            const struct ip_hdr *ip = (const struct ip_hdr *)buf;
            const int ihl = IPH_HL(ip) * 4;
            if (n < ihl + (int)sizeof(struct icmp_echo_hdr) || from.sin_addr.s_addr != gw) {
                continue;
            }
            const struct icmp_echo_hdr *reply = (const struct icmp_echo_hdr *)(buf + ihl);
            ok = ICMPH_TYPE(reply) == ICMP_ER && reply->id == echo.id && reply->seqno == echo.seqno;
        }
    }
    close(fd);
    return ok;
}

static void restart(const char *why)
{
    // A dead link ends a call within 10 s on its own (UPLINK_STALL_LIMIT_MS); don't cut a live one.
    while (comma_voice_active()) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGE(TAG, "restarting: %s", why);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

static void watchdog_task(void *arg)
{
    (void)arg;
    stage_t stage = IDLE;
    int misses = 0;
    bool router_answered = false;  // since the last IP: a router that never answers ping proves nothing
    uint32_t gw = 0;
    uint16_t seq = 0;
    int64_t next_ping = 0;
    int64_t rejoin_at = 0;
    int64_t ip_back_at = 0;
    int64_t last_dial_rejoin = -DIAL_REJOIN_SPACING_US;
    unsigned generation = 0;
    unsigned rejoin_generation = 0;
    link_t before = {0};
    const char *reason = "";
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        const int64_t now = esp_timer_get_time();
        const bool up = wifi_is_up();
        if (!up) {
            misses = 0;
            router_answered = false;
            if (stage == REJOINING && now - rejoin_at > REJOIN_LIMIT_US) {
                restart("no IP 60 s after rejoining Wi-Fi");
            }
            continue;
        }
        const unsigned gen = wifi_ip_generation();
        if (gen != generation) {
            generation = gen;
            gw = wifi_gateway();
            next_ping = now + 2000000;  // give ARP a moment
            if (stage == REJOINING && gen != rejoin_generation && ip_back_at == 0) {
                ip_back_at = now;
            }
        }
        if (stage == REJOINING && ip_back_at == 0) {
            // Still on the old link (the disconnect event hasn't landed yet).
            if (now - rejoin_at > REJOIN_LIMIT_US) {
                restart("Wi-Fi did not come back 60 s after rejoining");
            }
            continue;
        }

        if (stage == IDLE) {
            const int dials = s_dial_failures;
            const char *why = NULL;
            if (router_answered && misses >= MISSES_TO_ACT) {
                why = "router stopped answering ping";
            } else if (dials >= DIAL_FAILURES_TO_ACT && now - last_dial_rejoin >= DIAL_REJOIN_SPACING_US) {
                why = "Connector dials keep failing before TLS";
                last_dial_rejoin = now;
            }
            if (why) {
                read_link(&before);
                reason = why;
                ESP_LOGW(TAG, "rejoining Wi-Fi: %s (router misses %d, failed dials %d) bssid=" BSSID_FMT " ch=%u rssi=%d",
                         why, misses, dials, BSSID_ARGS(before.bssid), before.channel, before.rssi);
                s_dial_failures = 0;
                misses = 0;
                if (!wifi_rejoin()) {
                    ESP_LOGW(TAG, "rejoin not possible now (pairing mode or a scan)");
                    continue;
                }
                stage = REJOINING;
                rejoin_at = now;
                rejoin_generation = generation;
                ip_back_at = 0;
                router_answered = false;
                continue;
            }
        }

        if (gw == 0 || now < next_ping) {
            continue;
        }
        next_ping = now + PING_INTERVAL_US;
        bool ok = ping_gateway(gw, ++seq);
        comma_heap_log("netdog");
#ifdef NET_WATCHDOG_SIMULATE
        // Bench test only (#define NET_WATCHDOG_SIMULATE at the top of this file; -D via idf.py is
        // cached in build/toolchain/cflags and not rebuilt): 1 = three misses once after 60 s uptime,
        // 2 = misses forever after 60 s.
        static int simulated;
        if (now > 60LL * 1000000 && (NET_WATCHDOG_SIMULATE == 2 || simulated < 3)) {
            ok = false;
            simulated++;
        }
#endif
        if (ok) {
            misses = 0;
            router_answered = true;
            s_router_pings = true;
        } else {
            ++misses;
            ESP_LOGW(TAG, "router " IPSTR " missed ping (%d in a row)", IP2STR((esp_ip4_addr_t *)&gw), misses);
        }
        if (stage != REJOINING) {
            continue;
        }
        if (ok || !s_router_pings) {
            link_t after;
            read_link(&after);
            const bool moved = memcmp(before.bssid, after.bssid, sizeof(before.bssid)) != 0;
            ESP_LOGW(TAG, "recovered in %lld ms (IP after %lld ms): bssid=" BSSID_FMT " ch=%u rssi=%d, %s (was " BSSID_FMT " ch=%u); cause: %s",
                     (long long)((now - rejoin_at) / 1000), (long long)((ip_back_at ? ip_back_at - rejoin_at : 0) / 1000),
                     BSSID_ARGS(after.bssid), after.channel, after.rssi, moved ? "BSSID changed" : "same BSSID",
                     BSSID_ARGS(before.bssid), before.channel, reason);
            stage = IDLE;
        } else if (misses >= MISSES_TO_ACT && s_router_pings) {
            restart("router still not answering after rejoining Wi-Fi");
        }
    }
}

void net_watchdog_dial_result(bool opened, int http)
{
    if (opened || http != 0) {
        s_dial_failures = 0;
    } else {
        s_dial_failures++;
    }
}

void net_watchdog_start(void)
{
    static bool started;
    if (started) {
        return;
    }
    started = true;
    // No flash writes on this task, so its stack can live in PSRAM.
    if (xTaskCreateWithCaps(watchdog_task, "netdog", 4096, NULL, 3, NULL, COMMA_BUFFER_CAPS) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
    }
}
