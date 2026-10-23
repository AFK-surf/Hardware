// SPDX-License-Identifier: Apache-2.0
#include "b64.h"

static const char ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t b64_encode(const uint8_t *in, size_t n, char *out)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = ((uint32_t)in[i] << 16) | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = ALPHABET[(v >> 18) & 63];
        out[o++] = ALPHABET[(v >> 12) & 63];
        out[o++] = i + 1 < n ? ALPHABET[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? ALPHABET[v & 63] : '=';
    }
    return o;
}

static int value_of(char c)
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

long b64_decode(const char *in, size_t n, uint8_t *out, size_t cap)
{
    if (n % 4 != 0) {
        return B64_INVALID;
    }
    size_t o = 0;
    for (size_t i = 0; i < n; i += 4) {
        const int pad = (in[i + 3] == '=') + (in[i + 2] == '=');
        if (pad > 0 && (i + 4 != n || (in[i + 2] == '=' && in[i + 3] != '='))) {
            return B64_INVALID;
        }
        uint32_t w = 0;
        for (int k = 0; k < 4; ++k) {
            const int v = k < 4 - pad ? value_of(in[i + k]) : 0;
            if (v < 0) {
                return B64_INVALID;
            }
            w = (w << 6) | (uint32_t)v;
        }
        // Canonical only: the bits a padded group drops must be zero (like Go's strict decoding).
        if ((pad == 1 && (w & 0xFF)) || (pad == 2 && (w & 0xFFFF))) {
            return B64_INVALID;
        }
        const size_t take = 3 - (size_t)pad;
        if (o + take > cap) {
            return B64_TOO_LONG;
        }
        if (out) {
            out[o] = (uint8_t)(w >> 16);
            if (take > 1) {
                out[o + 1] = (uint8_t)(w >> 8);
            }
            if (take > 2) {
                out[o + 2] = (uint8_t)w;
            }
        }
        o += take;
    }
    return (long)o;
}
