// Host test for main/lan_tunnel.c (lan_tcp_dial, G3): builds it on the Mac with BSD sockets and a
// pthread stand-in for FreeRTOS (stubs/), so the tunnel task really runs on its own thread while
// this one plays the Connector task and the server. Every LAN address the firmware dials goes to a
// local server picked per test (test_connect); frames to the server are captured and checked.
// Run: ./run.sh

#include "../../main/lan_dest.c"
#include "../../main/lan_tunnel.c"

#undef connect

#include <pthread.h>
#include <stdatomic.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/time.h>

// ---- FreeRTOS stand-in ---------------------------------------------------------------------------

struct test_queue {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    size_t len, size, head, count;
    unsigned char *items;
};

QueueHandle_t xQueueCreate(size_t len, size_t item_size)
{
    QueueHandle_t q = calloc(1, sizeof(*q));
    pthread_mutex_init(&q->mu, NULL);
    pthread_cond_init(&q->cv, NULL);
    q->len = len;
    q->size = item_size;
    q->items = calloc(len, item_size);
    return q;
}

void vQueueDelete(QueueHandle_t q)
{
    pthread_mutex_destroy(&q->mu);
    pthread_cond_destroy(&q->cv);
    free(q->items);
    free(q);
}

static atomic_size_t g_fail_alloc_size;
static atomic_bool g_fail_command_send;
static atomic_bool g_pause_consumer, g_consumer_paused, g_failure_queue_waiting;
void *test_heap_alloc(size_t n)
{
    if (n == g_fail_alloc_size) { g_fail_alloc_size = 0; return NULL; }
    return malloc(n);
}

static void deadline_after(struct timespec *ts, TickType_t ms)
{
    struct timeval now;
    gettimeofday(&now, NULL);
    int64_t ns = (int64_t)now.tv_usec * 1000 + (int64_t)ms * 1000000;
    ts->tv_sec = now.tv_sec + ns / 1000000000;
    ts->tv_nsec = ns % 1000000000;
}

BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t wait)
{
    if (q == s_cmds && atomic_exchange(&g_fail_command_send, false)) return pdFALSE;
    struct timespec ts;
    deadline_after(&ts, wait == portMAX_DELAY ? 3600000 : wait);
    pthread_mutex_lock(&q->mu);
    while (q->count == q->len) {
        if (q == s_command_failures) g_failure_queue_waiting = true;
        if (wait == 0 || pthread_cond_timedwait(&q->cv, &q->mu, &ts) != 0) {
            pthread_mutex_unlock(&q->mu);
            return pdFALSE;
        }
    }
    memcpy(q->items + ((q->head + q->count) % q->len) * q->size, item, q->size);
    q->count++;
    pthread_cond_broadcast(&q->cv);
    pthread_mutex_unlock(&q->mu);
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t wait)
{
    if (q == s_cmds) {
        while (g_pause_consumer) {
            g_consumer_paused = true;
            usleep(1000);
        }
        g_consumer_paused = false;
    }
    struct timespec ts;
    deadline_after(&ts, wait == portMAX_DELAY ? 3600000 : wait);
    pthread_mutex_lock(&q->mu);
    while (q->count == 0) {
        if (wait == 0 || pthread_cond_timedwait(&q->cv, &q->mu, &ts) != 0) {
            pthread_mutex_unlock(&q->mu);
            return pdFALSE;
        }
    }
    memcpy(item, q->items + q->head * q->size, q->size);
    q->head = (q->head + 1) % q->len;
    q->count--;
    pthread_cond_broadcast(&q->cv);
    pthread_mutex_unlock(&q->mu);
    return pdTRUE;
}

static void *task_trampoline(void *arg)
{
    void **a = arg;
    ((void (*)(void *))a[0])(a[1]);
    return NULL;
}

BaseType_t xTaskCreateWithCaps(void (*fn)(void *), const char *name, size_t stack, void *arg, int prio,
                               TaskHandle_t *out, unsigned caps)
{
    static void *a[2];
    a[0] = (void *)fn;
    a[1] = arg;
    pthread_t th;
    return pthread_create(&th, NULL, task_trampoline, a) == 0 ? pdPASS : pdFALSE;
}

void vTaskDelay(TickType_t ticks)
{
    usleep(ticks * 1000);
}

// ---- Harness -------------------------------------------------------------------------------------

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

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

// ---- The network ---------------------------------------------------------------------------------

#define SELF "192.168.5.17"

bool wifi_ip_info(uint32_t *ip, uint32_t *netmask)
{
    *ip = inet_addr(SELF);
    *netmask = inet_addr("255.255.255.0");
    return true;
}

// Where the next dial goes: a loopback port, and optionally a small send buffer on the device's
// socket (so data from the server queues on the device).
static atomic_int g_route_port;
static atomic_int g_route_sndbuf;
static atomic_int g_connects;
// The next dial never completes (a SYN nobody answers): the socket becomes one end of a socket pair
// that is never writable. The other end stays open so it never reads as closed either.
static atomic_bool g_route_blackhole;
static int g_blackhole_writer = -1;
static char g_dialed[32];

int test_connect(int fd, const struct sockaddr *to, socklen_t len)
{
    const struct sockaddr_in *in = (const struct sockaddr_in *)to;
    char ip[16];
    inet_ntop(AF_INET, &in->sin_addr, ip, sizeof(ip));
    snprintf(g_dialed, sizeof(g_dialed), "%s:%d", ip, ntohs(in->sin_port));
    g_connects++;
    if (g_route_blackhole) {
        // A socket pair whose send buffer is already full is never writable.
        int p[2];
        socketpair(AF_UNIX, SOCK_STREAM, 0, p);
        int fl = fcntl(p[0], F_GETFL, 0);
        fcntl(p[0], F_SETFL, fl | O_NONBLOCK);
        static char junk[4096];
        while (send(p[0], junk, sizeof(junk), 0) > 0) {
        }
        dup2(p[0], fd);
        close(p[0]);
        if (g_blackhole_writer >= 0) {
            close(g_blackhole_writer);
        }
        g_blackhole_writer = p[1];
        errno = EINPROGRESS;
        return -1;
    }
    if (g_route_sndbuf) {
        int n = g_route_sndbuf;
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &n, sizeof(n));
    }
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)g_route_port)};
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return connect(fd, (struct sockaddr *)&a, sizeof(a));
}

