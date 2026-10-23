// SPDX-License-Identifier: Apache-2.0
#pragma once

// Reply frames written as JSON text into one growing buffer, in PSRAM where the board has it. Big
// replies never become a cJSON tree in internal RAM, and every frame escapes strings and shapes
// `error` replies the same way.

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

// A buffer the caller owns works too: set p (with p[0] = 0), cap, and limit = cap - 1. It then never
// grows, and what does not fit sets full instead.
typedef struct {
    char *p;       // NUL-terminated; NULL until something is added (and always, while measuring)
    size_t len;
    size_t cap;
    size_t limit;  // most content bytes, the NUL excluded; 0 means unbounded
    bool oom;      // an allocation failed: the content is incomplete
    bool full;     // an addition did not fit `limit` and was left out
    bool measure;  // count len only, store nothing
} jbuf_t;

void jbuf_add(jbuf_t *b, const char *s, size_t n);
void jbuf_str(jbuf_t *b, const char *s);
// n bytes of s as a JSON string literal: quotes, backslashes and control characters escaped,
// invalid UTF-8 replaced by U+FFFD.
void jbuf_json(jbuf_t *b, const char *s, size_t n);
static inline void jbuf_json_str(jbuf_t *b, const char *s)
{
    jbuf_json(b, s, strlen(s));
}
void jbuf_free(jbuf_t *b);

// {"id":<id>,"type":"error","error":<message>}, the reply to a request that failed. NULL when out
// of memory; the caller frees it.
char *json_error_frame(const char *id, const char *message);
// The same frame written into out (cap bytes, the NUL included) without allocating, for replies to
// an allocation failure. Returns its length, or 0 when it does not fit.
size_t json_error_frame_into(char *out, size_t cap, const char *id, const char *message);
