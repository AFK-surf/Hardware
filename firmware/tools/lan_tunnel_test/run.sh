#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# Host test for main/lan_tunnel.c (lan_tcp_dial) and the shared lan_dest.c. Needs cc; uses
# lan_http_test's stub ESP headers and the cJSON sources from managed_components (run one idf.py
# build first). LAN_TUNNEL_IDLE_S is 2 here so the idle check doesn't take a minute.
set -e
cd "$(dirname "$0")"
CJSON=../../managed_components/espressif__cjson/cJSON
OUT=${TMPDIR:-/tmp}/lan_tunnel_test
cc -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-deprecated-declarations -g -fsanitize=address,undefined \
    -DLAN_TUNNEL_IDLE_S=2 -Istubs -I../lan_http_test/stubs -I../../main -I"$CJSON" \
    test.c ../../main/b64.c ../../main/json_buf.c "$CJSON/cJSON.c" -lpthread -o "$OUT"
"$OUT"