static int listen_on(int *port, int backlog)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = {.sin_family = AF_INET};
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(s, (struct sockaddr *)&a, sizeof(a));
    listen(s, backlog);
    socklen_t l = sizeof(a);
    getsockname(s, (struct sockaddr *)&a, &l);
    *port = ntohs(a.sin_port);
    return s;
}

// The LAN hosts. Each accepted connection gets its own thread.
typedef enum { SRV_ECHO, SRV_SOURCE, SRV_STALL, SRV_RESET } srv_kind_t;

typedef struct {
    srv_kind_t kind;
    int listen_fd;
    int port;
    const unsigned char *source;  // SRV_SOURCE: send these bytes, then close
    size_t source_len;
    atomic_int accepted;
    atomic_int closed;  // connections that saw EOF or an error from the device
    atomic_long received;
} server_t;

typedef struct {
    server_t *srv;
    int fd;
} conn_arg_t;

static void *conn_thread(void *arg)
{
    conn_arg_t *c = arg;
    server_t *s = c->srv;
    unsigned char buf[65536];
    if (s->kind == SRV_SOURCE) {
        size_t off = 0;
        while (off < s->source_len) {
            ssize_t n = send(c->fd, s->source + off, s->source_len - off, 0);
            if (n <= 0) {
                break;
            }
            off += (size_t)n;
        }
        close(c->fd);
        s->closed++;
    } else if (s->kind == SRV_RESET) {
        recv(c->fd, buf, sizeof(buf), 0);  // the first request, then RST
        struct linger lg = {.l_onoff = 1, .l_linger = 0};
        setsockopt(c->fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
        close(c->fd);
        s->closed++;
    } else if (s->kind == SRV_STALL) {
        // Never reads; waits for the device to close.
        while (recv(c->fd, buf, 1, MSG_PEEK) != 0) {
            usleep(20000);
            int avail = 0;
            socklen_t l = sizeof(avail);
            (void)l;
            fd_set r;
            FD_ZERO(&r);
            FD_SET(c->fd, &r);
            struct timeval tv = {0, 0};
            if (select(c->fd + 1, &r, NULL, NULL, &tv) < 0) {
                break;
            }
        }
        close(c->fd);
        s->closed++;
    } else {
        for (;;) {
            ssize_t n = recv(c->fd, buf, sizeof(buf), 0);
            if (n <= 0) {
                break;
            }
            s->received += n;
            size_t off = 0;
            while (off < (size_t)n) {
                ssize_t w = send(c->fd, buf + off, (size_t)n - off, 0);
                if (w <= 0) {
                    break;
                }
                off += (size_t)w;
            }
        }
        close(c->fd);
        s->closed++;
    }
    free(c);
    return NULL;
}

static void *accept_thread(void *arg)
{
    server_t *s = arg;
    for (;;) {
        int fd = accept(s->listen_fd, NULL, NULL);
        if (fd < 0) {
            return NULL;
        }
        if (s->kind == SRV_STALL) {
            int n = 4096;
            setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &n, sizeof(n));
        }
        s->accepted++;
        conn_arg_t *c = malloc(sizeof(*c));
        c->srv = s;
        c->fd = fd;
        pthread_t th;
        pthread_create(&th, NULL, conn_thread, c);
        pthread_detach(th);
    }
}

static server_t *server(srv_kind_t kind)
{
    server_t *s = calloc(1, sizeof(*s));
    s->kind = kind;
    s->listen_fd = listen_on(&s->port, 16);
    pthread_t th;
    pthread_create(&th, NULL, accept_thread, s);
    pthread_detach(th);
    return s;
}

static void route(const server_t *s)
{
    g_route_port = s->port;
    g_route_sndbuf = 0;
}

// ---- The host (Connector side) -------------------------------------------------------------------

#define SESSION 7

static pthread_mutex_t g_fmu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_fcv = PTHREAD_COND_INITIALIZER;
static cJSON *g_frames[4096];
static int g_nframes, g_fhead;
static atomic_int g_wrong_session;

static atomic_uint g_session = SESSION;
static atomic_bool g_read_only;
static atomic_bool g_call;
static atomic_int g_send_delay_ms;

static bool host_send(void *ctx, unsigned session, const char *frame, size_t len)
{
    if (session != g_session) {
        g_wrong_session++;
        return false;
    }
    if (g_send_delay_ms) {
        usleep(g_send_delay_ms * 1000);
    }
    cJSON *f = cJSON_ParseWithLength(frame, len);
    if (!f) {
        fprintf(stderr, "FAIL: the device sent invalid JSON: %.200s\n", frame);
        g_failed++;
        return true;
    }
    pthread_mutex_lock(&g_fmu);
    g_frames[g_nframes++ % 4096] = f;
    pthread_cond_broadcast(&g_fcv);
    pthread_mutex_unlock(&g_fmu);
    return true;
}

static lan_tunnel_verdict_t host_check(void *ctx, unsigned session, const char **reason)
{
    if (session != g_session) {
        return LAN_TUNNEL_END_SILENT;
    }
    if (g_read_only) {
        *reason = "method \"lan_tcp_dial\" is not permitted for a local_file_read connector";
        return LAN_TUNNEL_END;
    }
    if (g_call) {
        *reason = "busy: voice call active";
        return LAN_TUNNEL_END;
    }
    return LAN_TUNNEL_GO;
}

