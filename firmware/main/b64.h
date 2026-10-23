// SPDX-License-Identifier: Apache-2.0
#pragma once

// Standard base64 with padding (RFC 4648 §4): every binary field of the Connector protocol
// (udp_request and udp_probes payloads, response chunks, tunnel data).

#include <stddef.h>
#include <stdint.h>

// Characters that n bytes encode to.
#define B64_ENCODED_LEN(n) (((n) + 2) / 3 * 4)

// Writes B64_ENCODED_LEN(n) characters to out, without a NUL. Returns that count.
size_t b64_encode(const uint8_t *in, size_t n, char *out);

#define B64_INVALID (-1)
#define B64_TOO_LONG (-2)

// Strict decoding of n characters: a multiple of 4, '=' only as padding at the end, zero pad bits,
// no whitespace. Returns the decoded byte count, B64_INVALID, or B64_TOO_LONG when the bytes would
// exceed cap. out may be NULL to validate and count only.
long b64_decode(const char *in, size_t n, uint8_t *out, size_t cap);
