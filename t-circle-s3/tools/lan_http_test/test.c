// Host test for main/lan_http.c: builds it on the Mac against stub ESP headers (stubs/), with
// POSIX sockets standing in for esp_transport. Every connection goes to the local server.py,
// whatever IP the firmware picked; the IP it asked for is recorded and checked. Chunk mode
// (response_mode "chunks") goes to a sink that reassembles and checks every frame.
// Run: ./run.sh

#include "../../main/lan_dest.c"
#include "../../main/lan_http.c"

#include <CommonCrypto/CommonDigest.h>
#include <errno.h>
#include <stdint.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <unistd.h>

// ---- Stubs -------------------------------------------------------------------------------------

static int g_port;
static char g_connect_host[64];
static bool g_wifi_up = true;
static uint32_t g_self;
static uint32_t g_mask;

bool wifi_ip_info(uint32_t *ip, uint32_t *netmask)
{
    if (!g_wifi_up) {
        return false;
    }
    *ip = g_self;
    *netmask = g_mask;
    return true;
}

struct transport {
    int fd;
    int err;
};

esp_transport_handle_t esp_transport_tcp_init(void)
{
    esp_transport_handle_t t = calloc(1, sizeof(*t));
    t->fd = -1;
    return t;
}

esp_transport_handle_t esp_transport_ssl_init(void)
{
    return esp_transport_tcp_init();
}

void esp_transport_ssl_crt_bundle_attach(esp_transport_handle_t t, int (*attach)(void *))
{
    (void)t;
    (void)attach;
}

void esp_transport_ssl_set_common_name(esp_transport_handle_t t, const char *name)
{
    (void)t;
    (void)name;
}

int esp_transport_connect(esp_transport_handle_t t, const char *host, int port, int timeout_ms)
{
    (void)timeout_ms;
    strlcpy(g_connect_host, host, sizeof(g_connect_host));
    t->fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(t->fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
        t->err = errno;
        return -1;
    }
    return 0;
}

int esp_transport_read(esp_transport_handle_t t, char *buf, int len, int timeout_ms)
{
    struct pollfd p = {.fd = t->fd, .events = POLLIN};
    int r = poll(&p, 1, timeout_ms);
    if (r == 0) {
        return ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT;
    }
    ssize_t n = recv(t->fd, buf, (size_t)len, 0);
    if (n == 0) {
        return ERR_TCP_TRANSPORT_CONNECTION_CLOSED_BY_FIN;
    }
    return n < 0 ? ERR_TCP_TRANSPORT_CONNECTION_FAILED : (int)n;
}

int esp_transport_write(esp_transport_handle_t t, const char *buf, int len, int timeout_ms)
{
    (void)timeout_ms;
    ssize_t n = send(t->fd, buf, (size_t)len, 0);
    return n < 0 ? -1 : (int)n;
}

int esp_transport_close(esp_transport_handle_t t)
{
    if (t->fd >= 0) {
        close(t->fd);
    }
    t->fd = -1;
    return 0;
}

int esp_transport_destroy(esp_transport_handle_t t)
{
    free(t);
    return 0;
}

int esp_transport_get_errno(esp_transport_handle_t t)
{
    return t->err;
}

// ---- Harness -----------------------------------------------------------------------------------

static int g_failed;
static int g_passed;

#define CHECK(cond, ...)                                       \
    do {                                                       \
        if (cond) {                                            \
            g_passed++;                                        \
        } else {                                               \
            g_failed++;                                        \
            fprintf(stdout, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stdout, __VA_ARGS__);                      \
            fprintf(stdout, "\n");                             \
        }                                                      \
    } while (0)

static uint32_t ip(const char *s)
{
    uint32_t v = 0;
    if (!parse_ipv4(s, &v)) {
        fprintf(stderr, "bad test ip %s\n", s);
        exit(2);
    }
    return v;
}

static void set_net(const char *self, const char *mask)
{
    g_self = ip(self);
    g_mask = ip(mask);
}

static char g_frame[300000];

// ---- Chunk sink ----------------------------------------------------------------------------------

typedef struct {
    const char *busy;        // what refuse() answers (NULL = go ahead)
    unsigned refuse_calls;
    const char *fail_msg;    // send()'s error at fail_at (default: socket closed)
    size_t frame_bytes;      // what frame_bytes() answers; 0 = alternate 1000 and 5000
    int fail_at;             // send() fails on this frame (1-based); 0 = never
    unsigned calls;          // frame_bytes() calls
    unsigned frames;         // frames accepted
    bool bad;                // a frame broke the contract (message in why)
    char why[300];
    size_t asked;            // what the last frame_bytes() call answered
    uint8_t *data;           // reassembled body
    size_t len;
    size_t cap;
} sink_state_t;

static sink_state_t g_sink;

static const char *sink_refuse(void *ctx)
{
    sink_state_t *st = ctx;
    st->refuse_calls++;
    return st->busy;
}

