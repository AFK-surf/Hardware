#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
set -eu
cd "$(dirname "$0")"
OUT=${TMPDIR:-/tmp}/speaker_volume_test
cc -std=gnu11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -I../../voice test.c -o "$OUT" -lm
"$OUT"
