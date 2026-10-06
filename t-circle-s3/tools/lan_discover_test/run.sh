#!/bin/sh
# Host test for main/lan_discover_core.c. Needs cc; reuses lan_http_test's stub ESP headers and the
# cJSON sources from managed_components (run one idf.py build first).
set -e
cd "$(dirname "$0")"
CJSON=../../managed_components/espressif__cjson/cJSON
OUT=${TMPDIR:-/tmp}/lan_discover_test
cc -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -g -fsanitize=address,undefined \
    -I../lan_http_test/stubs -I../../main -I"$CJSON" test.c "$CJSON/cJSON.c" -o "$OUT"
"$OUT"
