#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Host test for components/lan_script: generates the script stdlib for this host, then runs test.c with
# selftest.js. Needs cc and OpenSSL 3 (Homebrew's on macOS) for the host's crypto; the firmware's own
# crypto (lan_script_crypto_psa.c) is exercised on the board.
set -e
cd "$(dirname "$0")"
C=../../components/lan_script
Q=$C/mquickjs
OUT=${TMPDIR:-/tmp}/lan_script_test
SSL=${OPENSSL_PREFIX:-$(brew --prefix openssl@3 2>/dev/null || echo /usr)}
mkdir -p "$OUT"
cc -O2 -D_GNU_SOURCE -I"$Q" -o "$OUT/stdlib_gen" "$C/lan_script_stdlib.c" "$Q/mquickjs_build.c"
"$OUT/stdlib_gen" -a > "$OUT/mquickjs_atom.h"
"$OUT/stdlib_gen" > "$OUT/lan_script_stdlib.h"
cc -std=gnu11 -g -O1 -D_GNU_SOURCE -fno-math-errno -fsanitize=address,undefined -fno-sanitize=null,shift-base -Wall -Wno-unused-parameter \
    -I"$OUT" -I"$Q" -I"$C/include" -I"$SSL/include" \
    test.c "$C/lan_script.c" crypto_openssl.c "$Q/mquickjs.c" "$Q/cutils.c" "$Q/dtoa.c" "$Q/libm.c" \
    -L"$SSL/lib" -lcrypto -lm -o "$OUT/test"
"$OUT/test" selftest.js
