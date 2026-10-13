#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Host test for main/lan_udp.c (and the shared lan_dest.c). Needs cc; uses lan_http_test's stub ESP
# headers and the cJSON sources from managed_components (run one idf.py build first).
set -e
cd "$(dirname "$0")"
. ../host_board_flags.sh
CJSON=../../managed_components/espressif__cjson/cJSON
OUT=${TMPDIR:-/tmp}/lan_udp_test
cc $COMMA_TEST_DEFINES -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-deprecated-declarations -g \
    -fsanitize=address,undefined -Istubs -I../lan_http_test/stubs -I../../main -I"$CJSON" \
    test.c ../../main/lan_discover_core.c "$CJSON/cJSON.c" -o "$OUT"
"$OUT"
