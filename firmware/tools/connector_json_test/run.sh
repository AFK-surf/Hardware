#!/bin/sh
# Real cJSON parser with deterministic allocation failures and the IDF failure-hook stand-in.
set -e
cd "$(dirname "$0")"
CJSON=../../managed_components/espressif__cjson/cJSON
OUT=${TMPDIR:-/tmp}/connector_json_test
cc -std=gnu11 -Wall -Wextra -Werror -Wno-deprecated-declarations -g -fsanitize=address,undefined \
    -Istubs -I../lan_http_test/stubs -I../../main -I"$CJSON" \
    test.c ../../main/connector_json.c "$CJSON/cJSON.c" -lpthread -o "$OUT"
"$OUT"
