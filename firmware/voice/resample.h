#pragma once

// 24 kHz -> 16 kHz (3:2) for a board whose codec runs mic and speaker on one 24 kHz clock while the
// voice pipeline's mic is 16 kHz (voice_clean.h). Pure C, so tools/resample_test can check it on a
// host.
//
// Polyphase low-pass, designed at 48 kHz (24 kHz upsampled by 2) and read every third sample:
// Kaiser-windowed sinc, -6 dB at 6.9 kHz, flat (+-0.1 dB) to 6 kHz and at least 70 dB down from
// 8 kHz, so what lies above the new 8 kHz Nyquist does not fold back into the speech band. DC gain 1.

#include <stdint.h>

#define RESAMPLE_TAPS_PER_PHASE 48
// Input samples per call, at most (one 20 ms frame at 24 kHz).
#define RESAMPLE_MAX_IN 480

typedef struct {
    int16_t taps[2][RESAMPLE_TAPS_PER_PHASE];
    int16_t work[RESAMPLE_TAPS_PER_PHASE - 1 + RESAMPLE_MAX_IN];  // history, then this call's input
} resample_3to2_t;

// Build the filter and clear the history.
void resample_3to2_init(resample_3to2_t *rs);

// Start of a new recording: forget the history.
void resample_3to2_reset(resample_3to2_t *rs);

// in: n_in samples at 24 kHz, a multiple of 3 and at most RESAMPLE_MAX_IN. Writes n_in * 2 / 3
// samples at 16 kHz to out and returns that count (-1 on a bad n_in). Output is delayed by the
// filter, about 1 ms. Calls may use any chunk sizes: the result is the same.
int resample_3to2(resample_3to2_t *rs, const int16_t *in, int n_in, int16_t *out);
