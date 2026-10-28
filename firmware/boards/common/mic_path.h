// SPDX-License-Identifier: Apache-2.0
#pragma once

// Mic path for boards whose codec shares one stereo I2S port with the speaker at 24 kHz (StickS3,
// Waveshare AMOLED 1.43C): one slot out of the stereo frames, then 24 -> 16 kHz.
// Pure C, so tools/mic_path_test can check it on a host.
//
// The I2S port runs in stereo slot mode: every frame is two samples, left then right, and the mic
// used is on one slot. Taking that slot gives exactly one sample per frame, so the mic rate is the
// I2S rate. (Reading the ES8311 in mono slot mode once gave ~1.9x real time.)

#include <stdint.h>

#include "resample.h"

// Stereo frames per call, at most: one 20 ms frame at 24 kHz.
#define MIC_PATH_MAX_FRAMES RESAMPLE_MAX_IN

typedef struct {
    resample_3to2_t rs;
    int16_t slot24[MIC_PATH_MAX_FRAMES];
} mic_path_t;

// Copy one slot (0 = left, 1 = right) of `frames` interleaved stereo frames. Returns frames.
int mic_take_slot(const int16_t *stereo, int frames, int slot, int16_t *out);

void mic_path_init(mic_path_t *mp);
// Start of a new recording: forget the resampler's history.
void mic_path_reset(mic_path_t *mp);

// stereo: `frames` interleaved frames at 24 kHz, a multiple of 3, at most MIC_PATH_MAX_FRAMES.
// Writes frames * 2 / 3 samples of `slot` at 16 kHz and returns that count, or -1.
int mic_path_24k_to_16k(mic_path_t *mp, const int16_t *stereo, int frames, int slot, int16_t *out);
