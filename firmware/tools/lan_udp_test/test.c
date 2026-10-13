// SPDX-License-Identifier: Apache-2.0
// Host test for main/lan_udp.c: builds it on the Mac with BSD sockets. Every datagram the firmware
// sends goes to a local peer socket instead (stubs/lwip/sockets.h); the peer's answers are rewritten
// to look like they came from the LAN addresses the test chose.
// Run: ./run.sh

#include "../../main/lan_dest.c"
#include "../../main/lan_udp.c"

#undef sendto
#undef recvfrom

#include <assert.h>

static int g_checks, g_failed;

#define CHECK(cond, ...)                                         \
    do {                                                         \
        g_checks++;                                              \
        if (!(cond)) {                                           \
            g_failed++;                                          \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                        \
            fprintf(stderr, "\n");                               \
        }                                                        \
    } while (0)

// ---- Network stand-in --------------------------------------------------------------------------

#define SELF "192.168.1.17"

static const lan_request_t *g_request;
static const char *g_cancelled;
static bool g_cancel_after_resolve;
static int64_t g_cancel_at;
static const char *cancel_request(void *ctx) {
    (void)ctx;
    return g_cancel_at && esp_timer_get_time() >= g_cancel_at ? "socket closed" : g_cancelled;
}

bool wifi_ip_info(uint32_t *ip, uint32_t *netmask)
{
    *ip = inet_addr(SELF);
    *netmask = inet_addr("255.255.255.0");
    if (g_cancel_after_resolve) g_cancelled = "socket closed during DNS";
    return true;
}

// Peer 0 answers as the destination; peer 1 as some other host on the LAN.
static int g_peer[2];
static uint16_t g_peer_port[2];
static struct sockaddr_in g_fake[2];

// What the firmware sent.
static struct sockaddr_in g_sent_to;
static uint8_t g_sent[2048];
static ssize_t g_sent_len = -1;

// Answers to send when the firmware sends: from peer `who`, `len` bytes of `data`.
typedef struct {
    int who;
    const uint8_t *data;
    size_t len;
} answer_t;
static answer_t g_answers[8];
static int g_nanswers;

ssize_t test_sendto(int s, const void *buf, size_t len, int flags, const struct sockaddr *to, socklen_t tolen)
{
    memcpy(&g_sent_to, to, sizeof(g_sent_to));
    memcpy(g_sent, buf, len < sizeof(g_sent) ? len : sizeof(g_sent));
    g_sent_len = (ssize_t)len;
    struct sockaddr_in me;
    socklen_t ml = sizeof(me);
    getsockname(s, (struct sockaddr *)&me, &ml);
    struct sockaddr_in peer = {.sin_family = AF_INET, .sin_port = g_peer_port[0], .sin_addr.s_addr = inet_addr("127.0.0.1")};
    ssize_t n = sendto(s, buf, len, flags, (struct sockaddr *)&peer, sizeof(peer));
    // The firmware's socket is now bound; answer it.
    if (getsockname(s, (struct sockaddr *)&me, &ml) == 0) {
        me.sin_addr.s_addr = inet_addr("127.0.0.1");
        for (int i = 0; i < g_nanswers; ++i) {
            sendto(g_peer[g_answers[i].who], g_answers[i].data, g_answers[i].len, 0, (struct sockaddr *)&me, sizeof(me));
        }
    }
    return n;
}

ssize_t test_recvfrom(int s, void *buf, size_t len, int flags, struct sockaddr *from, socklen_t *fromlen)
{
    ssize_t n = recvfrom(s, buf, len, flags, from, fromlen);
    struct sockaddr_in *f = (struct sockaddr_in *)from;
    for (int k = 0; k < 2; ++k) {
        if (n >= 0 && f->sin_port == g_peer_port[k]) {
            *f = g_fake[k];
        }
    }
    return n;
}

static void drain_peers(void)
{
    uint8_t b[8192];
    for (int k = 0; k < 2; ++k) {
        while (recv(g_peer[k], b, sizeof(b), MSG_DONTWAIT) >= 0) {
        }
    }
}

