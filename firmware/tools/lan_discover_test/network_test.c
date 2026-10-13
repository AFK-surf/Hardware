// SPDX-License-Identifier: Apache-2.0
// Execute the real runner with a virtual clock/ARP cache/socket layer. No boards or LAN traffic.
// Built with the DevKit's wait for BLE off, so the wait runs against the same virtual clock.
#define CONFIG_COMMA_FREE_BLE_FOR_DISCOVERY 1
#include "../../main/lan_discover.c"
#include <assert.h>

static struct netif netif;
static int64_t clock_ms;
static int checks, opened, closed, probe_socket, socket_fail_at, hosts;
static int sends[256][8], first_at[256][8], bucket_count[400];
static bool sockets[20], answer_pending, echo_answers, flood, send_failure;
static int answer_len, recv_calls, max_sends, latest_send, fail_next, failed_sends, expected_wait;
static struct sockaddr_in pending_from;

#define CHECK(c) do { ++checks; if (!(c)) { fprintf(stderr, "runner line %d: %s\n", __LINE__, #c); abort(); } } while (0)

static int64_t ble_off_at;  // provisioning_ble_on() until this virtual time
static bool log_allocs;
void *ld_test_malloc(size_t n) { return malloc(n); }
bool provisioning_ble_on(void) { return clock_ms < ble_off_at; }
void comma_log_failed_allocs(bool on) { log_allocs = on; }
void *ld_test_realloc(void *p, size_t n) { return realloc(p, n); }

esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key) { return &netif; }
void *esp_netif_get_netif_impl(esp_netif_t *n) { return n; }
bool wifi_ip_info(uint32_t *ip, uint32_t *mask)
{
    *ip = inet_addr("192.168.1.17");
    *mask = inet_addr("255.255.255.0");
    return true;
}
err_t tcpip_api_call(err_t (*fn)(struct tcpip_api_call_data *), struct tcpip_api_call_data *data) { return fn(data); }
err_t etharp_request(struct netif *n, const ip4_addr_t *ip) { return ERR_OK; }
int etharp_get_entry(size_t i, ip4_addr_t **ip, struct netif **n, struct eth_addr **mac)
{
    static ip4_addr_t address;
    static struct eth_addr ether = {{1, 2, 3, 4, 5, 6}};
    if (i >= (size_t)hosts) return 0;
    unsigned h = (unsigned)i + (hosts > 32 && clock_ms >= 600 ? 32 : 0);
    char text[16]; snprintf(text, sizeof(text), "192.168.1.%u", 30 + h);
    address.addr = inet_addr(text);
    *ip = &address; *n = &netif; *mac = &ether;
    return 1;
}
int64_t esp_timer_get_time(void) { return clock_ms * 1000; }
void vTaskDelay(unsigned ticks) { clock_ms += ticks; }
int test_socket(int domain, int type, int protocol)
{
    if (opened == socket_fail_at) { errno = EMFILE; return -1; }
    int s = 10 + opened++;
    CHECK(s < 20 && !sockets[s]);
    sockets[s] = true;
    return s;
}
int test_close(int s) { CHECK(sockets[s]); sockets[s] = false; closed++; return 0; }
int test_bind(int s, const struct sockaddr *addr, socklen_t len) { return 0; }
int test_setsockopt(int s, int level, int opt, const void *value, socklen_t len) { return 0; }
int test_select(int n, fd_set *read, fd_set *write, fd_set *error, struct timeval *wait)
{
    // Advancing by the full wait also tests queued answers being drained before retry selection.
    clock_ms += wait->tv_sec * 1000 + wait->tv_usec / 1000;
    FD_ZERO(read);
    if (probe_socket >= 0 && (answer_pending || flood)) { FD_SET(probe_socket, read); return 1; }
    return 0;
}
ssize_t test_sendto(int s, const void *data, size_t len, int flags, const struct sockaddr *to, socklen_t tolen)
{
    const struct sockaddr_in *dest = (const struct sockaddr_in *)to;
    unsigned port = ntohs(dest->sin_port);
    if (port < 7000 || port >= 7000 + CONFIG_COMMA_LAN_DISCOVER_PROBES) return (ssize_t)len;
    CHECK(flags & MSG_DONTWAIT);
    CHECK(probe_socket < 0 || probe_socket == s);
    probe_socket = s;
    unsigned h = ntohl(dest->sin_addr.s_addr) & 255, p = port - 7000;
    CHECK(h >= 30 && h < 30 + (unsigned)hosts);
    CHECK(++bucket_count[clock_ms / 25] <= 4);
    if (bucket_count[clock_ms / 25] > max_sends) max_sends = bucket_count[clock_ms / 25];
    latest_send = (int)clock_ms;
    // Failed attempts are retried; only successful datagrams count against the two-send limit.
    if (send_failure || fail_next > 0) { fail_next--; failed_sends++; errno = ENOMEM; return -1; }
    CHECK(++sends[h][p] <= 2);
    if (sends[h][p] == 1) first_at[h][p] = (int)clock_ms;
    else CHECK(clock_ms - first_at[h][p] >= 500);
    if (echo_answers) { pending_from = *dest; answer_pending = true; }
    return (ssize_t)len;
}
ssize_t test_recvfrom(int s, void *data, size_t len, int flags, struct sockaddr *from, socklen_t *fromlen)
{
    if (s != probe_socket || (!answer_pending && !flood)) { errno = EWOULDBLOCK; return -1; }
    recv_calls++;
    CHECK(len == CONFIG_COMMA_LAN_DISCOVER_PROBE_ANSWER + 1);
    *(struct sockaddr_in *)from = pending_from;
    *fromlen = sizeof(pending_from);
    if (flood) ((struct sockaddr_in *)from)->sin_port = htons(9999);
    answer_pending = false;
    size_t n = (size_t)answer_len < len ? (size_t)answer_len : len;
    memset(data, 0xFF, n);
    return (ssize_t)n;
}

