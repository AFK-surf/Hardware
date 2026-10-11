#include "heap_diag.h"
#include "memory_caps.h"
// lan_tcp_dial (G3): TCP tunnels from the T-Circle to LAN hosts, relayed over the Connector socket.
// Protocol and limits in lan_tunnel.h.
//
// One task runs every tunnel: a select() loop over the LAN sockets, plus a queue of commands from the
// Connector task (dial, server data, acks, eof). The Connector task only parses and base64-decodes
// server frames; all tunnel state lives here. Every frame to the server (answer, data, ack, eof) goes
// out from this task in order, through the host's send(), so the answer always precedes the data.
//
// Back-pressure:
//   LAN → server: a LAN socket is read only while the window has room. When the server is slow the
//                 socket stays unread, and its small receive window (LAN_TUNNEL_SOCKET_WINDOW) makes
//                 TCP slow the LAN host down with few Wi-Fi RX buffers held.
//   server → LAN: data waits here (PSRAM, at most one window) until the LAN socket takes it; acks
//                 go out as it is written.

#include "lan_tunnel.h"

#include "lan_dest.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#ifdef ESP_PLATFORM
#include "lwip/api.h"
#include "lwip/priv/sockets_priv.h"
#include "lwip/priv/tcpip_priv.h"
#include "lwip/tcp.h"
#endif

#define ID_MAX 64
#define HOST_MAX 253
#define ERR_MAX LAN_DEST_ERR_MAX
// While a tunnel is open the loop wakes this often to pick up commands (latency server → LAN).
#define POLL_MS 20
#define QUEUE_LEN 32
#define TASK_STACK 6144
#define TASK_PRIO 2
// Frames read from one LAN socket per pass, so one busy tunnel can't starve the other.
#define READS_PER_PASS 4
// After the server's eof: how long queued bytes may take to reach the LAN host before the close.
#define DRAIN_MS 2000
#ifndef COMMA_TUNNEL_DEBUG
#define COMMA_TUNNEL_DEBUG 0
#endif
#define B64_LEN(n) (((n) + 2) / 3 * 4)
// The id as a JSON string (escaped) fits this.
#define ID_JSON_MAX (ID_MAX * 6 + 3)
// One data frame: envelope, id and base64 text.
#define FRAME_CAP (B64_LEN(LAN_TUNNEL_MAX_FRAME) + ID_JSON_MAX + 128)

static const char *TAG = "lan_tunnel";

// ---- Commands from the Connector task ------------------------------------------------------------

typedef struct block {
    struct block *next;
    size_t len;
    size_t off;
    unsigned char data[];
} block_t;

typedef enum {
    CMD_DIAL,
    CMD_DATA,
    CMD_ACK,
    CMD_EOF,
    CMD_OOM, // stop this tunnel instead of dropping a data frame and stalling
    CMD_BAD,  // a data frame that is not valid base64 or is too large
} cmd_kind_t;

typedef struct {
    cmd_kind_t kind;
    unsigned session;
    char id[ID_MAX + 1];
    // CMD_DIAL
    char host[HOST_MAX + 1];
    bool host_ok;  // a non-empty string that fits
    double port;   // NAN-free: -1 when missing or not a number
    double timeout_s;
    // CMD_ACK
    int64_t seq;
    // CMD_DATA: decoded bytes, handed over to the tunnel's queue
    block_t *block;
} cmd_t;

// ---- Tunnels -------------------------------------------------------------------------------------

typedef enum {
    T_FREE = 0,
    T_DIALING,
    T_OPEN,
    T_DRAINING,  // the server sent eof: write what is queued, then close without a word
} tstate_t;

typedef struct {
    tstate_t state;
    unsigned session;
    char id[ID_MAX + 1];
    char id_json[ID_JSON_MAX];
    int fd;
    char remote[24];  // ip:port
    int dial_timeout_s;
    int64_t deadline_us;  // dial deadline, or drain deadline
    // LAN → server
    int64_t sent;
    int64_t acked;
    // server → LAN
    block_t *head;
    block_t *tail;
    size_t queued;
    int64_t written;
    int64_t ack_sent;
    // housekeeping
    int64_t opened_us;
    int64_t active_us;  // last data either way
    int wnd_cut;        // how much the socket's receive window was cut (restored before close)
    unsigned frames_up;
    unsigned frames_down;
    int64_t send_max_us;
    size_t internal_min;
} tunnel_t;

static lan_tunnel_host_t s_host;
static QueueHandle_t s_cmds;
static tunnel_t s_t[LAN_TUNNEL_MAX_OPEN];
static unsigned char *s_raw;  // one LAN read, PSRAM
static char *s_frame;         // one data frame, PSRAM
static volatile int s_count;
static atomic_bool s_command_failure;

