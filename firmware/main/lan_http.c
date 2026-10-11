#include "heap_diag.h"
#include "memory_caps.h"
// http_request for the Connector. Built on esp_transport + http_parser rather than esp_http_client:
// esp_http_client reads nothing from a response that has neither Content-Length nor chunked
// encoding (its read loop compares against content_length = -1), and plenty of small LAN devices
// answer HTTP/1.0-style, closing the connection to end the body.

#include "lan_http.h"

#include "lan_dest.h"
#include "wifi.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_transport.h"
#include "esp_transport_ssl.h"
#include "esp_transport_tcp.h"
#include "http_parser.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "psa/crypto.h"

#define HOST_MAX 253
#define READ_CHUNK 2048
#define HEADER_NAME_MAX 128
#define HEADERS_MAX (16 * 1024)  // all response header bytes together
#define ERR_MAX 400
#define USER_AGENT "comma-t-circle/0.1"

static const char *TAG = "lan_http";

// ---- Growable string in PSRAM ------------------------------------------------------------------

typedef struct {
    char *p;
    size_t len;
    size_t cap;
    bool oom;
} sb_t;

static void sb_add(sb_t *b, const char *s, size_t n)
{
    if (b->oom) {
        return;
    }
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->len + n + 1) {
            cap *= 2;
        }
        char *p = heap_caps_realloc(b->p, cap, COMMA_BUFFER_CAPS);
        if (!p) {
            b->oom = true;
            return;
        }
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}

static void sb_str(sb_t *b, const char *s)
{
    sb_add(b, s, strlen(s));
}

static void sb_free(sb_t *b)
{
    free(b->p);
    memset(b, 0, sizeof(*b));
}

