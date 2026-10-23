#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Host test for the shared encoders: main/b64.c, main/json_buf.c and main/ws_frame.c.
set -e
cd "$(dirname "$0")"
OUT=${TMPDIR:-/tmp}/codec_test
cc -std=gnu11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -Istubs -I../lan_http_test/stubs -I../../main \
    test.c ../../main/b64.c ../../main/json_buf.c ../../main/ws_frame.c -o "$OUT"
"$OUT"