static size_t sink_frame_bytes(void *ctx)
{
    sink_state_t *st = ctx;
    st->calls++;
    st->asked = st->frame_bytes ? st->frame_bytes : (st->calls % 2 ? 1000 : 5000);
    return st->asked;
}

static int b64_val(char c)
{
    const char *p = c ? strchr(B64, c) : NULL;
    return p ? (int)(p - B64) : -1;
}

// Strict standard base64 with padding; -1 when invalid.
static long b64_decode(const char *in, uint8_t *out)
{
    const size_t n = strlen(in);
    if (n % 4) {
        return -1;
    }
    long o = 0;
    for (size_t i = 0; i < n; i += 4) {
        int v[4];
        for (int k = 0; k < 4; ++k) {
            v[k] = in[i + k] == '=' && i + 4 == n && k >= 2 ? 0 : b64_val(in[i + k]);
            if (v[k] < 0) {
                return -1;
            }
        }
        const uint32_t w = (uint32_t)v[0] << 18 | (uint32_t)v[1] << 12 | (uint32_t)v[2] << 6 | (uint32_t)v[3];
        out[o++] = (uint8_t)(w >> 16);
        if (in[i + 2] != '=') {
            out[o++] = (uint8_t)(w >> 8);
        }
        if (in[i + 3] != '=') {
            out[o++] = (uint8_t)w;
        }
    }
    return o;
}

static bool sink_send(void *ctx, const char *frame, size_t len, size_t raw_bytes, char *err, size_t err_size)
{
    sink_state_t *st = ctx;
    if (st->fail_at && (int)st->frames + 1 == st->fail_at) {
        snprintf(err, err_size, "%s", st->fail_msg ? st->fail_msg : "the Connector socket closed during the transfer");
        return false;
    }
    if (strlen(frame) != len) {
        snprintf(st->why, sizeof(st->why), "frame %u: len %zu, strlen %zu", st->frames, len, strlen(frame));
        st->bad = true;
    }
    cJSON *f = cJSON_Parse(frame);
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(f, "id");
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(f, "type");
    const cJSON *seq = cJSON_GetObjectItemCaseSensitive(f, "seq");
    const cJSON *data = cJSON_GetObjectItemCaseSensitive(f, "data_b64");
    if (!f || cJSON_GetArraySize(f) != 4 || !cJSON_IsString(id) || strcmp(id->valuestring, "req_test") != 0 ||
        !cJSON_IsString(type) || strcmp(type->valuestring, "response_chunk") != 0 || !cJSON_IsNumber(seq) ||
        seq->valuedouble != st->frames || !cJSON_IsString(data)) {
        snprintf(st->why, sizeof(st->why), "frame %u malformed: %.200s", st->frames, frame);
        st->bad = true;
        cJSON_Delete(f);
        return true;
    }
    if (st->len + raw_bytes + 4 > st->cap) {
        st->cap = (st->len + raw_bytes + 4) * 2;
        st->data = realloc(st->data, st->cap);
    }
    const long n = b64_decode(data->valuestring, st->data + st->len);
    if (n < 0 || (size_t)n != raw_bytes || raw_bytes == 0 || raw_bytes > st->asked || raw_bytes > LAN_HTTP_CHUNK_BYTES) {
        snprintf(st->why, sizeof(st->why), "frame %u: decoded %ld, raw_bytes %zu, asked %zu", st->frames, n, raw_bytes,
                 st->asked);
        st->bad = true;
    } else {
        st->len += (size_t)n;
    }
    st->frames++;
    cJSON_Delete(f);
    return true;
}

static const lan_http_sink_t SINK = {
    .refuse = sink_refuse, .frame_bytes = sink_frame_bytes, .send = sink_send, .ctx = &g_sink};
static const lan_http_sink_t *g_use_sink = &SINK;

static void sink_reset(size_t frame_bytes, int fail_at)
{
    free(g_sink.data);
    memset(&g_sink, 0, sizeof(g_sink));
    g_sink.frame_bytes = frame_bytes;
    g_sink.fail_at = fail_at;
}

static uint8_t pattern_byte(size_t i)
{
    return (uint8_t)(i ^ (i >> 8) ^ (i >> 16));
}

static void sha256_hex(const uint8_t *p, size_t n, char *hex)
{
    uint8_t d[32];
    CC_SHA256(p, (CC_LONG)n, d);
    for (int i = 0; i < 32; ++i) {
        sprintf(hex + 2 * i, "%02x", d[i]);
    }
}