// Length of the valid UTF-8 sequence at s (1-4), or 0 when it is invalid or cut off.
static size_t utf8_len(const uint8_t *s, size_t n)
{
    const uint8_t c = s[0];
    size_t len;
    uint32_t cp;
    if (c < 0x80) {
        return 1;
    } else if (c >= 0xC2 && c <= 0xDF) {
        len = 2;
        cp = c & 0x1F;
    } else if (c >= 0xE0 && c <= 0xEF) {
        len = 3;
        cp = c & 0x0F;
    } else if (c >= 0xF0 && c <= 0xF4) {
        len = 4;
        cp = c & 0x07;
    } else {
        return 0;
    }
    if (n < len) {
        return 0;
    }
    for (size_t i = 1; i < len; ++i) {
        if ((s[i] & 0xC0) != 0x80) {
            return 0;
        }
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    // Overlong forms, UTF-16 surrogates and anything past U+10FFFF.
    if ((len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
        return 0;
    }
    return len;
}

// A JSON string literal of these bytes. Invalid UTF-8 becomes U+FFFD, like Go's encoding/json.
static void sb_json(sb_t *b, const char *s, size_t n)
{
    const uint8_t *u = (const uint8_t *)s;
    sb_add(b, "\"", 1);
    size_t run = 0;  // start of the pending stretch that needs no escaping
    for (size_t i = 0; i < n;) {
        const uint8_t c = u[i];
        const char *esc = NULL;
        char hex[8];
        size_t step = 1;
        if (c == '"') {
            esc = "\\\"";
        } else if (c == '\\') {
            esc = "\\\\";
        } else if (c == '\n') {
            esc = "\\n";
        } else if (c == '\r') {
            esc = "\\r";
        } else if (c == '\t') {
            esc = "\\t";
        } else if (c < 0x20) {
            snprintf(hex, sizeof(hex), "\\u%04x", c);
            esc = hex;
        } else if (c >= 0x80) {
            step = utf8_len(u + i, n - i);
            if (step == 0) {
                esc = "\xEF\xBF\xBD";
                step = 1;
            }
        }
        if (esc) {
            sb_add(b, s + run, i - run);
            sb_str(b, esc);
            run = i + step;
        }
        i += step;
    }
    sb_add(b, s + run, n - run);
    sb_add(b, "\"", 1);
}

static char *error_frame(const char *id, const char *message)
{
    sb_t b = {0};
    sb_str(&b, "{\"id\":");
    sb_json(&b, id, strlen(id));
    sb_str(&b, ",\"type\":\"error\",\"error\":");
    sb_json(&b, message, strlen(message));
    sb_str(&b, "}");
    if (b.oom) {
        sb_free(&b);
    }
    return b.p;
}

// ---- URL and destination -----------------------------------------------------------------------

typedef struct {
    bool https;
    char host[HOST_MAX + 1];
    int port;
    bool explicit_port;
    const char *path;  // from the first '/' or '?' after the authority; "" if none
    size_t path_len;   // without any #fragment
} url_t;

static bool parse_url(const char *url, url_t *u, char *err)
{
    memset(u, 0, sizeof(*u));
    for (const char *p = url; *p; ++p) {
        if ((unsigned char)*p <= 0x20 || *p == 0x7F) {
            snprintf(err, ERR_MAX, "'url' must not contain spaces or control characters");
            return false;
        }
    }
    const char *sep = strstr(url, "://");
    if (!sep || sep == url) {
        snprintf(err, ERR_MAX, "'url' must be an absolute http or https URL");
        return false;
    }
    const size_t scheme_len = (size_t)(sep - url);
    if (scheme_len == 4 && strncasecmp(url, "http", 4) == 0) {
        u->https = false;
    } else if (scheme_len == 5 && strncasecmp(url, "https", 5) == 0) {
        u->https = true;
    } else {
        snprintf(err, ERR_MAX, "'url' must use http or https");
        return false;
    }
    const char *auth = sep + 3;
    const size_t auth_len = strcspn(auth, "/?#");
    const char *host = auth;
    for (const char *p = auth; p < auth + auth_len; ++p) {
        if (*p == '@') {
            snprintf(err, ERR_MAX, "credentials in the URL are not supported; send an Authorization header");
            return false;
        }
    }
    if (auth_len == 0) {
        snprintf(err, ERR_MAX, "'url' must be an absolute http or https URL");
        return false;
    }
    if (host[0] == '[') {
        snprintf(err, ERR_MAX, "IPv6 destinations are not supported by this device");
        return false;
    }
    const char *colon = memchr(host, ':', auth_len);
    const size_t host_len = colon ? (size_t)(colon - host) : auth_len;
    if (host_len == 0 || host_len > HOST_MAX) {
        snprintf(err, ERR_MAX, "'url' has an invalid host");
        return false;
    }
    memcpy(u->host, host, host_len);
    u->port = u->https ? 443 : 80;
    if (colon) {
        const char *p = colon + 1;
        const char *end = auth + auth_len;
        long port = 0;
        if (p == end) {
            port = u->port;  // "host:" means the default port
        }
        for (; p < end; ++p) {
            if (*p < '0' || *p > '9' || port > 65535) {
                port = -1;
                break;
            }
            port = port * 10 + (*p - '0');
        }
        if (port < 1 || port > 65535) {
            snprintf(err, ERR_MAX, "'url' has an invalid port");
            return false;
        }
        u->port = (int)port;
        u->explicit_port = colon + 1 != end;
    }
    u->path = auth + auth_len;
    u->path_len = strcspn(u->path, "#");
    return true;
}

// ---- Chunk mode ---------------------------------------------------------------------------------

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// Standard base64 with padding (RFC 4648 §4), not NUL-terminated. Returns the length,
// 4 * ceil(n / 3).
static size_t b64_encode(const uint8_t *in, size_t n, char *out)
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

#define B64_LEN(n) (((n) + 2) / 3 * 4)

// One transfer's state: the pending raw bytes of the next frame, and the frame buffer they are
// encoded into. Both are allocated once per transfer (PSRAM) and reused for every frame.
typedef struct {
    const lan_http_sink_t *sink;
    uint8_t *raw;
    size_t raw_len;
    size_t want;     // raw bytes for the frame being filled
    char *frame;     // starts with the fixed prefix {"id":"…","type":"response_chunk","seq":
    size_t prefix_len;
    unsigned seq;    // frames sent
    psa_hash_operation_t sha;
    bool failed;
    char err[ERR_MAX];
} chunks_t;

static size_t chunk_want(const chunks_t *c)
{
    const size_t n = c->sink->frame_bytes(c->sink->ctx);
    return n == 0 ? 1 : n > LAN_HTTP_CHUNK_BYTES ? LAN_HTTP_CHUNK_BYTES : n;
}

static bool chunks_init(chunks_t *c, const lan_http_sink_t *sink, const char *id)
{
    memset(c, 0, sizeof(*c));
    c->sink = sink;
    sb_t prefix = {0};
    sb_str(&prefix, "{\"id\":");
    sb_json(&prefix, id, strlen(id));
    sb_str(&prefix, ",\"type\":\"response_chunk\",\"seq\":");
    c->raw = heap_caps_malloc(LAN_HTTP_CHUNK_BYTES, COMMA_BUFFER_CAPS);
    // prefix, seq, the data_b64 key, the base64 text, the closing "} and a NUL
    c->frame = heap_caps_malloc(prefix.len + 16 + 16 + B64_LEN(LAN_HTTP_CHUNK_BYTES) + 4,
                                COMMA_BUFFER_CAPS);
    c->sha = psa_hash_operation_init();
    const bool ok = !prefix.oom && c->raw && c->frame && psa_hash_setup(&c->sha, PSA_ALG_SHA_256) == PSA_SUCCESS;
    if (ok) {
        memcpy(c->frame, prefix.p, prefix.len);
        c->prefix_len = prefix.len;
        c->want = chunk_want(c);
    }
    sb_free(&prefix);
    return ok;
}

static void chunks_free(chunks_t *c)
{
    psa_hash_abort(&c->sha);
    free(c->raw);
    free(c->frame);
    c->raw = NULL;
    c->frame = NULL;
}

// Sends the pending bytes as the next frame. False once sending has failed.
static bool chunks_flush(chunks_t *c)
{
    if (c->failed) {
        return false;
    }
    if (c->raw_len == 0) {
        return true;
    }
    if (psa_hash_update(&c->sha, c->raw, c->raw_len) != PSA_SUCCESS) {
        snprintf(c->err, sizeof(c->err), "hashing the response failed");
        c->failed = true;
        return false;
    }
    size_t n = c->prefix_len;
    n += (size_t)sprintf(c->frame + n, "%u,\"data_b64\":\"", c->seq);
    n += b64_encode(c->raw, c->raw_len, c->frame + n);
    memcpy(c->frame + n, "\"}", 3);
    n += 2;
    if (!c->sink->send(c->sink->ctx, c->frame, n, c->raw_len, c->err, sizeof(c->err))) {
        if (!c->err[0]) {
            snprintf(c->err, sizeof(c->err), "sending the response to the server failed");
        }
        c->failed = true;
        return false;
    }
    c->seq++;
    c->raw_len = 0;
    c->want = chunk_want(c);
    return true;
}

// Body bytes in chunk mode: fill the frame buffer, send it whenever it holds `want` bytes.
static bool chunks_add(chunks_t *c, const char *at, size_t len)
{
    while (len > 0) {
        size_t take = c->want - c->raw_len;
        if (take > len) {
            take = len;
        }
        memcpy(c->raw + c->raw_len, at, take);
        c->raw_len += take;
        at += take;
        len -= take;
        if (c->raw_len >= c->want && !chunks_flush(c)) {
            return false;
        }
    }
    return true;
}

// ---- Response parsing --------------------------------------------------------------------------

typedef struct {
    cJSON *headers;  // canonical name -> [values]
    char field[HEADER_NAME_MAX + 1];
    size_t field_len;
    sb_t value;
    bool in_value;
    bool field_too_long;
    size_t header_bytes;
    bool headers_done;
    bool complete;
    sb_t body;
    chunks_t *chunks;   // chunk mode: the body streams out here instead of into body
    size_t body_bytes;  // body bytes taken (kept or sent)
    size_t limit;       // take at most limit bytes; one more means truncated
    bool truncated;
    bool oom;
} resp_t;

// Go's textproto.CanonicalMIMEHeaderKey for the usual token characters: content-type -> Content-Type.
static void canonical_name(char *s)
{
    bool upper = true;
    for (; *s; ++s) {
        if (*s >= 'a' && *s <= 'z' && upper) {
            *s -= 'a' - 'A';
        } else if (*s >= 'A' && *s <= 'Z' && !upper) {
            *s += 'a' - 'A';
        }
        upper = *s == '-';
    }
}

static int commit_header(resp_t *r)
{
    if (!r->in_value) {
        return 0;
    }
    r->in_value = false;
    if (r->field_too_long || r->field_len == 0) {
        sb_free(&r->value);
        r->field_len = 0;
        r->field_too_long = false;
        return 0;
    }
    r->field[r->field_len] = 0;
    canonical_name(r->field);
    cJSON *list = cJSON_GetObjectItemCaseSensitive(r->headers, r->field);
    if (!list) {
        list = cJSON_AddArrayToObject(r->headers, r->field);
    }
    cJSON *item = cJSON_CreateString(r->value.p ? r->value.p : "");
    if (!list || !item) {
        cJSON_Delete(item);
        r->oom = true;
        return 1;
    }
    cJSON_AddItemToArray(list, item);
    sb_free(&r->value);
    r->field_len = 0;
    return 0;
}

static int on_header_field(http_parser *p, const char *at, size_t len)
{
    resp_t *r = p->data;
    if (commit_header(r) != 0) {
        return 1;
    }
    r->header_bytes += len;
    if (r->field_len + len > HEADER_NAME_MAX) {
        r->field_too_long = true;
    } else {
        memcpy(r->field + r->field_len, at, len);
        r->field_len += len;
    }
    return r->header_bytes > HEADERS_MAX;
}

static int on_header_value(http_parser *p, const char *at, size_t len)
{
    resp_t *r = p->data;
    r->in_value = true;
    r->header_bytes += len;
    sb_add(&r->value, at, len);
    if (r->value.oom) {
        r->oom = true;
        return 1;
    }
    return r->header_bytes > HEADERS_MAX;
}

static int on_headers_complete(http_parser *p)
{
    resp_t *r = p->data;
    r->headers_done = true;
    return commit_header(r);
}

static int on_body(http_parser *p, const char *at, size_t len)
{
    resp_t *r = p->data;
    const size_t room = r->limit - r->body_bytes;
    const bool over = len > room;
    if (over) {
        len = room;
    }
    if (r->chunks) {
        if (!chunks_add(r->chunks, at, len)) {
            return 1;
        }
    } else {
        sb_add(&r->body, at, len);
        if (r->body.oom) {
            r->oom = true;
            return 1;
        }
    }
    r->body_bytes += len;
    if (over) {
        r->truncated = true;
        return 1;  // enough: stop reading
    }
    return 0;
}

static int on_message_complete(http_parser *p)
{
    ((resp_t *)p->data)->complete = true;
    return 0;
}

// ---- Request -----------------------------------------------------------------------------------

static const char *const METHODS[] = {"GET", "POST", "PUT", "PATCH", "DELETE"};

static bool header_is(const char *name, const char *want)
{
    return strcasecmp(name, want) == 0;
}

// Header values as salix-connect's stringFromAny would print the common JSON types.
static char *header_value(const cJSON *v)
{
    if (cJSON_IsString(v)) {
        return strdup(v->valuestring);
    }
    if (cJSON_IsNull(v)) {
        return strdup("");
    }
    if (cJSON_IsBool(v)) {
        return strdup(cJSON_IsTrue(v) ? "true" : "false");
    }
    return cJSON_PrintUnformatted(v);
}

// Request line and headers. Framing headers (Host, Content-Length, Connection, Transfer-Encoding)
// are always the device's own.
static bool build_request(sb_t *req, const char *method, const url_t *u, const cJSON *headers, size_t body_len,
                          char *err)
{
    sb_str(req, method);
    sb_str(req, " ");
    if (u->path_len == 0 || u->path[0] != '/') {
        sb_str(req, "/");
    }
    sb_add(req, u->path, u->path_len);
    sb_str(req, " HTTP/1.1\r\nHost: ");
    sb_str(req, u->host);
    if (u->explicit_port) {
        char port[8];
        snprintf(port, sizeof(port), ":%d", u->port);
        sb_str(req, port);
    }
    sb_str(req, "\r\nConnection: close\r\n");
    bool has_type = false;
    bool has_agent = false;
    const cJSON *list = cJSON_IsObject(headers) ? headers : NULL;
    const cJSON *h;
    cJSON_ArrayForEach(h, list)
    {
        const char *name = h->string;
        if (!name || !name[0] || header_is(name, "Host") || header_is(name, "Content-Length") ||
            header_is(name, "Connection") || header_is(name, "Transfer-Encoding")) {
            continue;
        }
        for (const char *c = name; *c; ++c) {
            if ((unsigned char)*c <= 0x20 || *c == ':' || *c == 0x7F) {
                snprintf(err, ERR_MAX, "invalid header name: %.60s", name);
                return false;
            }
        }
        char *value = header_value(h);
        if (!value) {
            snprintf(err, ERR_MAX, "out of memory");
            return false;
        }
        if (strpbrk(value, "\r\n")) {
            free(value);
            snprintf(err, ERR_MAX, "header %.60s has a line break in its value", name);
            return false;
        }
        has_type |= header_is(name, "Content-Type");
        has_agent |= header_is(name, "User-Agent");
        sb_str(req, name);
        sb_str(req, ": ");
        sb_str(req, value);
        sb_str(req, "\r\n");
        free(value);
    }
    if (!has_agent) {
        sb_str(req, "User-Agent: " USER_AGENT "\r\n");
    }
    if (body_len > 0 && !has_type) {
        sb_str(req, "Content-Type: application/json\r\n");
    }
    // Like Go's net/http: an empty body still gets "Content-Length: 0" on POST, PUT and PATCH.
    if (body_len > 0 || strcmp(method, "POST") == 0 || strcmp(method, "PUT") == 0 || strcmp(method, "PATCH") == 0) {
        char line[40];
        snprintf(line, sizeof(line), "Content-Length: %u\r\n", (unsigned)body_len);
        sb_str(req, line);
    }
    sb_str(req, "\r\n");
    if (req->oom) {
        snprintf(err, ERR_MAX, "out of memory");
        return false;
    }
    return true;
}

static int remaining_ms(int64_t deadline_us)
{
    const int64_t left = (deadline_us - esp_timer_get_time()) / 1000;
    return left > 0 ? (int)left : 0;
}

static bool write_all(esp_transport_handle_t t, const char *p, size_t n, int64_t deadline_us)
{
    while (n > 0) {
        const int ms = remaining_ms(deadline_us);
        if (ms == 0) {
            return false;
        }
        const int w = esp_transport_write(t, p, (int)n, ms);
        if (w <= 0) {
            return false;
        }
        p += w;
        n -= (size_t)w;
    }
    return true;
}

static char *response_frame(const char *id, int status, const resp_t *r, const char *url, const char *method,
                            const char *sha256_hex)
{
    char *headers = cJSON_PrintUnformatted(r->headers);
    sb_t b = {0};
    char num[64];
    sb_str(&b, "{\"id\":");
    sb_json(&b, id, strlen(id));
    snprintf(num, sizeof(num), ",\"type\":\"response\",\"result\":{\"status\":%d,\"headers\":", status);
    sb_str(&b, num);
    sb_str(&b, headers ? headers : "{}");
    if (r->chunks) {
        snprintf(num, sizeof(num), ",\"body_encoding\":\"chunks\",\"chunks\":%u,\"sha256\":\"", r->chunks->seq);
        sb_str(&b, num);
        sb_str(&b, sha256_hex);
        sb_str(&b, "\"");
    } else {
        sb_str(&b, ",\"body\":");
        sb_json(&b, r->body.p ? r->body.p : "", r->body.len);
    }
    sb_str(&b, r->truncated ? ",\"truncated\":true" : ",\"truncated\":false");
    sb_str(&b, ",\"final_url\":");
    sb_json(&b, url, strlen(url));
    sb_str(&b, ",\"method\":");
    sb_json(&b, method, strlen(method));
    snprintf(num, sizeof(num), ",\"body_bytes\":%u}}", (unsigned)r->body_bytes);
    sb_str(&b, num);
    cJSON_free(headers);
    if (b.oom || !headers) {
        sb_free(&b);
    }
    return b.p;
}

// The chunk transfer's end: the last frame, then the digest as lowercase hex (65 bytes).
static bool chunks_finish(chunks_t *c, char *hex)
{
    if (!chunks_flush(c)) {
        return false;
    }
    uint8_t digest[32];
    size_t digest_len = 0;
    if (psa_hash_finish(&c->sha, digest, sizeof(digest), &digest_len) != PSA_SUCCESS || digest_len != 32) {
        snprintf(c->err, sizeof(c->err), "hashing the response failed");
        return false;
    }
    for (size_t i = 0; i < 32; ++i) {
        sprintf(hex + 2 * i, "%02x", digest[i]);
    }
    return true;
}

char *lan_http_run(const char *id, const cJSON *params, const lan_http_sink_t *sink)
{
    char err[ERR_MAX] = "";
    const int64_t started_us = esp_timer_get_time();
    if (!cJSON_IsObject(params)) {
        params = NULL;  // every field below has a default or a clear error
    }

    // Method (default POST, like salix-connect).
    const cJSON *jm = cJSON_GetObjectItemCaseSensitive(params, "method");
    const char *raw_method = cJSON_IsString(jm) && jm->valuestring[0] ? jm->valuestring : "POST";
    const char *method = NULL;
    for (size_t i = 0; i < sizeof(METHODS) / sizeof(METHODS[0]); ++i) {
        if (strcasecmp(raw_method, METHODS[i]) == 0) {
            method = METHODS[i];
        }
    }
    if (!method) {
        snprintf(err, sizeof(err), "unsupported HTTP method: %.40s", raw_method);
        return error_frame(id, err);
    }

    const cJSON *ju = cJSON_GetObjectItemCaseSensitive(params, "url");
    if (!cJSON_IsString(ju) || !ju->valuestring[0]) {
        return error_frame(id, "'url' is required");
    }
    const char *url_text = ju->valuestring;
    url_t url;
    if (!parse_url(url_text, &url, err)) {
        return error_frame(id, err);
    }

    // Chunk mode only when asked for; anything else stays exactly the text mode it always was.
    const cJSON *jr = cJSON_GetObjectItemCaseSensitive(params, "response_mode");
    bool chunk_mode = false;
    if (cJSON_IsString(jr) && strcmp(jr->valuestring, "chunks") == 0) {
        if (!sink) {
            return error_frame(id, "response_mode \"chunks\" is not available here");
        }
        chunk_mode = true;
    } else if (jr && !cJSON_IsNull(jr)) {
        return error_frame(id, "unsupported response_mode (this device supports \"chunks\")");
    }
    if (chunk_mode && sink->refuse) {
        const char *busy = sink->refuse(sink->ctx);
        if (busy) {
            ESP_LOGW(TAG, "%s refused: %s", id, busy);
            return error_frame(id, busy);
        }
    }
    const int timeout_max_s = chunk_mode ? LAN_HTTP_CHUNK_TIMEOUT_MAX_S : LAN_HTTP_TIMEOUT_MAX_S;
    const size_t limit_max = chunk_mode ? LAN_HTTP_CHUNK_MAX_TOTAL : LAN_HTTP_MAX_BYTES;

    const cJSON *jt = cJSON_GetObjectItemCaseSensitive(params, "timeout_seconds");
    // salix-connect's default (120 s) is over the device cap, so no value or a bad one means the cap.
    int timeout_s = timeout_max_s;
    if (cJSON_IsNumber(jt) && jt->valuedouble >= 1 && jt->valuedouble < timeout_max_s) {
        timeout_s = (int)jt->valuedouble;
    }
    const cJSON *jx = cJSON_GetObjectItemCaseSensitive(params, "max_bytes");
    double max_bytes = cJSON_IsNumber(jx) ? jx->valuedouble : 0;
    const size_t limit = (max_bytes <= 0 || max_bytes > limit_max) ? limit_max : (size_t)max_bytes;

    const cJSON *jb = cJSON_GetObjectItemCaseSensitive(params, "body");
    const char *body = cJSON_IsString(jb) ? jb->valuestring : "";
    const size_t body_len = strlen(body);

    const int64_t deadline_us = started_us + (int64_t)timeout_s * 1000000;
    uint32_t ip = 0;
    if (!lan_dest_resolve(url.host, &ip, err) || !lan_dest_check(ip, err)) {
        ESP_LOGW(TAG, "%s refused: %s", id, err);
        return error_frame(id, err);
    }
    char ip_text[16];
    inet_ntoa_r(*(struct in_addr *)&ip, ip_text, sizeof(ip_text));

    sb_t req = {0};
    if (!build_request(&req, method, &url, cJSON_GetObjectItemCaseSensitive(params, "headers"), body_len, err)) {
        sb_free(&req);
        return error_frame(id, err);
    }

    esp_transport_handle_t t = url.https ? esp_transport_ssl_init() : esp_transport_tcp_init();
    resp_t r = {.headers = cJSON_CreateObject(), .limit = limit};
    char *buf = heap_caps_malloc(READ_CHUNK, COMMA_BUFFER_CAPS);
    char *frame = NULL;
    int status = 0;
    chunks_t chunks;
    bool chunks_ready = false;
    if (chunk_mode) {
        chunks_ready = chunks_init(&chunks, sink, id);
        r.chunks = &chunks;
    }
    if (!t || !r.headers || !buf || (chunk_mode && !chunks_ready)) {
        snprintf(err, sizeof(err), "out of memory");
        goto done;
    }
    if (url.https) {
        esp_transport_ssl_crt_bundle_attach(t, esp_crt_bundle_attach);
        // The connection goes to the IP checked above (no second lookup to rebind), while TLS
        // still verifies the certificate against the name in the URL.
        esp_transport_ssl_set_common_name(t, url.host);
    }

    ESP_LOGI(TAG, "%s %s %s:%d%s (timeout %d s, max %u B%s)", id, method, url.host, url.port, url.https ? " tls" : "",
             timeout_s, (unsigned)limit, chunk_mode ? ", chunks" : "");
    if (esp_transport_connect(t, ip_text, url.port, remaining_ms(deadline_us)) < 0) {
        const int e = esp_transport_get_errno(t);
        if (remaining_ms(deadline_us) == 0) {
            snprintf(err, sizeof(err), "request to %s timed out after %d s", url.host, timeout_s);
        } else if (e == ECONNREFUSED || e == ECONNRESET) {
            // lwIP reports a refused connect (RST to the SYN) as ECONNRESET, "Connection reset by
            // peer", which read like the T-Circle itself had reset (G2, port 8800).
            snprintf(err, sizeof(err), "connect to %s:%d refused: nothing is listening on that port", url.host,
                     url.port);
        } else if (url.https && e == 0) {
            snprintf(err, sizeof(err), "TLS connection to %s:%d failed (handshake or untrusted certificate)", url.host,
                     url.port);
        } else {
            snprintf(err, sizeof(err), "connect to %s:%d failed: %s", url.host, url.port,
                     e ? strerror(e) : "connection refused or unreachable");
        }
        goto done;
    }
    if (!write_all(t, req.p, req.len, deadline_us) || !write_all(t, body, body_len, deadline_us)) {
        snprintf(err, sizeof(err), remaining_ms(deadline_us) ? "sending the request to %s failed"
                                                              : "request to %s timed out while sending",
                 url.host);
        goto done;
    }

    http_parser parser;
    http_parser_init(&parser, HTTP_RESPONSE);
    parser.data = &r;
    http_parser_settings settings = {
        .on_header_field = on_header_field,
        .on_header_value = on_header_value,
        .on_headers_complete = on_headers_complete,
        .on_body = on_body,
        .on_message_complete = on_message_complete,
    };
    while (!r.complete && !r.truncated) {
        const int ms = remaining_ms(deadline_us);
        if (ms == 0) {
            snprintf(err, sizeof(err), "request to %s timed out after %d s", url.host, timeout_s);
            goto done;
        }
        const int n = esp_transport_read(t, buf, READ_CHUNK, ms > 1000 ? 1000 : ms);
        if (n == ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT) {
            continue;
        }
        if (n < 0) {
            // Closed (FIN or error): an HTTP/1.0-style body ends here; tell the parser.
            http_parser_execute(&parser, &settings, NULL, 0);
            if (!r.complete && !r.truncated) {
                snprintf(err, sizeof(err), "%s closed the connection before the whole response arrived", url.host);
                goto done;
            }
            break;
        }
        const size_t used = http_parser_execute(&parser, &settings, buf, (size_t)n);
        if (chunk_mode && chunks.failed) {
            strlcpy(err, chunks.err, sizeof(err));
            goto done;
        }
        if (r.oom) {
            snprintf(err, sizeof(err), "out of memory reading the response");
            goto done;
        }
        if (used != (size_t)n && !r.truncated && !r.complete) {
            snprintf(err, sizeof(err), r.header_bytes > HEADERS_MAX ? "response headers from %s are too large"
                                                                    : "invalid HTTP response from %s",
                     url.host);
            goto done;
        }
    }
    status = parser.status_code;
    char sha256_hex[65] = "";
    if (chunk_mode && !chunks_finish(&chunks, sha256_hex)) {
        strlcpy(err, chunks.err, sizeof(err));
        goto done;
    }
    frame = response_frame(id, status, &r, url_text, method, sha256_hex);
    if (!frame) {
        snprintf(err, sizeof(err), "out of memory building the response");
    }

done:;
    const int64_t took_ms = (esp_timer_get_time() - started_us) / 1000;
    if (chunk_mode) {
        ESP_LOGI(TAG, "%s -> %s%d, %u B in %u chunks%s, %lld ms (%lld KB/s)", id, frame ? "" : "error ",
                 frame ? status : 0, (unsigned)r.body_bytes, chunks_ready ? chunks.seq : 0,
                 r.truncated ? " (truncated)" : "", (long long)took_ms,
                 (long long)(took_ms > 0 ? (int64_t)r.body_bytes * 1000 / 1024 / took_ms : 0));
    } else {
        ESP_LOGI(TAG, "%s -> %s%d, %u B%s, %lld ms", id, frame ? "" : "error ", frame ? status : 0,
                 (unsigned)r.body_bytes, r.truncated ? " (truncated)" : "", (long long)took_ms);
    }
    if (!frame) {
        ESP_LOGW(TAG, "%s failed: %s", id, err);
        frame = error_frame(id, err);
    }
    comma_heap_log(chunk_mode ? "http_chunks_peak" : "http_text_peak");
    if (t) {
        esp_transport_close(t);
        esp_transport_destroy(t);
    }
    free(buf);
    if (chunk_mode) {
        chunks_free(&chunks);
    }
    sb_free(&req);
    sb_free(&r.value);
    sb_free(&r.body);
    cJSON_Delete(r.headers);
    comma_heap_log("http_cleanup");
    return frame;
}
