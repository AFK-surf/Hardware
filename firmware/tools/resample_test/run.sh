#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# The 24 -> 16 kHz mic resampler (voice/resample.c): response, chunking and saturation.
set -e
cd "$(dirname "$0")"
OUT=${TMPDIR:-/tmp}/resample_test
cc -std=gnu11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -I../../voice test.c ../../voice/resample.c -lm -o "$OUT"
"$OUT"
