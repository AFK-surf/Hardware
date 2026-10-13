// SPDX-License-Identifier: Apache-2.0
#include "resample.h"

#include <math.h>
#include <string.h>

#define K RESAMPLE_TAPS_PER_PHASE
#define L (2 * K)            // taps at 48 kHz
#define FS_UP 48000.0
#define CUTOFF_HZ 6900.0
#define KAISER_BETA 6.5      // ~65 dB stopband; with 96 taps the transition is about 2 kHz

// Modified Bessel function of the first kind, order 0, for the Kaiser window.
static double bessel_i0(double x)
{
    double sum = 1.0;
    double term = 1.0;
    for (int k = 1; k < 40; ++k) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
    }
    return sum;
}

void resample_3to2_init(resample_3to2_t *rs)
{
    const double pi = 3.14159265358979323846;
    const double center = (L - 1) / 2.0;
    const double fc = CUTOFF_HZ / FS_UP;  // cycles per 48 kHz sample
    double h[L];
    for (int j = 0; j < L; ++j) {
        const double t = j - center;
        const double sinc = t == 0.0 ? 2.0 * fc : sin(2.0 * pi * fc * t) / (pi * t);
        const double r = t / center;
        h[j] = sinc * bessel_i0(KAISER_BETA * sqrt(1.0 - r * r)) / bessel_i0(KAISER_BETA);
    }
    // Phase p uses taps p, p + 2, ...; each phase sums to exactly 1.0 (32768) so DC passes unchanged.
    for (int p = 0; p < 2; ++p) {
        double sum = 0.0;
        for (int k = 0; k < K; ++k) {
            sum += h[2 * k + p];
        }
        int total = 0;
        for (int k = 0; k < K; ++k) {
            const long q = lround(h[2 * k + p] / sum * 32768.0);
            rs->taps[p][k] = (int16_t)q;
            total += (int)q;
        }
        rs->taps[p][K / 2] = (int16_t)(rs->taps[p][K / 2] + (32768 - total));  // rounding error to the peak
    }
    resample_3to2_reset(rs);
}

void resample_3to2_reset(resample_3to2_t *rs)
{
    memset(rs->work, 0, sizeof(rs->work));
}

int resample_3to2(resample_3to2_t *rs, const int16_t *in, int n_in, int16_t *out)
{
    if (n_in < 0 || n_in > RESAMPLE_MAX_IN || n_in % 3 != 0 || (n_in > 0 && (!in || !out))) {
        return -1;
    }
    int16_t *const x = rs->work + (K - 1);  // x[-(K-1)] .. x[-1] is the history
    memcpy(x, in, (size_t)n_in * sizeof(int16_t));
    const int n_out = n_in / 3 * 2;
    for (int i = 0; i < n_out; ++i) {
        // Output i sits at input time 1.5 * i: an even output on a sample (phase 0), an odd one
        // halfway between two (phase 1).
        const int p = i & 1;
        const int base = (3 * i - p) / 2;
        const int16_t *tap = rs->taps[p];
        int32_t acc = 1 << 14;
        for (int k = 0; k < K; ++k) {
            acc += (int32_t)tap[k] * x[base - k];
        }
        acc >>= 15;
        out[i] = (int16_t)(acc > INT16_MAX ? INT16_MAX : acc < INT16_MIN ? INT16_MIN : acc);
    }
    memmove(rs->work, rs->work + n_in, (K - 1) * sizeof(int16_t));
    return n_out;
}