static void *psram_alloc(size_t n)
{
    return heap_caps_malloc(n, COMMA_BUFFER_CAPS);
}

// ---- base64 --------------------------------------------------------------------------------------

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t lan_tunnel_b64_encode(const unsigned char *in, size_t n, char *out)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = ((uint32_t)in[i] << 16) | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = i + 1 < n ? B64[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? B64[v & 63] : '=';
    }
    return o;
}

static int b64_value(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    return c == '+' ? 62 : c == '/' ? 63 : -1;
}

long lan_tunnel_b64_decode(const char *in, size_t n, unsigned char *out, size_t cap)
{
    if (n % 4 != 0) {
        return -1;
    }
    size_t o = 0;
    for (size_t i = 0; i < n; i += 4) {
        const bool last = i + 4 == n;
        const int pad = (in[i + 3] == '=') + (in[i + 2] == '=');
        if (pad > 0 && (!last || (in[i + 2] == '=' && in[i + 3] != '='))) {
            return -1;
        }
        int v[4];
        for (int k = 0; k < 4 - pad; ++k) {
            v[k] = b64_value(in[i + k]);
            if (v[k] < 0) {
                return -1;
            }
        }
        for (int k = 4 - pad; k < 4; ++k) {
            v[k] = 0;
        }
        const uint32_t w = (uint32_t)v[0] << 18 | (uint32_t)v[1] << 12 | (uint32_t)v[2] << 6 | (uint32_t)v[3];
        // Canonical only: the bits a padded group drops must be zero (like Go's Strict decoding).
        if ((pad == 1 && (w & 0xFF)) || (pad == 2 && (w & 0xFFFF))) {
            return -1;
        }
        const size_t take = 3 - (size_t)pad;
        if (o + take > cap) {
            return -1;
        }
        out[o++] = (unsigned char)(w >> 16);
        if (take > 1) {
            out[o++] = (unsigned char)(w >> 8);
        }
        if (take > 2) {
            out[o++] = (unsigned char)w;
        }
    }
    return (long)o;
}

// ---- Frames to the server ------------------------------------------------------------------------

static bool send_text(const tunnel_t *t, const char *frame, size_t len)
{
    return s_host.send(s_host.ctx, t->session, frame, len);
}

static bool send_json_obj(unsigned session, cJSON *obj)
{
    char *json = obj ? cJSON_PrintUnformatted(obj) : NULL;
    cJSON_Delete(obj);
    if (!json) {
        ESP_LOGW(TAG, "out of memory building a frame");
        return false;
    }
    const bool ok = s_host.send(s_host.ctx, session, json, strlen(json));
    cJSON_free(json);
    return ok;
}

static void send_error_answer(unsigned session, const char *id, const char *message)
{
    cJSON *obj = cJSON_CreateObject();
    if (obj) {
        cJSON_AddStringToObject(obj, "id", id);
        cJSON_AddStringToObject(obj, "type", "error");
        cJSON_AddStringToObject(obj, "error", message);
    }
    send_json_obj(session, obj);
}

static bool send_connected(tunnel_t *t)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON *result = cJSON_CreateObject();
    if (obj && result) {
        cJSON_AddStringToObject(obj, "id", t->id);
        cJSON_AddStringToObject(obj, "type", "response");
        cJSON_AddStringToObject(result, "status", "connected");
        cJSON_AddStringToObject(result, "remote_address", t->remote);
        cJSON_AddNumberToObject(result, "window", LAN_TUNNEL_WINDOW);
        cJSON_AddNumberToObject(result, "max_frame", LAN_TUNNEL_MAX_FRAME);
        cJSON_AddItemToObject(obj, "result", result);
        result = NULL;
    }
    cJSON_Delete(result);
    return send_json_obj(t->session, obj);
}

static bool send_eof(tunnel_t *t, const char *reason)
{
    cJSON *obj = cJSON_CreateObject();
    cJSON *stream = cJSON_CreateObject();
    if (obj && stream) {
        cJSON_AddStringToObject(obj, "id", t->id);
        cJSON_AddStringToObject(obj, "type", "stream");
        if (reason && reason[0]) {
            cJSON_AddStringToObject(obj, "error", reason);
        }
        cJSON_AddStringToObject(stream, "channel", "tcp");
        cJSON_AddBoolToObject(stream, "eof", true);
        cJSON_AddItemToObject(obj, "stream", stream);
        stream = NULL;
    }
    cJSON_Delete(stream);
    return send_json_obj(t->session, obj);
}

static bool send_ack(tunnel_t *t)
{
    char frame[ID_JSON_MAX + 96];
    const int n = snprintf(frame, sizeof(frame), "{\"id\":%s,\"type\":\"stream\",\"stream\":{\"channel\":\"tcp_ack\",\"seq\":%lld}}",
                           t->id_json, (long long)t->written);
    return n > 0 && (size_t)n < sizeof(frame) && send_text(t, frame, (size_t)n);
}