static void setup(void)
{
    for (int k = 0; k < 2; ++k) {
        g_peer[k] = socket(AF_INET, SOCK_DGRAM, 0);
        int big = 1 << 20;
        setsockopt(g_peer[k], SOL_SOCKET, SO_SNDBUF, &big, sizeof(big));
        struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = inet_addr("127.0.0.1")};
        assert(bind(g_peer[k], (struct sockaddr *)&a, sizeof(a)) == 0);
        socklen_t l = sizeof(a);
        getsockname(g_peer[k], (struct sockaddr *)&a, &l);
        g_peer_port[k] = a.sin_port;
    }
    g_fake[0] = (struct sockaddr_in){.sin_family = AF_INET, .sin_port = htons(54321), .sin_addr.s_addr = inet_addr("192.168.1.19")};
    g_fake[1] = (struct sockaddr_in){.sin_family = AF_INET, .sin_port = htons(54321), .sin_addr.s_addr = inet_addr("192.168.1.99")};
}

// ---- Helpers -----------------------------------------------------------------------------------

static cJSON *g_reply;
static int g_ms;

// Run with params JSON; g_reply is the parsed frame.
static void run(const char *params_json)
{
    cJSON_Delete(g_reply);
    drain_peers();
    g_sent_len = -1;
    cJSON *p = cJSON_Parse(params_json);
    assert(p);
    const int64_t t0 = esp_timer_get_time();
    char *frame = lan_udp_run_request("req_\"q\"", p, g_request);
    g_ms = (int)((esp_timer_get_time() - t0) / 1000);
    cJSON_Delete(p);
    g_reply = frame ? cJSON_Parse(frame) : NULL;
    CHECK(g_reply != NULL, "reply is JSON: %s", frame ? frame : "(null)");
    CHECK(g_reply && strcmp(cJSON_GetObjectItem(g_reply, "id")->valuestring, "req_\"q\"") == 0, "id echoed");
    free(frame);
    g_nanswers = 0;
}

static const char *error_text(void)
{
    cJSON *e = g_reply ? cJSON_GetObjectItem(g_reply, "error") : NULL;
    return cJSON_IsString(e) ? e->valuestring : NULL;
}

static bool is_error(const char *needle)
{
    const char *e = error_text();
    return e && strstr(e, needle);
}

static cJSON *responses(void)
{
    cJSON *r = g_reply ? cJSON_GetObjectItem(g_reply, "result") : NULL;
    return r ? cJSON_GetObjectItem(r, "responses") : NULL;
}

static bool timed_out(void)
{
    cJSON *r = g_reply ? cJSON_GetObjectItem(g_reply, "result") : NULL;
    return r && cJSON_IsTrue(cJSON_GetObjectItem(r, "timed_out"));
}

static void answer(int who, const void *data, size_t len)
{
    g_answers[g_nanswers++] = (answer_t){.who = who, .data = data, .len = len};
}

// ---- Tests -------------------------------------------------------------------------------------

static void test_base64(void)
{
    uint8_t in[300], back[300];
    char text[420];
    srand(7);
    for (size_t n = 0; n < sizeof(in); ++n) {
        for (size_t i = 0; i < n; ++i) {
            in[i] = (uint8_t)rand();
        }
        b64_encode(in, n, text);
        CHECK(strlen(text) == (n + 2) / 3 * 4, "encoded length for %zu", n);
        int got = b64_decode(text, back, sizeof(back));
        CHECK(got == (int)n && memcmp(in, back, n) == 0, "round trip %zu", n);
    }
    b64_encode((const uint8_t *)"foobar", 6, text);
    CHECK(strcmp(text, "Zm9vYmFy") == 0, "RFC 4648 vector: %s", text);
    b64_encode((const uint8_t *)"fo", 2, text);
    CHECK(strcmp(text, "Zm8=") == 0, "padding: %s", text);
    const char *bad[] = {"Zm8", "Zm=8", "Z===", "Zm9v YmFy", "Zm9v\nYmFy", "Zm9-", "Zm9_", "=Zm8", "Zg==Zg=="};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        CHECK(b64_decode(bad[i], back, sizeof(back)) < 0, "rejects \"%s\"", bad[i]);
    }
    CHECK(b64_decode("Zm9vYmFy", back, 5) == -2, "longer than max");
    CHECK(b64_decode("Zm9vYmFy", back, 6) == 6, "exactly max");
    CHECK(b64_decode("", back, 1) == 0, "empty");
}

