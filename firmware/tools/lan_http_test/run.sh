#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Host test for main/lan_http.c. Needs cc and python3; no ESP-IDF build, but reads cJSON and
# http_parser sources from managed_components and $IDF_PATH (default: IDF 6.0.2 under ~/.espressif).
set -e
cd "$(dirname "$0")"
. ../host_board_flags.sh
IDF=${IDF_PATH:-$HOME/.espressif/v6.0.2/esp-idf}
CJSON=../../managed_components/espressif__cjson/cJSON
OUT=${TMPDIR:-/tmp}/lan_http_test
cc $COMMA_TEST_DEFINES -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-deprecated-declarations -g -fsanitize=address,undefined \
    -Istubs -I../../main -I"$CJSON" -I"$IDF/components/http_parser" \
    test.c ../../main/b64.c ../../main/json_buf.c "$CJSON/cJSON.c" "$IDF/components/http_parser/http_parser.c" -o "$OUT"
rm -f "$OUT.port"
python3 -u server.py > "$OUT.port" &
SERVER=$!
trap 'kill $SERVER 2>/dev/null' EXIT
while [ ! -s "$OUT.port" ]; do sleep 0.1; done
NO_PROXY='*' "$OUT" "$(cat "$OUT.port")"
