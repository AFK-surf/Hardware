// SPDX-License-Identifier: Apache-2.0
#include "json_buf.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_heap_caps.h"
#include "memory_caps.h"

void jbuf_add(jbuf_t *b, const char *s, size_t n)
{
    if (b->oom || b->full) {
        return;
    }
    if (b->limit && n > b->limit - b->len) {
        b->full = true;
        return;
    }
    if (b->measure) {
        b->len += n;
        return;
    }
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->len + n + 1) {
            cap *= 2;
        }
        if (b->limit && cap > b->limit + 1) {
            cap = b->limit + 1;
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

void jbuf_str(jbuf_t *b, const char *s)
{
    jbuf_add(b, s, strlen(s));
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

void jbuf_json(jbuf_t *b, const char *s, size_t n)
{
    const uint8_t *u = (const uint8_t *)s;
    jbuf_add(b, "\"", 1);
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
            jbuf_add(b, s + run, i - run);
            jbuf_str(b, esc);
            run = i + step;
        }
        i += step;
    }
    jbuf_add(b, s + run, n - run);
    jbuf_add(b, "\"", 1);
}

void jbuf_free(jbuf_t *b)
{
    free(b->p);
    memset(b, 0, sizeof(*b));
}

static void error_frame(jbuf_t *b, const char *id, const char *message)
{
    jbuf_str(b, "{\"id\":");
    jbuf_json_str(b, id);
    jbuf_str(b, ",\"type\":\"error\",\"error\":");
    jbuf_json_str(b, message);
    jbuf_str(b, "}");
}

char *json_error_frame(const char *id, const char *message)
{
    jbuf_t b = {0};
    error_frame(&b, id, message);
    if (b.oom) {
        jbuf_free(&b);
    }
    return b.p;
}

size_t json_error_frame_into(char *out, size_t cap, const char *id, const char *message)
{
    if (cap < 2) {
        return 0;
    }
    // A limit below the capacity means the buffer never grows: nothing is allocated or freed.
    jbuf_t b = {.p = out, .cap = cap, .limit = cap - 1};
    out[0] = 0;
    error_frame(&b, id, message);
    return b.full ? 0 : b.len;
}