static void test_exchange(void)
{
    // miIO hello: 32 bytes; the light answers once.
    static const uint8_t hello[32] = {0x21, 0x31, 0x00, 0x20, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                      0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                      0xff, 0xff, 0xff, 0xff};
    static const uint8_t reply[] = {0x21, 0x31, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x15, 0x24, 0x3f, 0x00};
    char b64[64], params[256];
    b64_encode(hello, sizeof(hello), b64);
    answer(0, reply, sizeof(reply));
    snprintf(params, sizeof(params), "{\"host\":\"192.168.1.19\",\"port\":54321,\"payload_b64\":\"%s\",\"timeout_ms\":2000}", b64);
    run(params);
    CHECK(!error_text(), "no error: %s", error_text());
    CHECK(g_sent_len == 32 && memcmp(g_sent, hello, 32) == 0, "payload sent as decoded bytes");
    CHECK(g_sent_to.sin_addr.s_addr == inet_addr("192.168.1.19") && ntohs(g_sent_to.sin_port) == 54321, "destination");
    cJSON *rs = responses();
    CHECK(cJSON_GetArraySize(rs) == 1, "one answer");
    cJSON *r0 = cJSON_GetArrayItem(rs, 0);
    CHECK(r0 && strcmp(cJSON_GetObjectItem(r0, "from")->valuestring, "192.168.1.19:54321") == 0, "from ip:port");
    char want[64];
    b64_encode(reply, sizeof(reply), want);
    CHECK(r0 && strcmp(cJSON_GetObjectItem(r0, "payload_b64")->valuestring, want) == 0, "answer bytes");
    CHECK(!timed_out(), "max_responses (default 1) reached, not timed out");
    CHECK(g_ms < 500, "returns as soon as the answer is in (%d ms)", g_ms);

    // Two answers wanted three: waits out the timeout. Another host's datagram is ignored.
    answer(0, "a", 1);
    answer(1, "spoof", 5);
    answer(0, "b", 1);
    run("{\"host\":\"192.168.1.19\",\"port\":54321,\"payload_b64\":\"eA==\",\"timeout_ms\":300,\"max_responses\":3}");
    rs = responses();
    CHECK(cJSON_GetArraySize(rs) == 2, "two answers from the destination, got %d", cJSON_GetArraySize(rs));
    CHECK(rs && strcmp(cJSON_GetObjectItem(cJSON_GetArrayItem(rs, 1), "payload_b64")->valuestring, "Yg==") == 0,
          "in arrival order");
    CHECK(timed_out(), "timed out short of max_responses");
    CHECK(g_ms >= 290 && g_ms < 800, "waited ~300 ms (%d)", g_ms);

    // No answer.
    run("{\"host\":\"192.168.1.19\",\"port\":54321,\"payload_b64\":\"eA==\",\"timeout_ms\":5}");
    CHECK(cJSON_GetArraySize(responses()) == 0 && timed_out(), "no answer: empty and timed out");
    CHECK(g_ms >= 95 && g_ms < 500, "timeout_ms 5 raised to 100 (%d ms)", g_ms);

    // max_responses 0: fire and forget.
    run("{\"host\":\"192.168.1.19\",\"port\":54321,\"payload_b64\":\"eA==\",\"max_responses\":0}");
    CHECK(cJSON_GetArraySize(responses()) == 0 && !timed_out() && g_ms < 100, "max_responses 0 returns at once");
    CHECK(g_sent_len == 1, "but still sends");

    // Empty payload, a hostname, the largest payload.
    run("{\"host\":\"192.168.1.19\",\"port\":9,\"payload_b64\":\"\",\"max_responses\":0}");
    CHECK(!error_text() && g_sent_len == 0, "empty datagram");
    static uint8_t big[LAN_UDP_MAX_PAYLOAD + 1];
    memset(big, 0xA5, sizeof(big));
    static char bigb64[(sizeof(big) + 2) / 3 * 4 + 16];
    static char bigparams[sizeof(bigb64) + 128];
    b64_encode(big, LAN_UDP_MAX_PAYLOAD, bigb64);
    snprintf(bigparams, sizeof(bigparams), "{\"host\":\"192.168.1.19\",\"port\":9,\"payload_b64\":\"%s\",\"max_responses\":0}",
             bigb64);
    run(bigparams);
    CHECK(!error_text() && g_sent_len == LAN_UDP_MAX_PAYLOAD, "1472 bytes sent: %s", error_text());
    b64_encode(big, LAN_UDP_MAX_PAYLOAD + 1, bigb64);
    snprintf(bigparams, sizeof(bigparams), "{\"host\":\"192.168.1.19\",\"port\":9,\"payload_b64\":\"%s\"}", bigb64);
    run(bigparams);
    CHECK(is_error("longer than 1472 bytes") && g_sent_len < 0, "1473 bytes refused: %s", error_text());
    b64_encode(big, LAN_UDP_MAX_PAYLOAD - 2, bigb64);  // 1470 bytes, no padding
    strcat(bigb64, "QUJDREVG");                      // + 6 = 1476 bytes
    snprintf(bigparams, sizeof(bigparams), "{\"host\":\"192.168.1.19\",\"port\":9,\"payload_b64\":\"%s\"}", bigb64);
    run(bigparams);
    CHECK(is_error("longer than 1472 bytes"), "1476 bytes refused: %s", error_text());

    // A large answer, and one past the cap (cut by the socket).
    static uint8_t ans[5000];
    for (size_t i = 0; i < sizeof(ans); ++i) {
        ans[i] = (uint8_t)i;
    }
    answer(0, ans, LAN_UDP_MAX_ANSWER);
    answer(0, ans, sizeof(ans));
    run("{\"host\":\"192.168.1.19\",\"port\":54321,\"payload_b64\":\"eA==\",\"max_responses\":2,\"timeout_ms\":1000}");
    rs = responses();
    CHECK(cJSON_GetArraySize(rs) == 2, "two big answers");
    for (int i = 0; i < 2 && rs; ++i) {
        uint8_t back[LAN_UDP_MAX_ANSWER];
        int n = b64_decode(cJSON_GetObjectItem(cJSON_GetArrayItem(rs, i), "payload_b64")->valuestring, back, sizeof(back));
        CHECK(n == LAN_UDP_MAX_ANSWER && memcmp(back, ans, (size_t)n) == 0, "answer %d: %d bytes", i, n);
    }
}