static bool send_data(tunnel_t *t, const unsigned char *raw, size_t n)
{
    size_t len = (size_t)snprintf(s_frame, FRAME_CAP, "{\"id\":%s,\"type\":\"stream\",\"stream\":{\"channel\":\"tcp\",\"data\":\"",
                                  t->id_json);
    len += lan_tunnel_b64_encode(raw, n, s_frame + len);
    memcpy(s_frame + len, "\"}}", 4);
    len += 3;
    const int64_t start = esp_timer_get_time();
    const bool ok = send_text(t, s_frame, len);
    const int64_t took = esp_timer_get_time() - start;
    if (took > t->send_max_us) {
        t->send_max_us = took;
    }
    return ok;
}

// ---- LAN socket ----------------------------------------------------------------------------------

#ifdef ESP_PLATFORM
// lwIP has no per-socket receive window (SO_RCVBUF is UDP/raw only). Right after the connect, before
// the LAN host has sent anything, cut this pcb's window in the tcpip thread: the next segment the
// device sends (the first request) already advertises the small window. tcp_recved() only ever
// gives back what the application read, so the cut stays. Restored before close, so a fully read
// socket still closes with FIN rather than RST.
typedef struct {
    struct tcpip_api_call_data call;
    struct netconn *conn;
    int target;  // shrink to this, or <= 0: restore by `cut`
    int cut;
} wnd_call_t;

static err_t wnd_apply(struct tcpip_api_call_data *data)
{
    wnd_call_t *w = (wnd_call_t *)data;
    struct tcp_pcb *pcb = w->conn ? w->conn->pcb.tcp : NULL;
    if (!pcb || pcb->state == LISTEN || pcb->state == CLOSED) {
        return ERR_CONN;
    }
    if (w->target > 0) {
        w->cut = pcb->rcv_wnd > (tcpwnd_size_t)w->target ? (int)(pcb->rcv_wnd - (tcpwnd_size_t)w->target) : 0;
        pcb->rcv_wnd -= (tcpwnd_size_t)w->cut;
        pcb->rcv_ann_wnd = pcb->rcv_wnd;
        pcb->rcv_ann_right_edge = pcb->rcv_nxt + pcb->rcv_wnd;
    } else {
        const uint32_t wnd = (uint32_t)pcb->rcv_wnd + (uint32_t)w->cut;
        pcb->rcv_wnd = wnd > TCP_WND_MAX(pcb) ? TCP_WND_MAX(pcb) : (tcpwnd_size_t)wnd;
    }
    return ERR_OK;
}

#if COMMA_TUNNEL_DEBUG
// Dev builds only (-DCOMMA_TUNNEL_DEBUG=1): a snapshot of the pcb, read in the tcpip thread.
typedef struct {
    struct tcpip_api_call_data call;
    struct netconn *conn;
    int state, rcv_wnd, rcv_ann_wnd, ann_edge, snd_wnd, persist_backoff, unsent, unacked, refused;
} pcb_snap_t;

static err_t pcb_snap(struct tcpip_api_call_data *data)
{
    pcb_snap_t *p = (pcb_snap_t *)data;
    struct tcp_pcb *pcb = p->conn ? p->conn->pcb.tcp : NULL;
    if (!pcb) {
        return ERR_CONN;
    }
    p->state = pcb->state;
    p->rcv_wnd = pcb->rcv_wnd;
    p->rcv_ann_wnd = pcb->rcv_ann_wnd;
    p->ann_edge = (int)(pcb->rcv_ann_right_edge - pcb->rcv_nxt);
    p->snd_wnd = pcb->snd_wnd;
    p->persist_backoff = pcb->persist_backoff;
    p->unsent = pcb->unsent != NULL;
    p->unacked = pcb->unacked != NULL;
    p->refused = pcb->refused_data != NULL;
    return ERR_OK;
}

static void debug_log(const tunnel_t *t)
{
    struct lwip_sock *sock = lwip_socket_dbg_get_socket(t->fd);
    pcb_snap_t p = {.conn = sock ? sock->conn : NULL};
    int unread = -1;
    ioctl(t->fd, FIONREAD, &unread);
    if (!p.conn || tcpip_api_call(pcb_snap, &p.call) != ERR_OK) {
        ESP_LOGI(TAG, "dbg %s: no pcb", t->id);
        return;
    }
    ESP_LOGI(TAG, "dbg %s st=%d up sent=%lld acked=%lld | down queued=%u written=%lld | unread=%d rcv_wnd=%d ann=%d "
                  "edge=%d snd_wnd=%d persist=%d unsent=%d unacked=%d refused=%d",
             t->id, t->state, (long long)t->sent, (long long)t->acked, (unsigned)t->queued, (long long)t->written, unread,
             p.rcv_wnd, p.rcv_ann_wnd, p.ann_edge, p.snd_wnd, p.persist_backoff, p.unsent, p.unacked, p.refused);
}
#endif

