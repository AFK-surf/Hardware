#include "mic_path.h"

#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { ++checks; if (!(c)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static int checks;

#define I2S_RATE 24000
#define MIC_RATE 16000
#define SECONDS 10
#define FRAMES (I2S_RATE * SECONDS)
#define OUT_MAX (FRAMES * 2 / 3)

static int16_t stereo[FRAMES * 2];
static int16_t left[FRAMES];
static int16_t right[FRAMES];
static int16_t out[OUT_MAX];
static int16_t ref[OUT_MAX];
static mic_path_t mp;
static resample_3to2_t rs;

// Speech-band test signal on one slot, something unrelated (and loud) on the other.
static void fill(void)
{
    uint32_t noise = 12345;
    for (int i = 0; i < FRAMES; ++i) {
        left[i] = (int16_t)lround(9000.0 * sin(2.0 * M_PI * 440.0 * i / I2S_RATE) +
                                  4000.0 * sin(2.0 * M_PI * 3100.0 * i / I2S_RATE));
        noise = noise * 1103515245u + 12345u;
        right[i] = (int16_t)(noise >> 16);
        stereo[2 * i] = left[i];
        stereo[2 * i + 1] = right[i];
    }
}

// Feed `frames` stereo frames in chunks of `chunk` (a multiple of 3); returns samples out.
static int run(int frames, int chunk, int slot, int16_t *dst)
{
    mic_path_reset(&mp);
    int got = 0;
    for (int at = 0; at < frames; at += chunk) {
        const int n = frames - at < chunk ? frames - at : chunk;
        const int r = mic_path_24k_to_16k(&mp, stereo + 2 * at, n, slot, dst + got);
        CHECK(r == n * 2 / 3);
        got += r;
    }
    return got;
}

// The same slot, de-interleaved first, through the bare resampler.
static int reference(const int16_t *mono, int frames, int16_t *dst)
{
    resample_3to2_reset(&rs);
    int got = 0;
    for (int at = 0; at < frames; at += RESAMPLE_MAX_IN) {
        const int n = frames - at < RESAMPLE_MAX_IN ? frames - at : RESAMPLE_MAX_IN;
        got += resample_3to2(&rs, mono + at, n, dst + got);
    }
    return got;
}

int main(void)
{
    mic_path_init(&mp);
    resample_3to2_init(&rs);
    fill();

    // Counts: one I2S second gives exactly MIC_RATE samples, ten seconds ten times that, whatever
    // the read size (20 ms frames, 10 ms DMA blocks, odd multiples of 3). The rate line on the
    // board expects this (16000/s); the Waveshare's mono-mode reads gave twice the I2S rate.
    static const int chunks[] = {480, 240, 3, 9, 159, 477};
    CHECK(run(I2S_RATE, 480, 0, out) == MIC_RATE);
    for (size_t c = 0; c < sizeof(chunks) / sizeof(chunks[0]); ++c) {
        CHECK(run(FRAMES, chunks[c], 0, out) == MIC_RATE * SECONDS);
    }

    // The left slot, and only it: bit-identical to de-interleaving first, in every chunking.
    const int n_ref = reference(left, FRAMES, ref);
    CHECK(n_ref == MIC_RATE * SECONDS);
    for (size_t c = 0; c < sizeof(chunks) / sizeof(chunks[0]); ++c) {
        CHECK(run(FRAMES, chunks[c], 0, out) == n_ref);
        CHECK(memcmp(out, ref, sizeof(int16_t) * (size_t)n_ref) == 0);
    }
    // Slot 1 is the right slot.
    CHECK(reference(right, FRAMES, ref) == n_ref);
    CHECK(run(FRAMES, 480, 1, out) == n_ref);
    CHECK(memcmp(out, ref, sizeof(int16_t) * (size_t)n_ref) == 0);

    // Unity DC on the mic slot while the other slot sits at full scale.
    for (int i = 0; i < I2S_RATE; ++i) {
        stereo[2 * i] = 1000;
        stereo[2 * i + 1] = INT16_MIN;
    }
    CHECK(run(I2S_RATE, 480, 0, out) == MIC_RATE);
    for (int i = 100; i < MIC_RATE; ++i) {
        CHECK(out[i] == 1000);
    }

    // mic_take_slot on its own (the 24 kHz path with V2 off).
    const int16_t two[] = {1, -1, 2, -2, 3, -3};
    int16_t picked[3];
    CHECK(mic_take_slot(two, 3, 0, picked) == 3);
    CHECK(picked[0] == 1 && picked[1] == 2 && picked[2] == 3);
    CHECK(mic_take_slot(two, 3, 1, picked) == 3);
    CHECK(picked[0] == -1 && picked[1] == -2 && picked[2] == -3);

    // Bad arguments.
    CHECK(mic_path_24k_to_16k(&mp, stereo, 0, 0, out) == -1);
    CHECK(mic_path_24k_to_16k(&mp, stereo, 4, 0, out) == -1);
    CHECK(mic_path_24k_to_16k(&mp, stereo, MIC_PATH_MAX_FRAMES + 3, 0, out) == -1);
    CHECK(mic_path_24k_to_16k(&mp, stereo, 3, 2, out) == -1);
    CHECK(mic_path_24k_to_16k(&mp, stereo, 3, -1, out) == -1);
    CHECK(mic_path_24k_to_16k(NULL, stereo, 3, 0, out) == -1);

    printf("mic_path_test: %d checks, 0 failed\n", checks);
    return 0;
}
