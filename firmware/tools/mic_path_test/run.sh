#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# The ES8311/ES7210 boards' mic path (boards/common/mic_path.c): one slot of the stereo I2S frames,
# then 24 -> 16 kHz. Checks that the slot is the only input and that sample counts match the clock.
set -e
cd "$(dirname "$0")"
OUT=${TMPDIR:-/tmp}/mic_path_test
cc -std=gnu11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -I../../voice -I../../boards/common test.c ../../boards/common/mic_path.c \
    ../../voice/resample.c -lm -o "$OUT"
"$OUT"