static int window_set(int fd, int target, int cut)
{
    struct lwip_sock *sock = lwip_socket_dbg_get_socket(fd);
    wnd_call_t w = {.conn = sock ? sock->conn : NULL, .target = target, .cut = cut};
    if (!w.conn || tcpip_api_call(wnd_apply, &w.call) != ERR_OK) {
        return 0;
    }
    return w.cut;
}
#else
static int window_set(int fd, int target, int cut)
{
    (void)fd;
    (void)target;
    (void)cut;
    return 0;
}
#endif

static void dial_failed_message(const tunnel_t *t, int e, char *err)
{
    if (e == ECONNREFUSED || e == ECONNRESET || e == ECONNABORTED) {
        // lwIP reports a refused connect (RST to the SYN) as ECONNRESET.
        snprintf(err, ERR_MAX, "connect to %s refused: nothing is listening on that port", t->remote);
    } else if (e == EHOSTUNREACH || e == ENETUNREACH) {
        snprintf(err, ERR_MAX, "connect to %s failed: host unreachable", t->remote);
    } else {
        snprintf(err, ERR_MAX, "connect to %s failed: %s", t->remote, e ? strerror(e) : "unknown error");
    }
}

// ---- Tunnel lifecycle ----------------------------------------------------------------------------

static tunnel_t *find(const char *id, unsigned session)
{
    for (int i = 0; i < LAN_TUNNEL_MAX_OPEN; ++i) {
        if (s_t[i].state != T_FREE && s_t[i].session == session && strcmp(s_t[i].id, id) == 0) {
            return &s_t[i];
        }
    }
    return NULL;
}

static void free_blocks(tunnel_t *t)
{
    while (t->head) {
        block_t *next = t->head->next;
        free(t->head);
        t->head = next;
    }
    t->tail = NULL;
    t->queued = 0;
}

// Ends a tunnel once. While dialing, `reason` goes back as the error answer (when there is one);
// once open, `notify` sends eof (with `reason` as its error when non-empty).
static void finish(tunnel_t *t, const char *reason, bool notify)
{
    if (t->state == T_FREE) {
        return;
    }
    const bool dialing = t->state == T_DIALING;
    if (notify) {
        if (dialing) {
            send_error_answer(t->session, t->id, reason && reason[0] ? reason : "tunnel closed before the connection opened");
        } else {
            send_eof(t, reason);
        }
    }
    if (t->fd >= 0) {
        if (t->wnd_cut > 0) {
            window_set(t->fd, 0, t->wnd_cut);
        }
        close(t->fd);
    }
    if (!dialing) {
        ESP_LOGI(TAG, "%s %s closed%s%s: up %lld B in %u frames, down %lld B in %u frames, %lld ms, send_max_ms=%lld, "
                      "internal min %u",
                 t->id, t->remote, reason && reason[0] ? ": " : "", reason ? reason : "", (long long)t->sent,
                 t->frames_up, (long long)t->written, t->frames_down,
                 (long long)((esp_timer_get_time() - t->opened_us) / 1000), (long long)(t->send_max_us / 1000),
                 (unsigned)t->internal_min);
    } else if (reason && reason[0]) {
        ESP_LOGW(TAG, "%s %s: %s", t->id, t->remote, reason);
    }
    free_blocks(t);
    memset(t, 0, sizeof(*t));
    t->fd = -1;
    t->state = T_FREE;
    s_count--;
}

static void note_ram(tunnel_t *t)
{
    const size_t internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    if (internal < t->internal_min) {
        t->internal_min = internal;
    }
}

