#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu
cd "$(dirname "$0")"
OUT=${TMPDIR:-/tmp}/wifi_scan_test
cc -std=gnu11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    -I../wifi_switch_test/stubs -I../../main \
    test.c -o "$OUT"
"$OUT"