static void reset(void)
{
    CHECK(opened == closed);
    clock_ms = opened = closed = recv_calls = max_sends = latest_send = fail_next = failed_sends = expected_wait = 0;
    ble_off_at = 0;
    probe_socket = socket_fail_at = -1;
    hosts = 1;
    echo_answers = flood = send_failure = answer_pending = false;
    answer_len = 4;
    memset(sends, 0, sizeof(sends)); memset(bucket_count, 0, sizeof(bucket_count));
}

static cJSON *run(int ms, unsigned probes, bool arp_only)
{
    cJSON *params = cJSON_CreateObject();
    cJSON_AddNumberToObject(params, "timeout_ms", ms);
    if (arp_only) { cJSON *sources = cJSON_AddArrayToObject(params, "sources"); cJSON_AddItemToArray(sources, cJSON_CreateString("arp")); }
    if (probes) {
        cJSON *list = cJSON_AddArrayToObject(params, "udp_probes");
        for (unsigned i = 0; i < probes; ++i) {
            cJSON *p = cJSON_CreateObject(); char id[17]; snprintf(id, sizeof(id), "p%u", i);
            cJSON_AddStringToObject(p, "id", id); cJSON_AddNumberToObject(p, "port", 7000 + i);
            cJSON_AddStringToObject(p, "payload_b64", "AA=="); cJSON_AddItemToArray(list, p);
        }
    }
    char *raw = lan_discover_run("runner", params);
    CHECK(raw != NULL);
    cJSON *frame = cJSON_Parse(raw);
    CHECK(frame && cJSON_GetObjectItem(frame, "result"));
    CHECK(opened == closed && clock_ms >= ms + expected_wait && clock_ms <= ms + expected_wait + 25);
    CHECK(!log_allocs);  // failed-allocation logging is off again after every run
    free(raw); cJSON_Delete(params);
    return frame;
}

static int64_t cancel_at;
static const char *cancel_request(void *ctx) {
    (void)ctx; return clock_ms >= cancel_at ? "socket closed" : NULL;
}
static void check_cancelled(const lan_request_t *request) {
    cJSON *params = cJSON_Parse("{\"timeout_ms\":1000}");
    char *raw = lan_discover_run_request("cancel", params, request);
    CHECK(raw != NULL);
    cJSON *frame = cJSON_Parse(raw);
    CHECK(frame && cJSON_IsString(cJSON_GetObjectItem(frame, "error")));
    CHECK(opened == closed && !log_allocs);
    cJSON_Delete(frame); cJSON_Delete(params); free(raw);
}
static void test_request_lifetime(void) {
    reset();
    lan_request_t request = {.deadline_us = -1};
    check_cancelled(&request); CHECK(opened == 0);
    reset(); request.deadline_us = 30000000; request.cancelled = cancel_request; cancel_at = 0;
    check_cancelled(&request); CHECK(opened == 0);
    reset(); ble_off_at = 3000; cancel_at = 250;
    check_cancelled(&request); CHECK(opened == 0 && clock_ms <= 300);
    reset(); cancel_at = 250;
    check_cancelled(&request); CHECK(opened > 0 && clock_ms <= 300);
    reset(); ble_off_at = 3000; request.cancelled = NULL; request.deadline_us = 250000;
    check_cancelled(&request); CHECK(opened == 0 && clock_ms <= 300);
}