// Runs one request. params is printf-style JSON; %P is not used, URLs carry the port explicitly.
static cJSON *run(const char *fmt, ...)
{
    char json[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(json, sizeof(json), fmt, ap);
    va_end(ap);
    cJSON *params = cJSON_Parse(json);
    if (!params) {
        fprintf(stderr, "bad test params %s\n", json);
        exit(2);
    }
    g_connect_host[0] = 0;
    char *frame = lan_http_run("req_test", params, g_use_sink);
    cJSON_Delete(params);
    strlcpy(g_frame, frame ? frame : "", sizeof(g_frame));
    cJSON *out = frame ? cJSON_Parse(frame) : NULL;
    free(frame);
    if (!out) {
        fprintf(stderr, "unparseable frame: %.300s\n", g_frame);
        exit(2);
    }
    return out;
}

static const char *str(const cJSON *obj, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(v) ? v->valuestring : "";  // "" rather than NULL keeps a failed check from crashing
}

static double num(const cJSON *obj, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(v) ? v->valuedouble : -1;
}

static const cJSON *result(const cJSON *frame)
{
    return cJSON_GetObjectItemCaseSensitive(frame, "result");
}

static bool is_error(const cJSON *frame, const char *contains)
{
    const char *e = str(frame, "error");
    return e[0] && strcmp(str(frame, "type"), "error") == 0 && strcmp(str(frame, "id"), "req_test") == 0 &&
           strstr(e, contains) != NULL;
}

static bool header_has(const cJSON *res, const char *name, int index, const char *value)
{
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(res, "headers"), name);
    const cJSON *item = cJSON_GetArrayItem(list, index);
    return cJSON_IsString(item) && strcmp(item->valuestring, value) == 0;
}

// The echo endpoint returns what it received; find one request header (case-insensitive).
static int echo_header_count(const cJSON *echo, const char *name, const char **value)
{
    int count = 0;
    const cJSON *pair;
    cJSON_ArrayForEach(pair, cJSON_GetObjectItemCaseSensitive(echo, "headers"))
    {
        const cJSON *k = cJSON_GetArrayItem(pair, 0);
        if (cJSON_IsString(k) && strcasecmp(k->valuestring, name) == 0) {
            count++;
            if (value) {
                *value = cJSON_GetArrayItem(pair, 1)->valuestring;
            }
        }
    }
    return count;
}

static cJSON *echo_of(const cJSON *frame)
{
    return cJSON_Parse(str(result(frame), "body"));
}

// ---- Tests -------------------------------------------------------------------------------------

static void test_destinations(void)
{
    char err[ERR_MAX];
    set_net("192.168.5.17", "255.255.255.0");
    const char *allowed[] = {"192.168.5.50", "192.168.5.1", "10.0.0.1",    "172.16.0.1",
                             "172.31.255.1", "169.254.1.1", "192.168.1.1", "10.255.255.254"};
    for (size_t i = 0; i < sizeof(allowed) / sizeof(*allowed); ++i) {
        CHECK(lan_dest_check(ip(allowed[i]), err), "%s should be allowed: %s", allowed[i], err);
    }
    const char *refused[] = {"127.0.0.1",    "127.8.8.8",     "192.168.5.17",   "8.8.8.8",       "172.32.0.1",
                             "172.15.255.1", "0.0.0.0",       "224.0.0.251",    "239.255.255.250", "255.255.255.255",
                             "192.168.5.255", "192.168.5.0",  "100.64.0.1",     "1.1.1.1"};
    for (size_t i = 0; i < sizeof(refused) / sizeof(*refused); ++i) {
        err[0] = 0;
        CHECK(!lan_dest_check(ip(refused[i]), err) && err[0], "%s should be refused", refused[i]);
    }
    CHECK(!lan_dest_check(ip("8.8.8.8"), err) && strstr(err, "public address"), "public wording: %s", err);
    CHECK(!lan_dest_check(ip("192.168.5.17"), err) && strstr(err, "T-Circle itself"), "self wording: %s", err);
    CHECK(!lan_dest_check(ip("127.0.0.1"), err) && strstr(err, "loopback"), "loopback wording: %s", err);

    // A LAN on non-private space (carrier-grade NAT here): its own subnet counts, the rest doesn't.
    set_net("100.64.3.5", "255.255.252.0");
    CHECK(lan_dest_check(ip("100.64.1.9"), err), "own /22 subnet should be allowed: %s", err);
    CHECK(!lan_dest_check(ip("100.64.9.9"), err), "outside own subnet should be refused");
    // A huge "subnet" is not trusted as LAN.
    set_net("11.1.1.1", "255.0.0.0");
    CHECK(!lan_dest_check(ip("11.2.2.2"), err), "public /8 subnet should be refused");
    // /31 and /32 have no network/broadcast addresses.
    set_net("192.168.9.0", "255.255.255.254");
    CHECK(lan_dest_check(ip("192.168.9.1"), err), "/31 peer should be allowed: %s", err);

    g_wifi_up = false;
    CHECK(!lan_dest_check(ip("192.168.5.50"), err) && strstr(err, "not on a network"), "wifi down: %s", err);
    g_wifi_up = true;
    set_net("192.168.5.17", "255.255.255.0");

    uint32_t v;
    const char *bad_ips[] = {"010.0.0.1", "10.1", "1.2.3.256", "1.2.3.4x", "1..2.3", "", "1.2.3.4.5", "0x7f.0.0.1"};
    for (size_t i = 0; i < sizeof(bad_ips) / sizeof(*bad_ips); ++i) {
        CHECK(!parse_ipv4(bad_ips[i], &v), "'%s' is not a strict dotted quad", bad_ips[i]);
    }
}

