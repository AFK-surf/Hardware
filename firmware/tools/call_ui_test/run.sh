#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu
cd "$(dirname "$0")"
OUT=${TMPDIR:-/tmp}/call_ui_test
cc -std=gnu11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    -Istubs -I../device_config_test/stubs -I../lan_http_test/stubs -I../../main -I../../voice -I../../ui \
    test.c -o "$OUT"
"$OUT"
