// SPDX-License-Identifier: Apache-2.0
#include "resample.h"

#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { ++checks; if (!(c)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static int checks;
static resample_3to2_t rs;

#define IN_RATE 24000.0
#define SECONDS 1
#define N_IN (24000 * SECONDS)
#define N_OUT (N_IN * 2 / 3)

static int16_t in[N_IN];
static int16_t out[N_OUT];
static int16_t out2[N_OUT];

static void sine(double hz, double amplitude)
{
    for (int i = 0; i < N_IN; ++i) {
        in[i] = (int16_t)lround(amplitude * sin(2.0 * M_PI * hz * i / IN_RATE));
    }
}

// Feed all of `in` in chunks of `chunk` samples (a multiple of 3).
static void run(int chunk, int16_t *dst)
{
    resample_3to2_reset(&rs);
    int got = 0;
    for (int at = 0; at < N_IN; at += chunk) {
        const int n = N_IN - at < chunk ? N_IN - at : chunk;
        const int r = resample_3to2(&rs, in + at, n, dst + got);
        CHECK(r == n / 3 * 2);
        got += r;
    }
    CHECK(got == N_OUT);
}

// RMS of the output after the filter has settled.
static double rms_settled(const int16_t *x)
{
    double sum = 0;
    int n = 0;
    for (int i = 1000; i < N_OUT; ++i, ++n) {
        sum += (double)x[i] * x[i];
    }
    return sqrt(sum / n);
}

static double gain_db(double hz)
{
    const double amplitude = 10000.0;
    sine(hz, amplitude);
    run(RESAMPLE_MAX_IN, out);
    return 20.0 * log10(rms_settled(out) / (amplitude / sqrt(2.0)) + 1e-12);
}

int main(void)
{
    resample_3to2_init(&rs);

    // Each phase sums to unity, and the worst-case accumulator stays inside int32.
    for (int p = 0; p < 2; ++p) {
        int sum = 0;
        long abs_sum = 0;
        for (int k = 0; k < RESAMPLE_TAPS_PER_PHASE; ++k) {
            sum += rs.taps[p][k];
            abs_sum += labs((long)rs.taps[p][k]);
        }
        CHECK(sum == 32768);
        CHECK(abs_sum * 32768L + (1L << 14) < 2147483647L);
    }

    // Bad lengths.
    CHECK(resample_3to2(&rs, in, 4, out) == -1);
    CHECK(resample_3to2(&rs, in, RESAMPLE_MAX_IN + 3, out) == -1);
    CHECK(resample_3to2(&rs, in, -3, out) == -1);
    CHECK(resample_3to2(&rs, in, 0, out) == 0);

    // DC passes unchanged once settled.
    for (int i = 0; i < N_IN; ++i) {
        in[i] = 1000;
    }
    run(RESAMPLE_MAX_IN, out);
    for (int i = 100; i < N_OUT; ++i) {
        CHECK(out[i] == 1000);
    }

    // Passband flat to 6 kHz, -6 dB near 7 kHz, at least 55 dB down from 8 kHz (what would alias).
    const double pass[] = {100, 300, 1000, 2000, 3000, 4000, 5000, 6000};
    for (size_t i = 0; i < sizeof(pass) / sizeof(pass[0]); ++i) {
        const double g = gain_db(pass[i]);
        printf("  %5.0f Hz: %+6.2f dB\n", pass[i], g);
        CHECK(fabs(g) < 0.2);
    }
    const double g7 = gain_db(7000);
    printf("  %5.0f Hz: %+6.2f dB\n", 7000.0, g7);
    CHECK(g7 < -3.0 && g7 > -9.0);
    const double stop[] = {8000, 8500, 9000, 10000, 11000, 11900};
    for (size_t i = 0; i < sizeof(stop) / sizeof(stop[0]); ++i) {
        const double g = gain_db(stop[i]);
        printf("  %5.0f Hz: %+6.2f dB (aliases to %.0f Hz)\n", stop[i], g, 16000 - stop[i]);
        CHECK(g < -55.0);
    }

    // Any chunking gives the same samples as one frame at a time.
    sine(1234.5, 12000);
    run(RESAMPLE_MAX_IN, out);
    const int chunks[] = {3, 6, 9, 30, 159, 240, 477};
    for (size_t c = 0; c < sizeof(chunks) / sizeof(chunks[0]); ++c) {
        run(chunks[c], out2);
        CHECK(memcmp(out, out2, sizeof(out)) == 0);
    }

    // A full-scale square wave saturates instead of wrapping: every output equals a 64-bit reference
    // with the same taps, rounded and clamped.
    for (int i = 0; i < N_IN; ++i) {
        in[i] = (i / 6) % 2 ? INT16_MIN : INT16_MAX;
    }
    run(RESAMPLE_MAX_IN, out);
    int clipped = 0;
    for (int i = 0; i < N_OUT; ++i) {
        const int p = i & 1;
        const int base = (3 * i - p) / 2;
        int64_t acc = 1 << 14;
        for (int k = 0; k < RESAMPLE_TAPS_PER_PHASE; ++k) {
            const int at = base - k;
            acc += (int64_t)rs.taps[p][k] * (at >= 0 ? in[at] : 0);
        }
        acc >>= 15;
        const int64_t want = acc > INT16_MAX ? INT16_MAX : acc < INT16_MIN ? INT16_MIN : acc;
        clipped += want != acc;
        CHECK(out[i] == want);
    }
    CHECK(clipped > 0);

    printf("%d checks, 0 failed\n", checks);
    return 0;
}