static void test_bad_params(void)
{
    cJSON *f;
    f = run("{\"url\":\"ftp://192.168.5.50/\"}");
    CHECK(is_error(f, "must use http or https"), "ftp: %s", g_frame);
    cJSON_Delete(f);
    f = run("{\"url\":\"192.168.5.50/x\"}");
    CHECK(is_error(f, "absolute http or https URL"), "no scheme: %s", g_frame);
    cJSON_Delete(f);
    f = run("{\"url\":\"http:///x\"}");
    CHECK(is_error(f, "absolute"), "no host: %s", g_frame);
    cJSON_Delete(f);
    f = run("{\"method\":\"GET\"}");
    CHECK(is_error(f, "'url' is required"), "no url: %s", g_frame);
    cJSON_Delete(f);
    f = run("{\"url\":\"http://[fe80::1]/\"}");
    CHECK(is_error(f, "IPv6"), "ipv6: %s", g_frame);
    cJSON_Delete(f);
    f = run("{\"url\":\"http://u:p@192.168.5.50/\"}");
    CHECK(is_error(f, "credentials in the URL"), "userinfo: %s", g_frame);
    cJSON_Delete(f);
    f = run("{\"url\":\"http://192.168.5.50:99999/\"}");
    CHECK(is_error(f, "invalid port"), "port: %s", g_frame);
    cJSON_Delete(f);
    f = run("{\"url\":\"http://192.168.5.50:8a/\"}");
    CHECK(is_error(f, "invalid port"), "port letters: %s", g_frame);
    cJSON_Delete(f);
    f = run("{\"url\":\"http://192.168.5.50/a b\"}");
    CHECK(is_error(f, "spaces"), "space: %s", g_frame);
    cJSON_Delete(f);
    f = run("{\"method\":\"TRACE\",\"url\":\"http://192.168.5.50/\"}");
    CHECK(is_error(f, "unsupported HTTP method: TRACE"), "trace: %s", g_frame);
    cJSON_Delete(f);
    f = run("{\"url\":\"http://8.8.8.8/\"}");
    CHECK(is_error(f, "public address"), "public: %s", g_frame);
    CHECK(g_connect_host[0] == 0, "no connection may be made to a refused address");
    cJSON_Delete(f);
    f = run("{\"url\":\"http://localhost:%d/cl\"}", g_port);
    CHECK(is_error(f, "loopback"), "localhost resolves to loopback: %s", g_frame);
    cJSON_Delete(f);
    f = run("{\"url\":\"http://no-such-host.invalid/\"}");
    CHECK(is_error(f, "could not resolve"), "dns: %s", g_frame);
    cJSON_Delete(f);
    f = run("{\"url\":\"http://192.168.5.50:%d/echo\",\"headers\":{\"X-Bad\":\"a\\r\\nInjected: 1\"}}", g_port);
    CHECK(is_error(f, "line break"), "header injection: %s", g_frame);
    cJSON_Delete(f);
    f = run("{\"url\":\"http://192.168.5.50:1/\",\"method\":\"GET\"}");
    CHECK(is_error(f, "connect to 192.168.5.50:1 failed"), "refused: %s", g_frame);
    cJSON_Delete(f);
}

