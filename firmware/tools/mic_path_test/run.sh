#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# The StickS3 mic path (boards/m5stack_sticks3/mic_path.c): one slot of the stereo I2S frames, then
# 24 -> 16 kHz. Checks that the slot is the only input and that sample counts match the clock.
set -e
cd "$(dirname "$0")"
OUT=${TMPDIR:-/tmp}/mic_path_test
cc -std=gnu11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -I../../voice -I../../boards/m5stack_sticks3 test.c ../../boards/m5stack_sticks3/mic_path.c \
    ../../voice/resample.c -lm -o "$OUT"
"$OUT"
