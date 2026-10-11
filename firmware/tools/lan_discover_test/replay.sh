#!/bin/sh
# Replay captured datagrams through the parser (see replay.c). Usage: ./replay.sh self_ip netmask files...
set -e
cd "$(dirname "$0")"
OUT=${TMPDIR:-/tmp}/lan_discover_replay
cc -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -g -fsanitize=address,undefined \
    -I../lan_http_test/stubs -I../../main replay.c -o "$OUT"
"$OUT" "$@"