static void test_responses(void)
{
    char url[128];
    cJSON *f;
    const cJSON *r;

    snprintf(url, sizeof(url), "http://192.168.5.50:%d/cl", g_port);
    f = run("{\"url\":\"%s\"}", url);
    r = result(f);
    CHECK(strcmp(str(f, "type"), "response") == 0 && strcmp(str(f, "id"), "req_test") == 0, "%s", g_frame);
    CHECK(num(r, "status") == 200, "status");
    CHECK(str(r, "body") && strcmp(str(r, "body"), "hello") == 0, "body");
    CHECK(cJSON_IsFalse(cJSON_GetObjectItem(r, "truncated")), "not truncated");
    CHECK(num(r, "body_bytes") == 5, "body_bytes");
    CHECK(strcmp(str(r, "final_url"), url) == 0, "final_url is the request URL");
    CHECK(strcmp(str(r, "method"), "POST") == 0, "default method POST");
    CHECK(header_has(r, "Content-Type", 0, "text/plain") && header_has(r, "Content-Length", 0, "5"),
          "headers as arrays: %s", g_frame);
    CHECK(strcmp(g_connect_host, "192.168.5.50") == 0, "connects to the checked IP, got %s", g_connect_host);
    cJSON_Delete(f);

    f = run("{\"method\":\"get\",\"url\":\"http://192.168.5.50:%d/chunked\"}", g_port);
    r = result(f);
    CHECK(str(r, "body") && strcmp(str(r, "body"), "hello world") == 0, "chunked: %s", g_frame);
    CHECK(strcmp(str(r, "method"), "GET") == 0, "method upper-cased");
    cJSON_Delete(f);

    f = run("{\"method\":\"GET\",\"url\":\"http://192.168.5.50:%d/close\"}", g_port);
    r = result(f);
    CHECK(str(r, "body") && strcmp(str(r, "body"), "no length here") == 0, "close-delimited: %s", g_frame);
    CHECK(header_has(r, "Server", 0, "tiny"), "http/1.0 headers");
    cJSON_Delete(f);

    f = run("{\"method\":\"GET\",\"url\":\"http://192.168.5.50:%d/big\",\"max_bytes\":10485760}", g_port);
    r = result(f);
    CHECK(num(r, "body_bytes") == LAN_HTTP_MAX_BYTES && strlen(str(r, "body")) == LAN_HTTP_MAX_BYTES,
          "capped at the device max: %g", num(r, "body_bytes"));
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(r, "truncated")), "truncated");
    cJSON_Delete(f);

    f = run("{\"method\":\"GET\",\"url\":\"http://192.168.5.50:%d/big\",\"max_bytes\":100}", g_port);
    r = result(f);
    CHECK(num(r, "body_bytes") == 100 && cJSON_IsTrue(cJSON_GetObjectItem(r, "truncated")), "max_bytes 100");
    cJSON_Delete(f);

    f = run("{\"method\":\"GET\",\"url\":\"http://192.168.5.50:%d/exact\",\"max_bytes\":10}", g_port);
    r = result(f);
    CHECK(num(r, "body_bytes") == 10 && cJSON_IsFalse(cJSON_GetObjectItem(r, "truncated")),
          "exactly max_bytes is not truncated: %s", g_frame);
    cJSON_Delete(f);

    f = run("{\"method\":\"GET\",\"url\":\"http://192.168.5.50:%d/redirect\"}", g_port);
    r = result(f);
    CHECK(num(r, "status") == 302 && header_has(r, "Location", 0, "http://192.168.5.50/elsewhere"),
          "redirect returned as is: %s", g_frame);
    cJSON_Delete(f);

    f = run("{\"method\":\"GET\",\"url\":\"http://192.168.5.50:%d/dup\"}", g_port);
    r = result(f);
    CHECK(header_has(r, "Set-Cookie", 0, "a=1") && header_has(r, "Set-Cookie", 1, "b=2") &&
              header_has(r, "X-Thing", 0, "y"),
          "repeated headers, canonical names: %s", g_frame);
    cJSON_Delete(f);

    f = run("{\"method\":\"GET\",\"url\":\"http://192.168.5.50:%d/binary\"}", g_port);
    CHECK(strstr(g_frame, "\"body\":\"A\\u0000B\xEF\xBF\xBD\\\"\\\\\\n\xEF\xBF\xBD\xEF\xBF\xBD\"") != NULL,
          "NUL escaped, invalid UTF-8 replaced: %s", g_frame);
    CHECK(num(result(f), "body_bytes") == 9, "body_bytes counts raw bytes");
    cJSON_Delete(f);

    f = run("{\"method\":\"GET\",\"url\":\"http://192.168.5.50:%d/utf8\"}", g_port);
    CHECK(strcmp(str(result(f), "body"), "灯 on ✓") == 0, "utf-8 kept: %s", g_frame);
    cJSON_Delete(f);

    f = run("{\"method\":\"GET\",\"url\":\"http://192.168.5.50:%d/status500\"}", g_port);
    CHECK(num(result(f), "status") == 500 && strcmp(str(result(f), "body"), "oops") == 0, "500 is a response");
    cJSON_Delete(f);

    const int64_t t0 = esp_timer_get_time();
    f = run("{\"method\":\"GET\",\"url\":\"http://192.168.5.50:%d/slow\",\"timeout_seconds\":1}", g_port);
    const int64_t took_ms = (esp_timer_get_time() - t0) / 1000;
    CHECK(is_error(f, "timed out after 1 s"), "timeout: %s", g_frame);
    CHECK(took_ms >= 900 && took_ms < 2000, "timeout honoured, took %lld ms", (long long)took_ms);
    cJSON_Delete(f);

    f = run("{\"method\":\"GET\",\"url\":\"http://192.168.5.50:%d/early-close\"}", g_port);
    CHECK(is_error(f, "closed the connection before"), "early close: %s", g_frame);
    cJSON_Delete(f);

    f = run("{\"method\":\"GET\",\"url\":\"http://192.168.5.50:%d/garbage\"}", g_port);
    CHECK(is_error(f, "invalid HTTP response"), "garbage: %s", g_frame);
    cJSON_Delete(f);
}