int main(void)
{
    reset();
    cJSON *frame = run(1000, 0, false);
    CHECK(opened == 3 && probe_socket == -1);
    cJSON_Delete(frame);
#if CONFIG_COMMA_LAN_DISCOVER_PROBES > 0
    reset(); hosts = 64;
    frame = run(8000, CONFIG_COMMA_LAN_DISCOVER_PROBES, false);
    CHECK(opened == 4 && max_sends == 4 && latest_send < 7600);
    for (unsigned h = 30; h < 94; ++h) for (unsigned p = 0; p < CONFIG_COMMA_LAN_DISCOVER_PROBES; ++p) CHECK(sends[h][p] == 2);
    CHECK(!cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetObjectItem(frame, "result"), "truncated")));
    cJSON_Delete(frame);

    reset(); hosts = 64;
    frame = run(1000, CONFIG_COMMA_LAN_DISCOVER_PROBES, true);
    CHECK(opened == 1 && latest_send < 750);
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetObjectItem(frame, "result"), "probes_truncated")));
    CHECK(!cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetObjectItem(frame, "result"), "truncated")));  // no host dropped
    cJSON_Delete(frame);

    reset(); socket_fail_at = 3;
    frame = run(1000, 1, false);
    CHECK(opened == 3 && probe_socket == -1);
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetObjectItem(frame, "result"), "probes_truncated")));
    CHECK(!cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetObjectItem(frame, "result"), "truncated")));
    cJSON_Delete(frame);

    reset(); echo_answers = true; answer_len = 4096;
    frame = run(1000, 1, true);
    cJSON *host = cJSON_GetArrayItem(cJSON_GetObjectItem(cJSON_GetObjectItem(frame, "result"), "hosts"), 0);
    cJSON *answer = cJSON_GetArrayItem(cJSON_GetObjectItem(host, "probes"), 0);
    CHECK(answer && cJSON_IsTrue(cJSON_GetObjectItem(answer, "cut")) && sends[30][0] == 1);
    CHECK(strlen(cJSON_GetObjectItem(answer, "payload_b64")->valuestring) == 4 * ((CONFIG_COMMA_LAN_DISCOVER_PROBE_ANSWER + 2)/3));
    cJSON_Delete(frame);

    reset(); echo_answers = true; answer_len = 0;
    frame = run(1000, 1, true);
    host = cJSON_GetArrayItem(cJSON_GetObjectItem(cJSON_GetObjectItem(frame, "result"), "hosts"), 0);
    answer = cJSON_GetArrayItem(cJSON_GetObjectItem(host, "probes"), 0);
    CHECK(answer && !strcmp(cJSON_GetObjectItem(answer, "payload_b64")->valuestring, "") && sends[30][0] == 1);
    cJSON_Delete(frame);

    reset(); flood = true;
    frame = run(1000, 1, true);
    CHECK(sends[30][0] == 2 && recv_calls <= 16 * 40);
    cJSON_Delete(frame);

    // Every send fails: retried every 50 ms until the listen-only tail, then unsent.
    reset(); send_failure = true;
    frame = run(1000, 1, true);
    CHECK(sends[30][0] == 0 && failed_sends == 15 && latest_send < 750);
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetObjectItem(frame, "result"), "probes_truncated")));
    CHECK(!cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetObjectItem(frame, "result"), "truncated")));
    cJSON_Delete(frame);

    // One failed send, retried on a later tick: nothing lost, no flag.
    reset(); fail_next = 1;
    frame = run(1000, 1, true);
    CHECK(sends[30][0] == 2 && failed_sends == 1 && first_at[30][0] >= 50);
    CHECK(!cJSON_GetObjectItem(cJSON_GetObjectItem(frame, "result"), "probes_truncated"));
    CHECK(!cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetObjectItem(frame, "result"), "truncated")));
    cJSON_Delete(frame);
#endif

    // BLE still on: discovery waits for it (100 ms steps), then runs normally.
    reset(); ble_off_at = 2950; expected_wait = 3000;
    frame = run(1000, 0, true);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(cJSON_GetObjectItem(frame, "result"), "hosts")) == 1);
    cJSON_Delete(frame);

    // BLE never ends: give up at the deadline-bounded limit (15 s - 2 s - 5 s) and run anyway.
    reset(); ble_off_at = 1000000; expected_wait = 8000;
    frame = run(5000, 0, true);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(cJSON_GetObjectItem(frame, "result"), "hosts")) == 1);
    cJSON_Delete(frame);
    test_request_lifetime();
    printf("runner: %d checks, 0 failed\n", checks);
    return 0;
}
