#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu
cd "$(dirname "$0")"
OUT=${TMPDIR:-/tmp}/device_config_test
cc -std=gnu11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
    -Istubs -I../lan_http_test/stubs -I../../main test.c -lpthread -o "$OUT"
"$OUT"
