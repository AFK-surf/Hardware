#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Host test for main/lan_discover_core.c. Needs cc; reuses lan_http_test's stub ESP headers and the
# cJSON sources from managed_components (run one idf.py build first).
set -e
cd "$(dirname "$0")"
if [ "${1:-}" = --no-probes ]; then
    COMMA_TEST_DEFINES=-DCONFIG_COMMA_LAN_DISCOVER_PROBES=0
else
    . ../host_board_flags.sh
fi
CJSON=../../managed_components/espressif__cjson/cJSON
OUT=${TMPDIR:-/tmp}/lan_discover_test
cc $COMMA_TEST_DEFINES -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-deprecated-declarations -g -fsanitize=address,undefined \
    -Istubs -I../lan_http_test/stubs -I../../main -I"$CJSON" test.c ../../main/b64.c ../../main/json_buf.c \
    "$CJSON/cJSON.c" -o "$OUT"
"$OUT"
cc $COMMA_TEST_DEFINES -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-deprecated-declarations -g \
    -fsanitize=address,undefined -Inetwork_stubs -Istubs -I../lan_http_test/stubs -I../../main -I"$CJSON" \
    network_test.c ../../main/lan_discover_core.c ../../main/b64.c ../../main/json_buf.c "$CJSON/cJSON.c" -o "${OUT}_network"
"${OUT}_network"