static void test_params(void)
{
    struct {
        const char *params;
        const char *error;
    } cases[] = {
        {"[]", "params must be an object"},
        {"{\"port\":1,\"payload_b64\":\"\"}", "'host' is required"},
        {"{\"host\":\"\",\"port\":1,\"payload_b64\":\"\"}", "'host' is required"},
        {"{\"host\":5,\"port\":1,\"payload_b64\":\"\"}", "'host' is required"},
        {"{\"host\":\"192.168.1.19\",\"payload_b64\":\"\"}", "'port' must be"},
        {"{\"host\":\"192.168.1.19\",\"port\":0,\"payload_b64\":\"\"}", "'port' must be"},
        {"{\"host\":\"192.168.1.19\",\"port\":65536,\"payload_b64\":\"\"}", "'port' must be"},
        {"{\"host\":\"192.168.1.19\",\"port\":80.5,\"payload_b64\":\"\"}", "'port' must be"},
        {"{\"host\":\"192.168.1.19\",\"port\":\"80\",\"payload_b64\":\"\"}", "'port' must be"},
        {"{\"host\":\"192.168.1.19\",\"port\":80}", "'payload_b64' is required"},
        {"{\"host\":\"192.168.1.19\",\"port\":80,\"payload_b64\":\"abc\"}", "not valid standard base64"},
        {"{\"host\":\"192.168.1.19\",\"port\":80,\"payload_b64\":\"ab c\"}", "not valid standard base64"},
        {"{\"host\":\"192.168.1.19\",\"port\":80,\"payload_b64\":\"\",\"timeout_ms\":\"1000\"}", "'timeout_ms' must be"},
        {"{\"host\":\"192.168.1.19\",\"port\":80,\"payload_b64\":\"\",\"timeout_ms\":1.5}", "'timeout_ms' must be"},
        {"{\"host\":\"192.168.1.19\",\"port\":80,\"payload_b64\":\"\",\"max_responses\":true}", "'max_responses' must be"},
        // Destinations: the same rule as http_request.
        {"{\"host\":\"8.8.8.8\",\"port\":53,\"payload_b64\":\"\"}", "public address"},
        {"{\"host\":\"192.168.1.255\",\"port\":54321,\"payload_b64\":\"\"}", "broadcast address of the local subnet"},
        {"{\"host\":\"255.255.255.255\",\"port\":54321,\"payload_b64\":\"\"}", "multicast, broadcast or reserved"},
        {"{\"host\":\"239.255.255.250\",\"port\":1982,\"payload_b64\":\"\"}", "multicast, broadcast or reserved"},
        {"{\"host\":\"" SELF "\",\"port\":54321,\"payload_b64\":\"\"}", "the T-Circle itself"},
        {"{\"host\":\"localhost\",\"port\":54321,\"payload_b64\":\"\"}", "loopback"},
        {"{\"host\":\"no-such-host.invalid\",\"port\":54321,\"payload_b64\":\"\"}", "could not resolve"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        run(cases[i].params);
        CHECK(is_error(cases[i].error), "%s -> want \"%s\", got \"%s\"", cases[i].params, cases[i].error,
              error_text() ? error_text() : "(no error)");
        CHECK(g_sent_len < 0, "%s: nothing sent", cases[i].params);
    }
    // Clamping.
    int v;
    cJSON *p = cJSON_Parse("{\"t\":99999,\"m\":-3}");
    CHECK(int_param(p, "t", 1, LAN_UDP_TIMEOUT_MIN_MS, LAN_UDP_TIMEOUT_MAX_MS, &v) && v == 10000, "timeout capped");
    CHECK(int_param(p, "m", 1, 0, LAN_UDP_MAX_RESPONSES, &v) && v == 0, "max_responses floored");
    CHECK(int_param(p, "missing", 7, 0, 10, &v) && v == 7, "default");
    cJSON_Delete(p);
    p = cJSON_Parse("{\"m\":99}");
    CHECK(int_param(p, "m", 1, 0, LAN_UDP_MAX_RESPONSES, &v) && v == 16, "max_responses capped");
    cJSON_Delete(p);
}

static void test_request_lifetime(void)
{
    const char *params = "{\"host\":\"192.168.1.19\",\"port\":54321,\"payload_b64\":\"AA==\",\"timeout_ms\":1000}";
    lan_request_t request = {.deadline_us = esp_timer_get_time() - 1, .cancelled = cancel_request};
    g_request = &request;
    run(params);
    CHECK(is_error("including queue time") && g_sent_len < 0, "expired request sent a datagram");
    request.deadline_us = esp_timer_get_time() + 30000000;
    g_cancelled = "socket closed"; run(params);
    CHECK(is_error("socket closed") && g_sent_len < 0, "disconnected request sent a datagram");
    g_cancelled = NULL; g_cancel_after_resolve = true; run(params);
    CHECK(is_error("during DNS") && g_sent_len < 0, "cancellation during DNS sent a datagram");
    g_cancel_after_resolve = false; g_cancelled = NULL;
    g_cancel_at = esp_timer_get_time() + 50000; run(params);
    CHECK(is_error("socket closed") && g_ms < 300, "active receive did not cancel promptly");
    g_cancel_at = 0;
    request.deadline_us = esp_timer_get_time() + 50000; run(params);
    CHECK(is_error("including queue time") && g_ms < 300, "remaining queue deadline ignored");
    g_request = NULL;
}

int main(void)
{
    setup();
    test_base64();
    test_exchange();
    test_params();
    test_request_lifetime();
    cJSON_Delete(g_reply);
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
