// SPDX-License-Identifier: Apache-2.0
// Host test for the encoders every Connector reply and the voice call share: main/b64.c (the
// protocol's base64), main/json_buf.c (reply text and `error` frames) and main/ws_frame.c (WebSocket
// frames out of the client's pieces). Run: ./run.sh

#include "b64.h"
#include "json_buf.h"
#include "ws_frame.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

// ---- base64 ------------------------------------------------------------------------------------

static void test_b64_vectors(void)
{
    // RFC 4648 §10.
    static const char *const plain[] = {"", "f", "fo", "foo", "foob", "fooba", "foobar"};
    static const char *const coded[] = {"", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy"};
    for (int i = 0; i < 7; ++i) {
        char out[16];
        const size_t n = b64_encode((const uint8_t *)plain[i], strlen(plain[i]), out);
        CHECK(n == strlen(coded[i]) && n == B64_ENCODED_LEN(strlen(plain[i])) && memcmp(out, coded[i], n) == 0,
              "encode %s", plain[i]);
        uint8_t back[16];
        const long m = b64_decode(coded[i], strlen(coded[i]), back, sizeof(back));
        CHECK(m == (long)strlen(plain[i]) && memcmp(back, plain[i], (size_t)m) == 0, "decode %s", coded[i]);
    }
}

static void test_b64_round_trip(void)
{
    uint8_t in[300], back[300];
    char text[B64_ENCODED_LEN(300)];
    srand(7);
    for (size_t n = 0; n < sizeof(in); ++n) {
        for (size_t i = 0; i < n; ++i) {
            in[i] = (uint8_t)rand();
        }
        const size_t len = b64_encode(in, n, text);
        CHECK(len == B64_ENCODED_LEN(n), "encoded length for %zu", n);
        CHECK(b64_decode(text, len, back, sizeof(back)) == (long)n && memcmp(in, back, n) == 0, "round trip %zu", n);
    }
    uint8_t all[256], all_back[256];
    char all_text[B64_ENCODED_LEN(256)];
    for (int i = 0; i < 256; ++i) {
        all[i] = (uint8_t)i;
    }
    const size_t n = b64_encode(all, sizeof(all), all_text);
    CHECK(b64_decode(all_text, n, all_back, sizeof(all_back)) == 256 && memcmp(all, all_back, 256) == 0,
          "every byte value");
}

static void test_b64_strict(void)
{
    // Malformed: lengths, misplaced or extra padding, other alphabets, whitespace, and pad bits that
    // are not zero (non-canonical encodings of the same bytes).
    static const char *const bad[] = {"Zm8",  "Zg=",  "Zg",   "Zm=8", "Z===",     "=Zm8", "=Zm9",      "Zg==Zg==",
                                      "Zh==", "Zm9=", "QR==", "QUJ=", "Zm9v YmFy", "Zm 9v", "Zm9v\nYmFy", "Zm9-",
                                      "Zm9_", "Zm9v!A=="};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        uint8_t out[16];
        CHECK(b64_decode(bad[i], strlen(bad[i]), out, sizeof(out)) == B64_INVALID, "rejects \"%s\"", bad[i]);
        CHECK(b64_decode(bad[i], strlen(bad[i]), NULL, sizeof(out)) == B64_INVALID, "validates \"%s\"", bad[i]);
    }
    uint8_t out[8];
    CHECK(b64_decode("Zm9vYmFy", 8, out, 5) == B64_TOO_LONG, "longer than the buffer");
    CHECK(b64_decode("Zm9vYmFy", 8, out, 6) == 6, "exactly the buffer");
    CHECK(b64_decode("Zm9vYmFy", 8, NULL, 6) == 6, "counts without a buffer");
    CHECK(b64_decode("", 0, out, 0) == 0, "empty");
}

// ---- JSON text ---------------------------------------------------------------------------------

static void test_json_escaping(void)
{
    jbuf_t b = {0};
    jbuf_json_str(&b, "a\"b\\c\n\r\t\x01\x7f");
    CHECK(b.p && strcmp(b.p, "\"a\\\"b\\\\c\\n\\r\\t\\u0001\x7f\"") == 0, "escapes: %s", b.p);
    jbuf_free(&b);

    // Valid UTF-8 passes; invalid bytes, overlong forms, surrogates and cut sequences become U+FFFD.
    jbuf_json(&b, "\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80", 9);
    CHECK(b.p && strcmp(b.p, "\"\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80\"") == 0, "UTF-8 kept");
    jbuf_free(&b);
    jbuf_json(&b, "\xFF\xC0\xAF\xED\xA0\x80\xE2\x82", 8);
    CHECK(b.p && strcmp(b.p, "\"\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD"
                             "\xEF\xBF\xBD\"") == 0, "invalid UTF-8 replaced: %s", b.p);
    jbuf_free(&b);
    jbuf_json(&b, "x\0y", 3);
    CHECK(b.p && strcmp(b.p, "\"x\\u0000y\"") == 0, "embedded NUL: %s", b.p);
    jbuf_free(&b);
}