static void test_requests(void)
{
    cJSON *f;
    cJSON *echo;
    const char *v = NULL;
    char host[64];
    snprintf(host, sizeof(host), "192.168.5.50:%d", g_port);

    f = run("{\"url\":\"http://192.168.5.50:%d/echo?x=1#frag\",\"body\":\"{\\\"a\\\":1}\"}", g_port);
    echo = echo_of(f);
    CHECK(echo && strcmp(str(echo, "method"), "POST") == 0 && strcmp(str(echo, "path"), "/echo?x=1") == 0,
          "request line, no fragment: %s", g_frame);
    CHECK(echo && strcmp(str(echo, "body"), "{\"a\":1}") == 0, "body sent");
    CHECK(echo_header_count(echo, "Content-Type", &v) == 1 && strcmp(v, "application/json") == 0,
          "default content type");
    CHECK(echo_header_count(echo, "Content-Length", &v) == 1 && strcmp(v, "7") == 0, "content length");
    CHECK(echo_header_count(echo, "Host", &v) == 1 && strcmp(v, host) == 0, "Host %s", v);
    CHECK(echo_header_count(echo, "Connection", &v) == 1 && strcmp(v, "close") == 0, "connection close");
    CHECK(echo_header_count(echo, "User-Agent", &v) == 1 && strcmp(v, USER_AGENT) == 0, "default UA");
    cJSON_Delete(echo);
    cJSON_Delete(f);

    f = run("{\"method\":\"PUT\",\"url\":\"http://192.168.5.50:%d/echo\",\"body\":\"on\","
            "\"headers\":{\"content-type\":\"text/plain\",\"X-Num\":5,\"X-Flag\":true,\"Host\":\"evil\","
            "\"User-Agent\":\"me\",\"Content-Length\":\"999\",\"\":\"skip\"}}",
            g_port);
    echo = echo_of(f);
    CHECK(echo && echo_header_count(echo, "Content-Type", &v) == 1 && strcmp(v, "text/plain") == 0,
          "caller content type kept, no default added: %s", g_frame);
    CHECK(echo && echo_header_count(echo, "X-Num", &v) == 1 && strcmp(v, "5") == 0, "number header");
    CHECK(echo && echo_header_count(echo, "X-Flag", &v) == 1 && strcmp(v, "true") == 0, "bool header");
    CHECK(echo && echo_header_count(echo, "Host", &v) == 1 && strcmp(v, host) == 0, "Host can't be overridden");
    CHECK(echo && echo_header_count(echo, "User-Agent", &v) == 1 && strcmp(v, "me") == 0, "caller UA");
    CHECK(echo && echo_header_count(echo, "Content-Length", &v) == 1 && strcmp(v, "2") == 0, "real length");
    cJSON_Delete(echo);
    cJSON_Delete(f);

    f = run("{\"method\":\"GET\",\"url\":\"http://192.168.5.50:%d\"}", g_port);
    echo = echo_of(f);
    CHECK(echo && strcmp(str(echo, "path"), "/") == 0, "empty path becomes /: %s", g_frame);
    CHECK(echo && echo_header_count(echo, "Content-Length", NULL) == 0, "GET without body has no length");
    CHECK(echo && echo_header_count(echo, "Content-Type", NULL) == 0, "GET without body has no type");
    cJSON_Delete(echo);
    cJSON_Delete(f);

    f = run("{\"method\":\"DELETE\",\"url\":\"http://192.168.5.50:%d/echo\"}", g_port);
    echo = echo_of(f);
    CHECK(echo && echo_header_count(echo, "Content-Length", NULL) == 0, "DELETE without body: %s", g_frame);
    cJSON_Delete(echo);
    cJSON_Delete(f);

    f = run("{\"method\":\"POST\",\"url\":\"http://192.168.5.50:%d/echo\"}", g_port);
    echo = echo_of(f);
    CHECK(echo && echo_header_count(echo, "Content-Length", &v) == 1 && strcmp(v, "0") == 0,
          "empty POST sends Content-Length: 0");
    cJSON_Delete(echo);
    cJSON_Delete(f);
}

