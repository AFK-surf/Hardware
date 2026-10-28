// SPDX-License-Identifier: Apache-2.0
// Speaker volume levels for boards with a volume setting (the Waveshare 1.43C's on-screen settings
// and USB console). Pure arithmetic, tested on a host (tools/speaker_volume_test).
//
// Levels 1-10 are 3 dB apart. Up to SPEAKER_VOLUME_CLEAN_MAX the codec's DAC volume does the work,
// at most 0 dB: the speaker path's soft limiter already brings peaks close to full scale, so DAC
// gain above 0 dB would clip the loudest words. The levels above it add digital gain before the
// limiter instead (+3 and +6 dB): louder on average, with the peaks compressed rather than clipped.
#pragma once

#include <stdint.h>

#define SPEAKER_VOLUME_MIN 1
#define SPEAKER_VOLUME_MAX 10
#define SPEAKER_VOLUME_CLEAN_MAX 8
// -3 dB at the DAC, the closest level to the -2.4 dB the 1.43C shipped with.
#define SPEAKER_VOLUME_DEFAULT 7
#define SPEAKER_VOLUME_STEP_DB 3
#define SPEAKER_BOOST_UNITY_Q8 256

typedef struct {
    int8_t dac_db;     // DAC volume, 0 dB or lower
    int8_t boost_db;   // gain before the limiter, 0 dB or higher
    int16_t boost_q8;  // boost_db as a Q8 factor
} speaker_volume_t;

static inline int speaker_volume_clamp(int level)
{
    return level < SPEAKER_VOLUME_MIN ? SPEAKER_VOLUME_MIN : level > SPEAKER_VOLUME_MAX ? SPEAKER_VOLUME_MAX : level;
}

static inline speaker_volume_t speaker_volume_gain(int level)
{
    // 256 x 10^(dB / 20) for +3 and +6 dB.
    static const int16_t BOOST_Q8[SPEAKER_VOLUME_MAX - SPEAKER_VOLUME_CLEAN_MAX] = {362, 511};
    const int total_db = SPEAKER_VOLUME_STEP_DB * (speaker_volume_clamp(level) - SPEAKER_VOLUME_CLEAN_MAX);
    speaker_volume_t gain = {.dac_db = 0, .boost_db = 0, .boost_q8 = SPEAKER_BOOST_UNITY_Q8};
    if (total_db <= 0) {
        gain.dac_db = (int8_t)total_db;
    } else {
        gain.boost_db = (int8_t)total_db;
        gain.boost_q8 = BOOST_Q8[total_db / SPEAKER_VOLUME_STEP_DB - 1];
    }
    return gain;
}

// One sample through the boost: Q8 gain, kept in 32 bits for the limiter that follows.
static inline int32_t speaker_volume_boost(int32_t sample, int16_t boost_q8)
{
    return (int32_t)(((int64_t)sample * boost_q8) / SPEAKER_BOOST_UNITY_Q8);
}