static void test_json_limits(void)
{
    jbuf_t b = {.limit = 10};
    jbuf_str(&b, "0123456789");
    CHECK(!b.full && !b.oom && b.len == 10 && b.cap == 11, "fills the limit exactly (cap %zu)", b.cap);
    jbuf_str(&b, "x");
    CHECK(b.full && !b.oom && b.len == 10 && strcmp(b.p, "0123456789") == 0, "past the limit: left out, marked full");
    jbuf_free(&b);

    jbuf_t m = {.measure = true};
    jbuf_str(&m, "{\"a\":");
    jbuf_json_str(&m, "q\"");
    CHECK(m.p == NULL && m.len == strlen("{\"a\":\"q\\\"\""), "measuring counts without storing (%zu)", m.len);
}

static void test_error_frames(void)
{
    char *f = json_error_frame("req_1", "bad \"thing\"");
    CHECK(f && strcmp(f, "{\"id\":\"req_1\",\"type\":\"error\",\"error\":\"bad \\\"thing\\\"\"}") == 0, "error frame: %s", f);
    char fixed[96];
    const size_t n = json_error_frame_into(fixed, sizeof(fixed), "req_1", "bad \"thing\"");
    CHECK(n == strlen(f) && strcmp(fixed, f) == 0, "the same frame without allocating");
    free(f);
    CHECK(json_error_frame_into(fixed, 20, "req_1", "out of memory") == 0, "a frame that does not fit is not sent");
    CHECK(json_error_frame_into(fixed, 1, "req_1", "x") == 0, "a one-byte buffer holds nothing");
}

// ---- WebSocket frames --------------------------------------------------------------------------

static ws_frame_result_t feed(ws_frame_t *f, uint8_t op, const char *data, int len, int total, int offset, bool fin)
{
    const esp_websocket_event_data_t e = {
        .data_ptr = data, .data_len = len, .fin = fin, .op_code = op, .payload_len = total, .payload_offset = offset};
    return ws_frame_feed(f, &e);
}

static void test_ws_frames(void)
{
    uint8_t buf[17];
    ws_frame_t f = {.buf = buf, .capacity = 16};

    CHECK(feed(&f, WS_OP_TEXT, "hello", 5, 5, 0, true) == WS_FRAME_DONE && f.opcode == WS_OP_TEXT && f.len == 5 &&
              memcmp(buf, "hello", 5) == 0, "one-piece frame");

    CHECK(feed(&f, WS_OP_TEXT, "0123456", 7, 16, 0, true) == WS_FRAME_PENDING, "first piece");
    CHECK(feed(&f, WS_OP_TEXT, "789abcdef", 9, 16, 7, true) == WS_FRAME_DONE && f.len == 16 &&
              memcmp(buf, "0123456789abcdef", 16) == 0, "pieces join at their offsets");

    CHECK(feed(&f, WS_OP_TEXT, "0123456", 7, 17, 0, true) == WS_FRAME_TOO_BIG && f.len == 17, "too big for the buffer");
    CHECK(feed(&f, WS_OP_TEXT, "789abcdefg", 10, 17, 7, true) == WS_FRAME_PENDING, "the rest of a dropped frame");

    CHECK(feed(&f, WS_OP_BIN, "abcd", 4, 8, 0, true) == WS_FRAME_PENDING, "start");
    CHECK(feed(&f, WS_OP_BIN, "ef", 2, 8, 6, true) == WS_FRAME_PENDING, "a piece out of order drops the frame");
    CHECK(feed(&f, WS_OP_BIN, "gh", 2, 8, 4, true) == WS_FRAME_PENDING, "nothing more of it completes");

    CHECK(feed(&f, WS_OP_TEXT, "abc", 3, 3, 0, false) == WS_FRAME_PENDING, "a fragmented message is never joined");

    // The client reports an empty PONG (an empty PING's answer) as a zero-length piece.
    CHECK(feed(&f, WS_OP_PONG, "", 0, 0, 0, true) == WS_FRAME_CONTROL, "empty PONG");
    CHECK(feed(&f, WS_OP_PING, "x", 1, 1, 0, true) == WS_FRAME_CONTROL, "PING");
    CHECK(feed(&f, WS_OP_TEXT, "", 0, 0, 0, true) == WS_FRAME_PENDING, "an empty data frame carries nothing");
    CHECK(ws_frame_feed(&f, NULL) == WS_FRAME_PENDING, "no event");

    CHECK(feed(&f, WS_OP_CLOSE, "\x03\xe8", 2, 2, 0, true) == WS_FRAME_DONE && f.opcode == WS_OP_CLOSE &&
              ((buf[0] << 8) | buf[1]) == 1000, "close code");

    CHECK(feed(&f, WS_OP_TEXT, "abc", 3, 6, 0, true) == WS_FRAME_PENDING, "half a frame");
    ws_frame_reset(&f);
    CHECK(feed(&f, WS_OP_TEXT, "def", 3, 6, 3, true) == WS_FRAME_PENDING, "a new connection forgets it");
}

int main(void)
{
    test_b64_vectors();
    test_b64_round_trip();
    test_b64_strict();
    test_json_escaping();
    test_json_limits();
    test_error_frames();
    test_ws_frames();
    printf("codec: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