// One chunk-mode fetch of server path `path`, checked against the pattern: frames in order, bytes
// identical, final frame consistent.
static void check_chunked(const char *path, const char *extra, size_t frame_bytes, size_t want_bytes,
                          bool want_truncated, unsigned want_chunks)
{
    sink_reset(frame_bytes, 0);
    cJSON *f = run("{\"method\":\"GET\",\"url\":\"http://192.168.5.50:%d%s\",\"response_mode\":\"chunks\"%s}", g_port,
                   path, extra);
    const cJSON *r = result(f);
    CHECK(strcmp(str(f, "type"), "response") == 0 && num(r, "status") == 200, "%s: %.300s", path, g_frame);
    CHECK(!g_sink.bad, "%s: %s", path, g_sink.why);
    CHECK(strcmp(str(r, "body_encoding"), "chunks") == 0, "%s: body_encoding", path);
    CHECK(cJSON_GetObjectItemCaseSensitive(r, "body") == NULL, "%s: no text body in chunk mode", path);
    CHECK(num(r, "chunks") == g_sink.frames, "%s: chunks %g, frames sent %u", path, num(r, "chunks"), g_sink.frames);
    if (want_chunks) {
        CHECK(g_sink.frames == want_chunks, "%s: %u frames, want %u", path, g_sink.frames, want_chunks);
    }
    CHECK(num(r, "body_bytes") == want_bytes && g_sink.len == want_bytes, "%s: body_bytes %g, got %zu, want %zu", path,
          num(r, "body_bytes"), g_sink.len, want_bytes);
    CHECK(cJSON_IsBool(cJSON_GetObjectItem(r, "truncated")) &&
              cJSON_IsTrue(cJSON_GetObjectItem(r, "truncated")) == want_truncated,
          "%s: truncated", path);
    size_t mismatch = SIZE_MAX;
    for (size_t i = 0; i < g_sink.len; ++i) {
        if (g_sink.data[i] != pattern_byte(i)) {
            mismatch = i;
            break;
        }
    }
    CHECK(mismatch == SIZE_MAX, "%s: byte %zu differs", path, mismatch);
    char hex[65];
    sha256_hex(g_sink.data, g_sink.len, hex);
    CHECK(strcmp(str(r, "sha256"), hex) == 0, "%s: sha256 %s, want %s", path, str(r, "sha256"), hex);
    CHECK(strcmp(str(r, "method"), "GET") == 0 && strstr(str(r, "final_url"), path), "%s: method/final_url", path);
    cJSON_Delete(f);
}