// The next frame from the device (caller deletes it), or NULL after timeout_ms.
static cJSON *next_frame(int timeout_ms)
{
    struct timespec ts;
    deadline_after(&ts, (TickType_t)timeout_ms);
    pthread_mutex_lock(&g_fmu);
    while (g_fhead == g_nframes) {
        if (pthread_cond_timedwait(&g_fcv, &g_fmu, &ts) != 0) {
            pthread_mutex_unlock(&g_fmu);
            return NULL;
        }
    }
    cJSON *f = g_frames[g_fhead++ % 4096];
    pthread_mutex_unlock(&g_fmu);
    return f;
}

static void drop_frames(void)
{
    cJSON *f;
    while ((f = next_frame(0))) {
        cJSON_Delete(f);
    }
}

static const char *str(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static const cJSON *obj(const cJSON *o, const char *k)
{
    return cJSON_GetObjectItemCaseSensitive(o, k);
}

static bool is_kind(const cJSON *f, const char *type, const char *id)
{
    return f && str(f, "type") && strcmp(str(f, "type"), type) == 0 && str(f, "id") && strcmp(str(f, "id"), id) == 0;
}

static const char *channel(const cJSON *f)
{
    return str(obj(f, "stream"), "channel");
}

static bool is_eof(const cJSON *f)
{
    return f && channel(f) && strcmp(channel(f), "tcp") == 0 && cJSON_IsTrue(obj(obj(f, "stream"), "eof"));
}

static int count_keys(const cJSON *o)
{
    int n = 0;
    const cJSON *c;
    cJSON_ArrayForEach(c, o) n++;
    return n;
}

// ---- Server side of the protocol -----------------------------------------------------------------

static void dial(const char *id, const char *params_json)
{
    cJSON *p = params_json ? cJSON_Parse(params_json) : NULL;
    CHECK(lan_tunnel_dial(id, p, g_session), "dial %s not queued", id);
    cJSON_Delete(p);
}

static void server_frame(const char *json)
{
    cJSON *m = cJSON_Parse(json);
    const char *id = str(m, "id");
    CHECK(lan_tunnel_frame(id, m, g_session), "not taken as a tunnel frame: %.100s", json);
    cJSON_Delete(m);
}

static void server_data(const char *id, const unsigned char *data, size_t n)
{
    char *b64 = malloc(B64_LEN(n) + 1);
    b64[lan_tunnel_b64_encode(data, n, b64)] = 0;
    char *json = malloc(B64_LEN(n) + 200);
    sprintf(json, "{\"id\":\"%s\",\"type\":\"stream\",\"stream\":{\"channel\":\"tcp\",\"data\":\"%s\"}}", id, b64);
    server_frame(json);
    free(b64);
    free(json);
}

static void server_ack(const char *id, long long seq)
{
    char json[200];
    snprintf(json, sizeof(json), "{\"id\":\"%s\",\"type\":\"stream\",\"stream\":{\"channel\":\"tcp_ack\",\"seq\":%lld}}", id, seq);
    server_frame(json);
}

static void server_eof(const char *id)
{
    char json[200];
    snprintf(json, sizeof(json), "{\"id\":\"%s\",\"type\":\"stream\",\"stream\":{\"channel\":\"tcp\",\"eof\":true}}", id);
    server_frame(json);
}

// Dials and waits for the answer; true when connected. *answer gets the frame unless NULL.
static bool open_tunnel(const char *id, const char *params_json, cJSON **answer)
{
    dial(id, params_json);
    cJSON *f = next_frame(3000);
    const bool ok = is_kind(f, "response", id);
    if (answer) {
        *answer = f;
    } else {
        cJSON_Delete(f);
    }
    return ok;
}

static bool wait_until(atomic_int *v, int want, int timeout_ms)
{
    const int64_t until = now_ms() + timeout_ms;
    while (*v < want && now_ms() < until) {
        usleep(5000);
    }
    return *v >= want;
}

static bool wait_no_tunnels(int timeout_ms)
{
    const int64_t until = now_ms() + timeout_ms;
    while (lan_tunnel_count() != 0 && now_ms() < until) {
        usleep(5000);
    }
    return lan_tunnel_count() == 0;
}

// Collects device data frames for `id` into buf, acking each one at once when `ack`. Stops at
// `want` bytes, at eof (*eof set), or after quiet_ms with nothing. Returns bytes collected.
static size_t collect(const char *id, unsigned char *buf, size_t want, bool ack, int quiet_ms, cJSON **eof,
                      size_t *max_frame_seen, long long *acked_by_device)
{
    size_t got = 0;
    while (got < want) {
        cJSON *f = next_frame(quiet_ms);
        if (!f) {
            break;
        }
        CHECK(str(f, "id") && strcmp(str(f, "id"), id) == 0, "frame for another id");
        CHECK(strcmp(str(f, "type"), "stream") == 0, "type %s", str(f, "type"));
        if (is_eof(f)) {
            if (eof) {
                *eof = f;
            } else {
                cJSON_Delete(f);
            }
            break;
        }
        if (strcmp(channel(f), "tcp_ack") == 0) {
            if (acked_by_device) {
                *acked_by_device = (long long)obj(obj(f, "stream"), "seq")->valuedouble;
            }
            cJSON_Delete(f);
            continue;
        }
        const char *data = str(obj(f, "stream"), "data");
        CHECK(data && count_keys(obj(f, "stream")) == 2 && count_keys(f) == 3, "data frame shape");
        const size_t text = data ? strlen(data) : 0;
        unsigned char *raw = malloc(text);
        long n = lan_tunnel_b64_decode(data, text, raw, text);
        CHECK(n > 0, "data frame not valid base64");
        if (n > 0) {
            CHECK((size_t)n <= LAN_TUNNEL_MAX_FRAME, "frame of %ld B over max_frame", n);
            if (max_frame_seen && (size_t)n > *max_frame_seen) {
                *max_frame_seen = (size_t)n;
            }
            if (got + (size_t)n <= want) {
                memcpy(buf + got, raw, (size_t)n);
            }
            got += (size_t)n;
            if (ack) {
                server_ack(id, (long long)got);
            }
        }
        free(raw);
        cJSON_Delete(f);
    }
    return got;
}

static unsigned char *pattern(size_t n, unsigned seed)
{
    unsigned char *p = malloc(n);
    uint32_t x = seed * 2654435761u + 1;
    for (size_t i = 0; i < n; ++i) {
        x = x * 1103515245u + 12345u;
        p[i] = (unsigned char)(x >> 16);
    }
    return p;
}

// ---- Tests ---------------------------------------------------------------------------------------

static void test_base64(void)
{
    static const char *const plain[] = {"", "f", "fo", "foo", "foob", "fooba", "foobar"};
    static const char *const coded[] = {"", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy"};
    for (int i = 0; i < 7; ++i) {
        char out[16];
        size_t n = lan_tunnel_b64_encode((const unsigned char *)plain[i], strlen(plain[i]), out);
        CHECK(n == strlen(coded[i]) && memcmp(out, coded[i], n) == 0, "encode %s", plain[i]);
        unsigned char back[16];
        long m = lan_tunnel_b64_decode(coded[i], strlen(coded[i]), back, sizeof(back));
        CHECK(m == (long)strlen(plain[i]) && memcmp(back, plain[i], (size_t)m) == 0, "decode %s", coded[i]);
    }
    static const char *const bad[] = {"Zg=", "Zg", "Z===", "Zg==Zg==", "Zh==", "Zm9=", "Zm9v!A==", "Zm 9v", "=Zm9"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        unsigned char out[16];
        CHECK(lan_tunnel_b64_decode(bad[i], strlen(bad[i]), out, sizeof(out)) == -1, "accepted bad base64 %s", bad[i]);
    }
    unsigned char out[2];
    CHECK(lan_tunnel_b64_decode("Zm9v", 4, out, 2) == -1, "decode past the buffer");
    unsigned char all[256], back[256];
    char text[B64_LEN(256)];
    for (int i = 0; i < 256; ++i) {
        all[i] = (unsigned char)i;
    }
    size_t n = lan_tunnel_b64_encode(all, 256, text);
    CHECK(lan_tunnel_b64_decode(text, n, back, 256) == 256 && memcmp(all, back, 256) == 0, "every byte value");
}

static void test_connect_answer_and_echo(server_t *echo)
{
    route(echo);
    cJSON *a = NULL;
    CHECK(open_tunnel("tun_1", "{\"host\":\"192.168.5.33\",\"port\":554,\"timeout_seconds\":15}", &a), "no answer");
    CHECK(strcmp(g_dialed, "192.168.5.33:554") == 0, "dialed %s", g_dialed);
    const cJSON *r = obj(a, "result");
    CHECK(count_keys(a) == 3 && count_keys(r) == 4, "answer shape");
    CHECK(str(r, "status") && strcmp(str(r, "status"), "connected") == 0, "status");
    CHECK(str(r, "remote_address") && strcmp(str(r, "remote_address"), "192.168.5.33:554") == 0, "remote_address %s",
          str(r, "remote_address"));
    CHECK(obj(r, "window")->valuedouble == LAN_TUNNEL_WINDOW, "window");
    CHECK(obj(r, "max_frame")->valuedouble == LAN_TUNNEL_MAX_FRAME, "max_frame");
    cJSON_Delete(a);

    const char *req = "OPTIONS rtsp://192.168.5.33:554/stream1 RTSP/1.0\r\nCSeq: 1\r\n\r\n";
    server_data("tun_1", (const unsigned char *)req, strlen(req));
    unsigned char back[256];
    long long device_ack = 0;
    size_t got = collect("tun_1", back, strlen(req), true, 2000, NULL, NULL, &device_ack);
    CHECK(got == strlen(req) && memcmp(back, req, got) == 0, "echo came back wrong (%zu B)", got);
    // The device's ack for the request may come before or after the echo.
    cJSON *f;
    while (device_ack < (long long)strlen(req) && (f = next_frame(500))) {
        if (channel(f) && strcmp(channel(f), "tcp_ack") == 0) {
            device_ack = (long long)obj(obj(f, "stream"), "seq")->valuedouble;
        }
        cJSON_Delete(f);
    }
    CHECK(device_ack == (long long)strlen(req), "device acked %lld", device_ack);

    // Server eof: the LAN connection closes, and the device says nothing more.
    const int closed = echo->closed;
    server_eof("tun_1");
    CHECK(wait_until(&echo->closed, closed + 1, 2000), "LAN socket not closed after server eof");
    CHECK(wait_no_tunnels(1000), "tunnel still counted");
    f = next_frame(200);
    CHECK(f == NULL, "device sent something after the server's eof: %s", f ? cJSON_PrintUnformatted(f) : "");
    cJSON_Delete(f);
}

// 1 MB up and back through an echo, the server honouring the device's window and acking at once.
static void test_megabyte_echo(server_t *echo)
{
    route(echo);
    CHECK(open_tunnel("tun_mb", "{\"host\":\"192.168.5.33\",\"port\":7}", NULL), "no answer");
    const size_t total = 1024 * 1024;
    unsigned char *src = pattern(total, 1);
    unsigned char *back = malloc(total);
    size_t sent = 0, got = 0, max_frame = 0;
    long long device_acked = 0;
    const int64_t until = now_ms() + 20000;
    while (got < total && now_ms() < until) {
        // Server → device: 32 KB frames (what DeviceRelay sends) while the device's window allows.
        while (sent < total && (long long)sent - device_acked + 32768 <= LAN_TUNNEL_WINDOW) {
            const size_t n = total - sent < 32768 ? total - sent : 32768;
            server_data("tun_mb", src + sent, n);
            sent += n;
        }
        cJSON *f = next_frame(2000);
        if (!f) {
            break;
        }
        if (channel(f) && strcmp(channel(f), "tcp_ack") == 0) {
            const long long seq = (long long)obj(obj(f, "stream"), "seq")->valuedouble;
            CHECK(seq > device_acked && seq <= (long long)sent, "ack %lld after %lld (sent %zu)", seq, device_acked, sent);
            device_acked = seq;
        } else if (channel(f) && strcmp(channel(f), "tcp") == 0 && str(obj(f, "stream"), "data")) {
            const char *d = str(obj(f, "stream"), "data");
            long n = lan_tunnel_b64_decode(d, strlen(d), back + got, total - got);
            CHECK(n > 0 && n <= LAN_TUNNEL_MAX_FRAME, "bad data frame (%ld)", n);
            if (n > 0) {
                if ((size_t)n > max_frame) {
                    max_frame = (size_t)n;
                }
                got += (size_t)n;
                server_ack("tun_mb", (long long)got);
            }
        } else {
            CHECK(false, "unexpected frame %s", cJSON_PrintUnformatted(f));
        }
        cJSON_Delete(f);
    }
    CHECK(got == total && memcmp(src, back, total) == 0, "1 MB echo: %zu of %zu B, %s", got, total,
          got == total ? "bytes differ" : "short");
    CHECK(max_frame == LAN_TUNNEL_MAX_FRAME, "largest device frame %zu", max_frame);
    // The last ack may still be on its way.
    cJSON *f;
    while (device_acked < (long long)total && (f = next_frame(500))) {
        if (channel(f) && strcmp(channel(f), "tcp_ack") == 0) {
            device_acked = (long long)obj(obj(f, "stream"), "seq")->valuedouble;
        }
        cJSON_Delete(f);
    }
    CHECK(device_acked == (long long)total, "device acked %lld of %zu", device_acked, total);
    server_eof("tun_mb");
    CHECK(wait_no_tunnels(2000), "tunnel still open");
    free(src);
    free(back);
}

// The device never has more than one window unacknowledged, and resumes when acks come in.
static void test_window(void)
{
    const size_t total = 1024 * 1024;
    server_t *src = server(SRV_SOURCE);
    src->source = pattern(total, 2);
    src->source_len = total;
    route(src);
    CHECK(open_tunnel("tun_win", "{\"host\":\"192.168.5.40\",\"port\":8554}", NULL), "no answer");
    unsigned char *buf = malloc(total);
    size_t got = collect("tun_win", buf, total, false, 400, NULL, NULL, NULL);
    CHECK(got == LAN_TUNNEL_WINDOW, "without acks the device sent %zu B, window %d", got, LAN_TUNNEL_WINDOW);
    server_ack("tun_win", 64 * 1024);
    size_t more = collect("tun_win", buf + got, total - got, false, 400, NULL, NULL, NULL);
    CHECK(more == 64 * 1024, "after acking 64 KB the device sent %zu B more", more);
    got += more;
    server_ack("tun_win", (long long)got);
    // Now ack as it arrives; the source closes after 1 MB: data, then a plain eof.
    cJSON *eof = NULL;
    size_t rest = 0;
    {
        size_t n;
        do {
            n = collect("tun_win", buf + got + rest, total - got - rest, false, 1000, &eof, NULL, NULL);
            rest += n;
            server_ack("tun_win", (long long)(got + rest));
        } while (n > 0 && !eof && got + rest < total);
    }
    got += rest;
    CHECK(got == total && memcmp(buf, src->source, total) == 0, "pulled %zu of %zu B through the window", got, total);
    if (!eof) {
        eof = next_frame(2000);
    }
    CHECK(is_eof(eof) && !str(eof, "error"), "LAN close: expected a plain eof, got %s", eof ? cJSON_PrintUnformatted(eof) : "nothing");
    cJSON_Delete(eof);
    CHECK(wait_no_tunnels(1000), "tunnel still open");
    free(buf);
}

static void expect_error(const char *what, const char *id, const char *needle, int timeout_ms)
{
    cJSON *f = next_frame(timeout_ms);
    const char *e = str(f, "error");
    CHECK(is_kind(f, "error", id) && e && strstr(e, needle), "%s: expected an error with \"%s\", got %s", what, needle,
          f ? cJSON_PrintUnformatted(f) : "nothing");
    if (f) {
        CHECK(count_keys(f) == 3, "%s: error frame shape", what);
    }
    cJSON_Delete(f);
}

static void expect_eof_error(const char *what, const char *id, const char *needle, int timeout_ms)
{
    cJSON *f;
    // Skip acks/data still in flight.
    while ((f = next_frame(timeout_ms)) && !is_eof(f)) {
        cJSON_Delete(f);
    }
    const char *e = str(f, "error");
    CHECK(f && is_eof(f) && strcmp(str(f, "id"), id) == 0 && e && strstr(e, needle),
          "%s: expected eof with \"%s\", got %s", what, needle, f ? cJSON_PrintUnformatted(f) : "nothing");
    cJSON_Delete(f);
}

static void test_dial_errors(server_t *echo)
{
    // Refused: a port nobody listens on.
    int port;
    int s = listen_on(&port, 1);
    close(s);
    g_route_port = port;
    dial("tun_refused", "{\"host\":\"192.168.5.33\",\"port\":8800}");
    expect_error("refused", "tun_refused", "connect to 192.168.5.33:8800 refused", 3000);

    // Timeout: the SYN is never answered.
    g_route_blackhole = true;
    const int64_t t0 = now_ms();
    dial("tun_slow", "{\"host\":\"192.168.5.33\",\"port\":554,\"timeout_seconds\":1}");
    expect_error("timeout", "tun_slow", "connect to 192.168.5.33:554 timed out after 1 s", 4000);
    const int64_t took = now_ms() - t0;
    CHECK(took >= 950 && took < 1500, "timeout took %lld ms", (long long)took);
    // timeout_seconds over the cap (or missing) means 15 s: check the message only via a short
    // server eof while dialing instead of waiting 15 s.
    dial("tun_slow2", "{\"host\":\"192.168.5.33\",\"port\":554,\"timeout_seconds\":600}");
    usleep(100000);
    server_eof("tun_slow2");
    expect_error("eof while dialing", "tun_slow2", "tunnel closed before the connection opened", 2000);
    g_route_blackhole = false;
    CHECK(wait_no_tunnels(2000), "tunnel left after the timeout");

    // Destination and params: refused before any connect.
    route(echo);
    const int connects = g_connects;
    dial("tun_pub", "{\"host\":\"8.8.8.8\",\"port\":53}");
    expect_error("public", "tun_pub", "public address", 2000);
    dial("tun_lo", "{\"host\":\"127.0.0.1\",\"port\":22}");
    expect_error("loopback", "tun_lo", "loopback", 2000);
    dial("tun_self", "{\"host\":\"" SELF "\",\"port\":22}");
    expect_error("self", "tun_self", "the T-Circle itself", 2000);
    dial("tun_bcast", "{\"host\":\"192.168.5.255\",\"port\":22}");
    expect_error("broadcast", "tun_bcast", "broadcast", 2000);
    dial("tun_nohost", "{\"port\":22}");
    expect_error("no host", "tun_nohost", "'host' is required", 2000);
    dial("tun_noparams", NULL);
    expect_error("no params", "tun_noparams", "'host' is required", 2000);
    dial("tun_port0", "{\"host\":\"192.168.5.33\",\"port\":0}");
    expect_error("port 0", "tun_port0", "'port' must be between 1 and 65535", 2000);
    dial("tun_portbig", "{\"host\":\"192.168.5.33\",\"port\":70000}");
    expect_error("port 70000", "tun_portbig", "'port' must be", 2000);
    dial("tun_portstr", "{\"host\":\"192.168.5.33\",\"port\":\"554\"}");
    expect_error("port string", "tun_portstr", "'port' must be", 2000);
    dial("tun_portfrac", "{\"host\":\"192.168.5.33\",\"port\":55.5}");
    expect_error("port 55.5", "tun_portfrac", "'port' must be", 2000);
    CHECK(g_connects == connects, "%d connect(s) for refused destinations", g_connects - connects);
}

static void test_gates(server_t *echo)
{
    route(echo);
    int connects = g_connects;
    g_call = true;
    dial("tun_call", "{\"host\":\"192.168.5.33\",\"port\":554}");
    expect_error("call at dial", "tun_call", "busy: voice call active", 2000);
    g_call = false;
    g_read_only = true;
    dial("tun_ro", "{\"host\":\"192.168.5.33\",\"port\":554}");
    expect_error("read-only at dial", "tun_ro", "not permitted for a local_file_read connector", 2000);
    g_read_only = false;
    CHECK(g_connects == connects, "connect while refused");

    // A call that starts while a tunnel is open ends it with the same error, and closes the socket.
    CHECK(open_tunnel("tun_c2", "{\"host\":\"192.168.5.33\",\"port\":554}", NULL), "no answer");
    int closed = echo->closed;
    const int64_t t0 = now_ms();
    g_call = true;
    expect_eof_error("call mid-tunnel", "tun_c2", "busy: voice call active", 1000);
    CHECK(now_ms() - t0 < 200, "the call ended the tunnel after %lld ms", (long long)(now_ms() - t0));
    CHECK(wait_until(&echo->closed, closed + 1, 1000), "LAN socket left open during the call");
    g_call = false;

    CHECK(open_tunnel("tun_r2", "{\"host\":\"192.168.5.33\",\"port\":554}", NULL), "no answer");
    g_read_only = true;
    expect_eof_error("read-only mid-tunnel", "tun_r2", "not permitted", 1000);
    g_read_only = false;

    // The Connector socket went away: the tunnel closes without a word (nothing could carry it).
    CHECK(open_tunnel("tun_s", "{\"host\":\"192.168.5.33\",\"port\":554}", NULL), "no answer");
    closed = echo->closed;
    g_session = SESSION + 1;
    CHECK(wait_until(&echo->closed, closed + 1, 1000), "LAN socket left open after the Connector socket closed");
    CHECK(wait_no_tunnels(1000), "tunnel still counted");
    cJSON *f = next_frame(200);
    CHECK(f == NULL, "frame after the socket closed");
    cJSON_Delete(f);
    g_session = SESSION;
    CHECK(g_wrong_session == 0, "%d frame(s) for an old socket", (int)g_wrong_session);
}

static void test_capacity(server_t *echo)
{
    route(echo);
    CHECK(open_tunnel("tun_a", "{\"host\":\"192.168.5.33\",\"port\":554}", NULL), "a");
    CHECK(open_tunnel("tun_b", "{\"host\":\"192.168.5.34\",\"port\":554}", NULL), "b");
    dial("tun_c", "{\"host\":\"192.168.5.35\",\"port\":554}");
    expect_error("third tunnel", "tun_c", "tunnel capacity exhausted", 2000);
    dial("tun_a", "{\"host\":\"192.168.5.33\",\"port\":554}");
    expect_error("same id", "tun_a", "tunnel already exists", 2000);
    // The same id on a different socket is another tunnel, but capacity still counts.
    server_eof("tun_a");
    server_eof("tun_b");
    CHECK(wait_no_tunnels(2000), "tunnels left");
    // Frames for unknown tunnels are taken and ignored.
    server_ack("tun_unknown", 5);
    server_data("tun_unknown", (const unsigned char *)"x", 1);
    cJSON *f = next_frame(200);
    CHECK(f == NULL, "answer to a frame for an unknown tunnel");
    cJSON_Delete(f);
    // Not a tunnel frame at all.
    cJSON *m = cJSON_Parse("{\"id\":\"r\",\"type\":\"stream\",\"stream\":{\"channel\":\"stdout\",\"data\":\"eA==\"}}");
    CHECK(!lan_tunnel_frame("r", m, g_session), "a stdout stream taken as a tunnel frame");
    cJSON_Delete(m);
}

static void test_bad_frames(server_t *echo)
{
    route(echo);
    CHECK(open_tunnel("tun_bad", "{\"host\":\"192.168.5.33\",\"port\":554}", NULL), "no answer");
    server_frame("{\"id\":\"tun_bad\",\"type\":\"stream\",\"stream\":{\"channel\":\"tcp\",\"data\":\"not base64!\"}}");
    expect_eof_error("bad base64", "tun_bad", "invalid tunnel frame", 1000);

    CHECK(open_tunnel("tun_big", "{\"host\":\"192.168.5.33\",\"port\":554}", NULL), "no answer");
    unsigned char *big = pattern(LAN_TUNNEL_SERVER_FRAME_MAX + 3, 3);
    server_data("tun_big", big, LAN_TUNNEL_SERVER_FRAME_MAX + 3);
    expect_eof_error("frame over 32 KB", "tun_big", "invalid tunnel frame", 1000);
    free(big);

    // Exactly 32 KB is fine (the server's own frame size).
    CHECK(open_tunnel("tun_32k", "{\"host\":\"192.168.5.33\",\"port\":554}", NULL), "no answer");
    unsigned char *ok = pattern(LAN_TUNNEL_SERVER_FRAME_MAX, 4);
    server_data("tun_32k", ok, LAN_TUNNEL_SERVER_FRAME_MAX);
    unsigned char *back = malloc(LAN_TUNNEL_SERVER_FRAME_MAX);
    size_t got = collect("tun_32k", back, LAN_TUNNEL_SERVER_FRAME_MAX, true, 2000, NULL, NULL, NULL);
    CHECK(got == LAN_TUNNEL_SERVER_FRAME_MAX && memcmp(ok, back, got) == 0, "32 KB frame echoed %zu B", got);
    server_eof("tun_32k");
    free(ok);
    free(back);
    CHECK(wait_no_tunnels(2000), "tunnels left");
    drop_frames();
}

// A LAN host that doesn't read: server data queues on the device, up to one window and no more.
static void test_window_exceeded(void)
{
    server_t *stall = server(SRV_STALL);
    route(stall);
    g_route_sndbuf = 4096;
    CHECK(open_tunnel("tun_full", "{\"host\":\"192.168.5.33\",\"port\":554}", NULL), "no answer");
    g_route_sndbuf = 0;
    unsigned char *chunk = pattern(32768, 5);
    // A server that ignores the window: 5 x 32 KB with no ack back.
    for (int i = 0; i < 5; ++i) {
        server_data("tun_full", chunk, 32768);
    }
    expect_eof_error("window exceeded", "tun_full", "tunnel window exceeded", 2000);
    CHECK(wait_until(&stall->closed, 1, 2000), "LAN socket left open");
    CHECK(wait_no_tunnels(1000), "tunnel left");
    free(chunk);
    drop_frames();
}

static void test_lan_reset(void)
{
    server_t *rst = server(SRV_RESET);
    route(rst);
    CHECK(open_tunnel("tun_rst", "{\"host\":\"192.168.5.33\",\"port\":554}", NULL), "no answer");
    server_data("tun_rst", (const unsigned char *)"hello", 5);
    expect_eof_error("LAN reset", "tun_rst", "reset the connection", 2000);
    CHECK(wait_no_tunnels(1000), "tunnel left");
}

static void test_idle(server_t *echo)
{
    route(echo);
    CHECK(open_tunnel("tun_idle", "{\"host\":\"192.168.5.33\",\"port\":554}", NULL), "no answer");
    const int64_t t0 = now_ms();
    char want[32];
    snprintf(want, sizeof(want), "tunnel idle for %d s", LAN_TUNNEL_IDLE_S);
    expect_eof_error("idle", "tun_idle", want, LAN_TUNNEL_IDLE_S * 1000 + 1000);
    const int64_t took = now_ms() - t0;
    CHECK(took >= LAN_TUNNEL_IDLE_S * 1000 - 50 && took < LAN_TUNNEL_IDLE_S * 1000 + 500, "idle after %lld ms",
          (long long)took);
}

// Server eof with bytes still queued for the LAN host: they go out first, then the close.
static void test_drain_on_eof(server_t *echo)
{
    route(echo);
    CHECK(open_tunnel("tun_drain", "{\"host\":\"192.168.5.33\",\"port\":554}", NULL), "no answer");
    const long before = echo->received;
    const int closed = echo->closed;
    unsigned char *p = pattern(100000, 6);
    for (size_t off = 0; off < 100000; off += 25000) {
        server_data("tun_drain", p + off, 25000);
    }
    server_eof("tun_drain");
    CHECK(wait_until(&echo->closed, closed + 1, 3000), "LAN socket not closed");
    CHECK(echo->received - before == 100000, "the LAN host got %ld of 100000 B before the close",
          echo->received - before);
    free(p);
    drop_frames();
}

static void check_survivor(const char *id)
{
    const unsigned char data[] = "still usable after another command failed";
    unsigned char back[sizeof(data)] = {0};
    server_data(id, data, sizeof(data));
    long long ack = 0;
    size_t n = collect(id, back, sizeof(back), true, 1000, NULL, NULL, &ack);
    CHECK(n == sizeof(data) && memcmp(back, data, sizeof(data)) == 0, "%s did not survive", id);
    CHECK(ack == sizeof(data), "%s did not acknowledge server data", id);
}

static bool pause_consumer(void)
{
    g_pause_consumer = true;
    int64_t until = now_ms() + 1000;
    while (!g_consumer_paused && now_ms() < until) usleep(1000);
    CHECK(g_consumer_paused, "consumer did not pause");
    if (!g_consumer_paused) g_pause_consumer = false;
    return g_consumer_paused;
}

static void test_out_of_memory(server_t *echo)
{
    // A decode allocation, command allocation, and command queue send can each fail.
    for (int failure = 0; failure < 4; ++failure) {
        route(echo);
        CHECK(open_tunnel("tun_oom", "{\"host\":\"192.168.5.33\",\"port\":7}", NULL), "no answer");
        CHECK(open_tunnel("tun_survivor", "{\"host\":\"192.168.5.33\",\"port\":7}", NULL), "no answer");
        const size_t n = 32768;
        unsigned char *data = pattern(n, 42);
        if (failure == 3 && pause_consumer()) {
            // Actually fill the command queue, in addition to the injected send failure.
            for (int i = 0; i < QUEUE_LEN; ++i) server_ack("tun_survivor", 0);
        } else if (failure == 2) g_fail_command_send = true;
        else g_fail_alloc_size = failure ? sizeof(cmd_t) : sizeof(block_t) + B64_LEN(n) / 4 * 3;
        server_data("tun_oom", data, n);
        g_pause_consumer = false;
        free(data);
        cJSON *f = next_frame(2000);
        CHECK(is_kind(f, "stream", "tun_oom") && is_eof(f) && str(f, "error"), "only failed tunnel should end");
        cJSON_Delete(f);
        CHECK(g_fail_alloc_size == 0, "allocation fault did not fire");
        CHECK(!g_fail_command_send, "queue fault did not fire");
        check_survivor("tun_survivor");
        CHECK(lan_tunnel_count() == 1, "unrelated tunnel closed or failed tunnel leaked");
        server_eof("tun_survivor");
        CHECK(wait_no_tunnels(2000), "survivor leaked a tunnel");
        drop_frames();
    }
    test_connect_answer_and_echo(echo);
}

static atomic_bool g_failure_posted;

static void *post_another_failure(void *arg)
{
    (void)arg;
    command_failed("unknown_after_full", g_session);
    g_failure_posted = true;
    return NULL;
}

static void test_failure_backlog(server_t *echo)
{
    route(echo);
    CHECK(open_tunnel("tun_backlog", "{\"host\":\"192.168.5.33\",\"port\":7}", NULL), "no answer");
    CHECK(open_tunnel("tun_survivor", "{\"host\":\"192.168.5.33\",\"port\":7}", NULL), "no answer");
    if (!pause_consumer()) return;
    command_failed("tun_backlog", g_session);
    command_failed("unknown_before_full", g_session);
    g_failure_queue_waiting = false;
    g_failure_posted = false;
    pthread_t producer;
    int err = pthread_create(&producer, NULL, post_another_failure, NULL);
    CHECK(err == 0, "producer thread creation failed");
    if (err == 0) {
        int64_t until = now_ms() + 1000;
        while (!g_failure_queue_waiting && now_ms() < until) usleep(1000);
        CHECK(g_failure_queue_waiting && !g_failure_posted, "full failure queue did not apply back-pressure");
    }
    g_pause_consumer = false;
    if (err == 0) {
        CHECK(pthread_join(producer, NULL) == 0, "producer thread did not join");
        CHECK(g_failure_posted, "failure identity was not handed over");
    }
    cJSON *f = next_frame(1000);
    CHECK(is_kind(f, "stream", "tun_backlog") && is_eof(f) && str(f, "error"), "pending identity was overwritten");
    cJSON_Delete(f);
    check_survivor("tun_survivor");
    server_eof("tun_survivor");
    CHECK(wait_no_tunnels(1000), "backlog test leaked tunnels");
    drop_frames();
}

static void test_failure_identity(server_t *echo)
{
    route(echo);
    CHECK(open_tunnel("tun_same_id", "{\"host\":\"192.168.5.33\",\"port\":7}", NULL), "no answer");
    CHECK(open_tunnel("tun_other", "{\"host\":\"192.168.5.33\",\"port\":7}", NULL), "no answer");
    cJSON *ack = cJSON_Parse("{\"stream\":{\"channel\":\"tcp_ack\",\"seq\":0}}");
    g_fail_alloc_size = sizeof(cmd_t);
    CHECK(lan_tunnel_frame("tun_same_id", ack, g_session - 1), "old session frame not taken");
    CHECK(g_fail_alloc_size == 0, "old session fault did not fire");
    g_fail_alloc_size = sizeof(cmd_t);
    CHECK(lan_tunnel_frame("unknown_id", ack, g_session), "unknown frame not taken");
    CHECK(g_fail_alloc_size == 0, "unknown id fault did not fire");
    // A refused new dial is another identified command; it cannot kill existing tunnels.
    g_fail_command_send = true;
    CHECK(!lan_tunnel_dial("refused_dial", NULL, g_session), "dial queue fault did not refuse");
    cJSON_Delete(ack);
    check_survivor("tun_same_id");
    check_survivor("tun_other");
    CHECK(lan_tunnel_count() == 2, "stale/unknown command ended an unrelated tunnel");
    // Only genuinely missing identity retains the conservative all-tunnel fallback.
    command_failed(NULL, g_session);
    bool seen_same = false, seen_other = false;
    for (int i = 0; i < 2; ++i) {
        cJSON *f = next_frame(1000);
        CHECK(is_eof(f) && str(f, "error"), "unidentified failure must end both tunnels");
        seen_same |= is_kind(f, "stream", "tun_same_id");
        seen_other |= is_kind(f, "stream", "tun_other");
        cJSON_Delete(f);
    }
    CHECK(seen_same && seen_other, "fallback lost a tunnel identity");
    CHECK(wait_no_tunnels(1000), "fallback leaked tunnels");
    drop_frames();
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    const lan_tunnel_host_t host = {.send = host_send, .check = host_check};
    g_fail_alloc_size = FRAME_CAP;
    CHECK(!lan_tunnel_start(&host), "startup allocation fault did not refuse");
    CHECK(!s_cmds && !s_command_failures && !s_raw && !s_frame, "startup allocation fault leaked resources");
    if (!lan_tunnel_start(&host)) {
        fprintf(stderr, "lan_tunnel_start failed\n");
        return 1;
    }
    server_t *echo = server(SRV_ECHO);
    test_base64();
    test_connect_answer_and_echo(echo);
    test_megabyte_echo(echo);
    test_window();
    test_dial_errors(echo);
    test_gates(echo);
    test_capacity(echo);
    test_bad_frames(echo);
    test_window_exceeded();
    test_lan_reset();
    test_drain_on_eof(echo);
    test_idle(echo);
    test_out_of_memory(echo);
    test_failure_identity(echo);
    test_failure_backlog(echo);
    printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