static void start_dial(const cmd_t *c)
{
    // Checked again here: a call may have started, or access changed, since the Connector task
    // passed the request on.
    const char *reason = NULL;
    const lan_tunnel_verdict_t verdict = s_host.check ? s_host.check(s_host.ctx, c->session, &reason) : LAN_TUNNEL_GO;
    if (verdict != LAN_TUNNEL_GO) {
        if (verdict == LAN_TUNNEL_END) {
            ESP_LOGW(TAG, "%s refused: %s", c->id, reason ? reason : "");
            send_error_answer(c->session, c->id, reason ? reason : "lan_tcp_dial is not available right now");
        }
        return;
    }
    if (!c->host_ok) {
        send_error_answer(c->session, c->id, "'host' is required");
        return;
    }
    if (!(c->port >= 1 && c->port <= 65535) || c->port != (double)(int)c->port) {
        send_error_answer(c->session, c->id, "'port' must be between 1 and 65535");
        return;
    }
    if (find(c->id, c->session)) {
        send_error_answer(c->session, c->id, "tunnel already exists");
        return;
    }
    tunnel_t *t = NULL;
    for (int i = 0; i < LAN_TUNNEL_MAX_OPEN && !t; ++i) {
        if (s_t[i].state == T_FREE) {
            t = &s_t[i];
        }
    }
    if (!t) {
        send_error_answer(c->session, c->id, "tunnel capacity exhausted");
        return;
    }
    char err[ERR_MAX] = "";
    uint32_t ip = 0;
    if (!lan_dest_resolve(c->host, &ip, err) || !lan_dest_check(ip, err)) {
        ESP_LOGW(TAG, "%s refused: %s", c->id, err);
        send_error_answer(c->session, c->id, err);
        return;
    }
    const int port = (int)c->port;
    int timeout_s = (int)c->timeout_s;
    if (timeout_s <= 0 || timeout_s > LAN_TUNNEL_DIAL_TIMEOUT_MAX_S) {
        timeout_s = LAN_TUNNEL_DIAL_TIMEOUT_MAX_S;
    }

    memset(t, 0, sizeof(*t));
    t->fd = -1;
    t->session = c->session;
    strlcpy(t->id, c->id, sizeof(t->id));
    cJSON *id_str = cJSON_CreateString(c->id);
    char *id_json = id_str ? cJSON_PrintUnformatted(id_str) : NULL;
    cJSON_Delete(id_str);
    if (!id_json) {
        send_error_answer(c->session, c->id, "out of memory");
        return;
    }
    strlcpy(t->id_json, id_json, sizeof(t->id_json));
    cJSON_free(id_json);
    char ip_text[16];
    inet_ntop(AF_INET, &ip, ip_text, sizeof(ip_text));
    snprintf(t->remote, sizeof(t->remote), "%s:%d", ip_text, port);
    t->dial_timeout_s = timeout_s;
    t->internal_min = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    t->state = T_DIALING;
    s_count++;

    t->fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (t->fd < 0) {
        finish(t, "no free socket on the T-Circle", true);
        return;
    }
    const int flags = fcntl(t->fd, F_GETFL, 0);
    fcntl(t->fd, F_SETFL, flags | O_NONBLOCK);
    const int one = 1;
    setsockopt(t->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    to.sin_addr.s_addr = ip;
    ESP_LOGI(TAG, "%s dial %s (timeout %d s)", t->id, t->remote, timeout_s);
    t->deadline_us = esp_timer_get_time() + (int64_t)timeout_s * 1000000;
    if (connect(t->fd, (struct sockaddr *)&to, sizeof(to)) != 0 && errno != EINPROGRESS) {
        dial_failed_message(t, errno, err);
        finish(t, err, true);
    }
    // Otherwise the loop sees the socket writable once the connect is done.
}

static void dial_done(tunnel_t *t)
{
    int e = 0;
    socklen_t len = sizeof(e);
    if (getsockopt(t->fd, SOL_SOCKET, SO_ERROR, &e, &len) != 0) {
        e = errno;
    }
    if (e != 0) {
        char err[ERR_MAX];
        dial_failed_message(t, e, err);
        finish(t, err, true);
        return;
    }
    t->wnd_cut = window_set(t->fd, LAN_TUNNEL_SOCKET_WINDOW, 0);
    if (!send_connected(t)) {
        finish(t, "", false);
        return;
    }
    t->state = T_OPEN;
    t->opened_us = t->active_us = esp_timer_get_time();
    ESP_LOGI(TAG, "%s connected to %s (receive window cut by %d)", t->id, t->remote, t->wnd_cut);
}

// ---- Commands ------------------------------------------------------------------------------------

static void free_cmd(cmd_t *c)
{
    free(c->block);
    free(c);
}

static void run_cmd(cmd_t *c)
{
    if (c->kind == CMD_DIAL) {
        start_dial(c);
        free_cmd(c);
        return;
    }
    tunnel_t *t = find(c->id, c->session);
    if (!t) {
        free_cmd(c);
        return;  // unknown or already closed: like salix-connect, ignore
    }
    if (c->kind == CMD_ACK) {
        if (c->seq > t->acked && c->seq <= t->sent) {
            t->acked = c->seq;
        }
    } else if (c->kind == CMD_EOF) {
        if (t->state == T_OPEN && t->head) {
            t->state = T_DRAINING;
            t->deadline_us = esp_timer_get_time() + DRAIN_MS * 1000LL;
        } else if (t->state == T_DIALING) {
            finish(t, "tunnel closed before the connection opened", true);
        } else {
            finish(t, "", false);
        }
    } else if (c->kind == CMD_OOM) {
        finish(t, "out of memory for a tunnel frame", true);
    } else if (c->kind == CMD_BAD) {
        finish(t, "invalid tunnel frame", true);
    } else if (c->kind == CMD_DATA && t->state != T_DRAINING) {
        if (t->state == T_DIALING) {
            finish(t, "data before the connection opened", true);
        } else if (t->queued + c->block->len > LAN_TUNNEL_WINDOW) {
            finish(t, "tunnel window exceeded", true);
        } else {
            block_t *b = c->block;
            c->block = NULL;
            if (t->tail) {
                t->tail->next = b;
            } else {
                t->head = b;
            }
            t->tail = b;
            t->queued += b->len;
            t->frames_down++;
        }
    }
    free_cmd(c);
}

static void drain_cmds(TickType_t wait)
{
    cmd_t *c;
    while (xQueueReceive(s_cmds, &c, wait) == pdTRUE) {
        run_cmd(c);
        wait = 0;
    }
}

// ---- The loop ------------------------------------------------------------------------------------

// Writes queued server bytes to the LAN socket; one ack for whatever went out.
static void pump_out(tunnel_t *t)
{
    while (t->head) {
        block_t *b = t->head;
        const ssize_t n = send(t->fd, b->data + b->off, b->len - b->off, MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            char err[ERR_MAX];
            snprintf(err, sizeof(err), "writing to %s failed: %s", t->remote, strerror(errno));
            finish(t, err, t->state == T_OPEN);
            return;
        }
        b->off += (size_t)n;
        t->written += n;
        t->queued -= (size_t)n;
        t->active_us = esp_timer_get_time();
        if (b->off == b->len) {
            t->head = b->next;
            if (!t->head) {
                t->tail = NULL;
            }
            free(b);
        }
    }
    if (t->written > t->ack_sent && t->state == T_OPEN) {
        if (!send_ack(t)) {
            finish(t, "", false);
            return;
        }
        t->ack_sent = t->written;
    }
}

// Reads the LAN socket while the window has room; each read goes out as one data frame.
static void pump_in(tunnel_t *t)
{
    for (int i = 0; i < READS_PER_PASS && t->state == T_OPEN; ++i) {
        const int64_t room = LAN_TUNNEL_WINDOW - (t->sent - t->acked);
        if (room <= 0) {
            return;
        }
        const size_t want = room < LAN_TUNNEL_MAX_FRAME ? (size_t)room : LAN_TUNNEL_MAX_FRAME;
        const ssize_t n = recv(t->fd, s_raw, want, MSG_DONTWAIT);
        if (n == 0) {
            finish(t, "", true);  // the LAN host closed: a plain eof
            return;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            char err[ERR_MAX];
            if (errno == ECONNRESET) {
                snprintf(err, sizeof(err), "%s reset the connection", t->remote);
            } else {
                snprintf(err, sizeof(err), "reading from %s failed: %s", t->remote, strerror(errno));
            }
            finish(t, err, true);
            return;
        }
        t->sent += n;
        t->active_us = esp_timer_get_time();
        if (!send_data(t, s_raw, (size_t)n)) {
            finish(t, "", false);  // the Connector socket is gone or stuck: nothing more can be said
            return;
        }
        t->frames_up++;
        note_ram(t);
    }
}

// Verdicts, deadlines and idle time.
static void housekeeping(tunnel_t *t, int64_t now)
{
#if COMMA_TUNNEL_DEBUG && defined(ESP_PLATFORM)
    static int64_t s_dbg_us;
    if (t->state == T_OPEN && now - s_dbg_us >= 1000000) {
        s_dbg_us = now;
        debug_log(t);
    }
#endif
    const char *reason = NULL;
    const lan_tunnel_verdict_t v = s_host.check ? s_host.check(s_host.ctx, t->session, &reason) : LAN_TUNNEL_GO;
    if (v == LAN_TUNNEL_END_SILENT) {
        finish(t, "", false);
    } else if (v == LAN_TUNNEL_END) {
        finish(t, reason ? reason : "tunnel stopped", true);
    } else if (t->state == T_DIALING && now >= t->deadline_us) {
        char err[ERR_MAX];
        snprintf(err, sizeof(err), "connect to %s timed out after %d s", t->remote, t->dial_timeout_s);
        finish(t, err, true);
    } else if (t->state == T_DRAINING && (!t->head || now >= t->deadline_us)) {
        finish(t, "", false);
    } else if (t->state == T_OPEN && now - t->active_us >= LAN_TUNNEL_IDLE_S * 1000000LL) {
        char err[64];
        snprintf(err, sizeof(err), "tunnel idle for %d s", LAN_TUNNEL_IDLE_S);
        finish(t, err, true);
    }
}

void lan_tunnel_step(int wait_ms)
{
    if (atomic_exchange(&s_command_failure, false)) {
        for (int i = 0; i < LAN_TUNNEL_MAX_OPEN; ++i) {
            if (s_t[i].state != T_FREE) finish(&s_t[i], "tunnel command unavailable", true);
        }
    }
    drain_cmds(s_count > 0 ? 0 : pdMS_TO_TICKS(wait_ms));
    if (s_count == 0) {
        return;
    }
    const int64_t now = esp_timer_get_time();
    for (int i = 0; i < LAN_TUNNEL_MAX_OPEN; ++i) {
        if (s_t[i].state != T_FREE) {
            housekeeping(&s_t[i], now);
        }
    }
    fd_set rd, wr, ex;
    FD_ZERO(&rd);
    FD_ZERO(&wr);
    FD_ZERO(&ex);
    int maxfd = -1;
    for (int i = 0; i < LAN_TUNNEL_MAX_OPEN; ++i) {
        tunnel_t *t = &s_t[i];
        if (t->state == T_FREE || t->fd < 0) {
            continue;
        }
        if (t->state == T_DIALING) {
            FD_SET(t->fd, &wr);
            FD_SET(t->fd, &ex);
        } else {
            if (t->state == T_OPEN && t->sent - t->acked < LAN_TUNNEL_WINDOW) {
                FD_SET(t->fd, &rd);
            }
            if (t->head) {
                FD_SET(t->fd, &wr);
            }
            FD_SET(t->fd, &ex);
        }
        if (t->fd > maxfd) {
            maxfd = t->fd;
        }
    }
    const int wait = wait_ms < POLL_MS ? wait_ms : POLL_MS;
    struct timeval tv = {.tv_sec = 0, .tv_usec = wait * 1000};
    const int n = maxfd >= 0 ? select(maxfd + 1, &rd, &wr, &ex, &tv) : 0;
    if (maxfd < 0) {
        vTaskDelay(pdMS_TO_TICKS(wait));
    }
    if (n > 0) {
        for (int i = 0; i < LAN_TUNNEL_MAX_OPEN; ++i) {
            tunnel_t *t = &s_t[i];
            if (t->state == T_FREE || t->fd < 0) {
                continue;
            }
            const int fd = t->fd;
            if (t->state == T_DIALING) {
                if (FD_ISSET(fd, &wr) || FD_ISSET(fd, &ex)) {
                    dial_done(t);
                }
                continue;
            }
            if (FD_ISSET(fd, &wr) || FD_ISSET(fd, &ex)) {
                pump_out(t);
            }
            if (t->state == T_OPEN && (FD_ISSET(fd, &rd) || FD_ISSET(fd, &ex))) {
                pump_in(t);
            }
        }
    }
    // Server data that arrived while the socket was already writable goes out without waiting for
    // the next select.
    drain_cmds(0);
    for (int i = 0; i < LAN_TUNNEL_MAX_OPEN; ++i) {
        if ((s_t[i].state == T_OPEN || s_t[i].state == T_DRAINING) && s_t[i].head) {
            pump_out(&s_t[i]);
        }
    }
}

static void tunnel_task(void *arg)
{
    (void)arg;
    for (;;) {
        lan_tunnel_step(1000);
#if CONFIG_COMMA_HEAP_DIAGNOSTICS
        static int64_t last;
        if (esp_timer_get_time() - last > 1000000) {
            last = esp_timer_get_time();
            comma_heap_log("tunnel_sample");
        }
#endif
    }
}

bool lan_tunnel_start(const lan_tunnel_host_t *host)
{
    comma_heap_log("tunnel_task_start");
    s_host = *host;
    for (int i = 0; i < LAN_TUNNEL_MAX_OPEN; ++i) {
        s_t[i].fd = -1;
    }
    s_cmds = xQueueCreate(QUEUE_LEN, sizeof(cmd_t *));
    s_raw = psram_alloc(LAN_TUNNEL_MAX_FRAME);
    s_frame = psram_alloc(FRAME_CAP);
    if (!s_cmds || !s_raw || !s_frame) {
        ESP_LOGE(TAG, "out of memory, lan_tcp_dial off");
        goto fail;
    }
    // Sockets only, never flash: the stack can be in PSRAM.
    if (xTaskCreateWithCaps(tunnel_task, "lan_tunnel", TASK_STACK, NULL, TASK_PRIO, NULL, COMMA_BUFFER_CAPS) != pdPASS) {
        ESP_LOGE(TAG, "task create failed, lan_tcp_dial off");
        goto fail;
    }
    return true;
fail:
    if (s_cmds) vQueueDelete(s_cmds);
    free(s_raw);
    free(s_frame);
    s_cmds = NULL;
    s_raw = NULL;
    s_frame = NULL;
    return false;
}

int lan_tunnel_count(void)
{
    return s_count;
}

// ---- Connector-task side ---------------------------------------------------------------------------

static bool post(cmd_t *c)
{
    // The tunnel task drains the queue every POLL_MS, so this only waits when something is badly wrong.
    if (xQueueSend(s_cmds, &c, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGW(TAG, "%s: command queue full, dropped", c->id);
        s_command_failure = true;
        free(c->block);
        free(c);
        return false;
    }
    return true;
}

static cmd_t *new_cmd(cmd_kind_t kind, const char *id, unsigned session)
{
    cmd_t *c = psram_alloc(sizeof(cmd_t));
    if (!c) {
        return NULL;
    }
    memset(c, 0, sizeof(*c));
    c->kind = kind;
    c->session = session;
    strlcpy(c->id, id, sizeof(c->id));
    c->port = -1;
    return c;
}

bool lan_tunnel_dial(const char *id, const cJSON *params, unsigned session)
{
    if (!s_cmds) return false;
    cmd_t *c = new_cmd(CMD_DIAL, id, session);
    if (!c) {
        return false;
    }
    const cJSON *host = cJSON_IsObject(params) ? cJSON_GetObjectItemCaseSensitive(params, "host") : NULL;
    const cJSON *port = cJSON_IsObject(params) ? cJSON_GetObjectItemCaseSensitive(params, "port") : NULL;
    const cJSON *timeout = cJSON_IsObject(params) ? cJSON_GetObjectItemCaseSensitive(params, "timeout_seconds") : NULL;
    if (cJSON_IsString(host) && host->valuestring[0] && strlen(host->valuestring) <= HOST_MAX) {
        strlcpy(c->host, host->valuestring, sizeof(c->host));
        c->host_ok = true;
    }
    if (cJSON_IsNumber(port)) {
        c->port = port->valuedouble;
    }
    if (cJSON_IsNumber(timeout)) {
        c->timeout_s = timeout->valuedouble;
    }
    return post(c);
}

bool lan_tunnel_frame(const char *id, const cJSON *msg, unsigned session)
{
    if (!s_cmds) return false;
    const cJSON *stream = cJSON_GetObjectItemCaseSensitive(msg, "stream");
    const cJSON *channel = cJSON_IsObject(stream) ? cJSON_GetObjectItemCaseSensitive(stream, "channel") : NULL;
    if (!cJSON_IsString(channel)) {
        return false;
    }
    const bool ack = strcmp(channel->valuestring, "tcp_ack") == 0;
    if (!ack && strcmp(channel->valuestring, "tcp") != 0) {
        return false;
    }
    if (strlen(id) > ID_MAX) {
        return true;
    }
    cmd_t *c = NULL;
    if (ack) {
        const cJSON *seq = cJSON_GetObjectItemCaseSensitive(stream, "seq");
        if (!cJSON_IsNumber(seq)) {
            return true;
        }
        c = new_cmd(CMD_ACK, id, session);
        if (c) {
            c->seq = (int64_t)seq->valuedouble;
        }
    } else {
        const cJSON *eof = cJSON_GetObjectItemCaseSensitive(stream, "eof");
        const cJSON *error = cJSON_GetObjectItemCaseSensitive(msg, "error");
        const cJSON *data = cJSON_GetObjectItemCaseSensitive(stream, "data");
        if (cJSON_IsTrue(eof) || (cJSON_IsString(error) && error->valuestring[0])) {
            c = new_cmd(CMD_EOF, id, session);
        } else if (cJSON_IsString(data) && data->valuestring[0]) {
            const size_t text = strlen(data->valuestring);
            const size_t most = text / 4 * 3;
            if (text > B64_LEN(LAN_TUNNEL_SERVER_FRAME_MAX)) {
                c = new_cmd(CMD_BAD, id, session);
            } else {
                c = new_cmd(CMD_DATA, id, session);
                block_t *b = c ? psram_alloc(sizeof(block_t) + most) : NULL;
                if (c && !b) {
                    comma_heap_log("tunnel_decode_oom");
                    c->kind = CMD_OOM;
                } else if (c) {
                    const long n = lan_tunnel_b64_decode(data->valuestring, text, b->data, most);
                    memset(b, 0, sizeof(*b));
                    if (n <= 0 || n > LAN_TUNNEL_SERVER_FRAME_MAX) {
                        free(b);
                        c->kind = CMD_BAD;
                    } else {
                        b->len = (size_t)n;
                        c->block = b;
                    }
                }
            }
        } else {
            return true;  // nothing to do
        }
    }
    if (!c) {
        ESP_LOGW(TAG, "%s: out of memory for a tunnel frame", id);
        s_command_failure = true;
        return true;
    }
    post(c);
    return true;
}