static void test_chunks(void)
{
    cJSON *f;
    char extra[64];

    // The prompt's test files: 10 KB, an 800 KB "JPEG", 5 MB (over the 4 MB cap), every byte value.
    check_chunked("/pattern/10240", "", 8192, 10240, false, 2);
    check_chunked("/pattern/819200", "", LAN_HTTP_CHUNK_BYTES, 819200, false, 25);
    check_chunked("/pattern/819200", "", 8192, 819200, false, 100);
    check_chunked("/pattern/5242880", "", LAN_HTTP_CHUNK_BYTES, LAN_HTTP_CHUNK_MAX_TOTAL, true, 128);
    check_chunked("/pattern/65536", "", 999, 65536, false, 66);  // every byte value, many times
    // max_bytes over the cap means the cap; under it, it's the limit.
    snprintf(extra, sizeof(extra), ",\"max_bytes\":10485760");
    check_chunked("/pattern/5242880", extra, LAN_HTTP_CHUNK_BYTES, LAN_HTTP_CHUNK_MAX_TOTAL, true, 128);
    snprintf(extra, sizeof(extra), ",\"max_bytes\":100000");
    check_chunked("/pattern/819200", extra, 8192, 100000, true, 13);
    snprintf(extra, sizeof(extra), ",\"max_bytes\":819200");
    check_chunked("/pattern/819200", extra, 8192, 819200, false, 100);
    // Over the text cap (64 KB) is fine in chunk mode.
    check_chunked("/pattern/70000", "", LAN_HTTP_CHUNK_BYTES, 70000, false, 3);
    // Other framings, and a frame size that changes from frame to frame.
    check_chunked("/pattern-chunked/300001", "", 0, 300001, false, 0);
    check_chunked("/pattern-close/70001", "", 4096, 70001, false, 18);
    // frame_bytes() above the device max is clamped.
    check_chunked("/pattern/100000", "", 1 << 20, 100000, false, 4);

    // Empty body: no chunk frames, the digest of nothing.
    sink_reset(8192, 0);
    f = run("{\"method\":\"GET\",\"url\":\"http://192.168.5.50:%d/redirect\",\"response_mode\":\"chunks\"}", g_port);
    CHECK(num(result(f), "status") == 302 && num(result(f), "chunks") == 0 && num(result(f), "body_bytes") == 0 &&
              g_sink.frames == 0 &&
              strcmp(str(result(f), "sha256"), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0,
          "empty body: %s", g_frame);
    CHECK(header_has(result(f), "Location", 0, "http://192.168.5.50/elsewhere"), "headers in the final frame");
    cJSON_Delete(f);

    // The sink fails on the 3rd frame: an error frame with its message, nothing after.
    sink_reset(8192, 3);
    f = run("{\"method\":\"GET\",\"url\":\"http://192.168.5.50:%d/pattern/819200\",\"response_mode\":\"chunks\"}",
            g_port);
    CHECK(is_error(f, "socket closed during the transfer") && g_sink.frames == 2, "send failure: %u frames, %s",
          g_sink.frames, g_frame);
    cJSON_Delete(f);

    // Failures before any body: plain error frames, no chunks.
    sink_reset(8192, 0);
    f = run("{\"method\":\"GET\",\"url\":\"http://192.168.5.50:%d/early-close\",\"response_mode\":\"chunks\"}",
            g_port);
    CHECK(is_error(f, "closed the connection before"), "early close in chunk mode: %s", g_frame);
    cJSON_Delete(f);
    f = run("{\"url\":\"http://8.8.8.8/\",\"response_mode\":\"chunks\"}");
    CHECK(is_error(f, "public address") && g_sink.frames == 0, "public refused in chunk mode: %s", g_frame);
    cJSON_Delete(f);

    // Unknown modes are refused; null means text mode.
    f = run("{\"method\":\"GET\",\"url\":\"http://192.168.5.50:%d/cl\",\"response_mode\":\"binary\"}", g_port);
    CHECK(is_error(f, "unsupported response_mode"), "unknown mode: %s", g_frame);
    cJSON_Delete(f);
    f = run("{\"method\":\"GET\",\"url\":\"http://192.168.5.50:%d/cl\",\"response_mode\":null}", g_port);
    CHECK(strcmp(str(result(f), "body"), "hello") == 0, "null mode is text: %s", g_frame);
    cJSON_Delete(f);

    // Without a sink (no Connector to stream to), chunk mode is refused.
    g_use_sink = NULL;
    f = run("{\"method\":\"GET\",\"url\":\"http://192.168.5.50:%d/cl\",\"response_mode\":\"chunks\"}", g_port);
    CHECK(is_error(f, "not available"), "no sink: %s", g_frame);
    cJSON_Delete(f);
    g_use_sink = &SINK;

    // Text mode never touches the sink.
    sink_reset(8192, 0);
    f = run("{\"method\":\"GET\",\"url\":\"http://192.168.5.50:%d/pattern/100000\"}", g_port);
    CHECK(num(result(f), "body_bytes") == LAN_HTTP_MAX_BYTES && cJSON_IsTrue(cJSON_GetObjectItem(result(f), "truncated")) &&
              g_sink.calls == 0 && g_sink.frames == 0 && cJSON_GetObjectItem(result(f), "body_encoding") == NULL,
          "text mode unchanged: %.200s", g_frame);
    cJSON_Delete(f);

    // A voice call is active: chunk mode is refused with the busy message before any network use
    // (no DNS, no connect, no frames); text mode carries on as usual.
    sink_reset(8192, 0);
    g_sink.busy = "busy: voice call active";
    f = run("{\"method\":\"GET\",\"url\":\"http://192.168.5.50:%d/pattern/819200\",\"response_mode\":\"chunks\"}",
            g_port);
    CHECK(is_error(f, "busy: voice call active") && strcmp(str(f, "error"), "busy: voice call active") == 0,
          "busy: %s", g_frame);
    CHECK(g_connect_host[0] == 0 && g_sink.frames == 0 && g_sink.calls == 0, "busy touched the network: %s, %u frames",
          g_connect_host, g_sink.frames);
    cJSON_Delete(f);
    f = run("{\"url\":\"http://no-such-host.invalid/\",\"response_mode\":\"chunks\"}");
    CHECK(is_error(f, "busy: voice call active"), "busy before DNS: %s", g_frame);
    cJSON_Delete(f);
    const unsigned asked = g_sink.refuse_calls;
    f = run("{\"method\":\"GET\",\"url\":\"http://192.168.5.50:%d/cl\"}", g_port);
    CHECK(strcmp(str(result(f), "body"), "hello") == 0 && g_sink.refuse_calls == asked, "text mode during a call: %s",
          g_frame);
    cJSON_Delete(f);
    // A call that starts mid-transfer: refuse() let it start, the 4th send fails with the busy
    // message (as connector.c does), and the transfer ends with that error after 3 frames.
    sink_reset(8192, 4);
    g_sink.fail_msg = "busy: voice call active";
    f = run("{\"method\":\"GET\",\"url\":\"http://192.168.5.50:%d/pattern/819200\",\"response_mode\":\"chunks\"}",
            g_port);
    CHECK(strcmp(str(f, "error"), "busy: voice call active") == 0 && g_sink.frames == 3 && g_sink.refuse_calls == 1,
          "call mid-transfer: %u frames, %s", g_sink.frames, g_frame);
    cJSON_Delete(f);

    // base64 of the encoder itself, the RFC 4648 §10 vectors.
    const char *vec[][2] = {{"", ""}, {"f", "Zg=="}, {"fo", "Zm8="}, {"foo", "Zm9v"}, {"foob", "Zm9vYg=="},
                            {"fooba", "Zm9vYmE="}, {"foobar", "Zm9vYmFy"}};
    for (size_t i = 0; i < sizeof(vec) / sizeof(*vec); ++i) {
        char out[16];
        const size_t n = b64_encode((const uint8_t *)vec[i][0], strlen(vec[i][0]), out);
        out[n] = 0;
        CHECK(strcmp(out, vec[i][1]) == 0 && n == B64_LEN(strlen(vec[i][0])), "b64(%s) = %s", vec[i][0], out);
    }
    sink_reset(0, 0);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: test <server port>\n");
        return 2;
    }
    g_port = atoi(argv[1]);
    test_destinations();
    test_bad_params();
    test_responses();
    test_requests();
    test_chunks();
    printf("%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
