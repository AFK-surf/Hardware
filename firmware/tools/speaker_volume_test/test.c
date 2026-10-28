// SPDX-License-Identifier: Apache-2.0
// Speaker volume levels (voice/speaker_volume.h): 3 dB steps, the DAC never above 0 dB, the boost
// only above the clean maximum and matching its dB value, out-of-range levels clamped.
#include <assert.h>
#include <math.h>
#include <stdio.h>

#include "speaker_volume.h"

static int checks;
#define CHECK(c) do { ++checks; assert(c); } while (0)

int main(void)
{
    int previous = -1000;
    for (int level = SPEAKER_VOLUME_MIN; level <= SPEAKER_VOLUME_MAX; ++level) {
        const speaker_volume_t g = speaker_volume_gain(level);
        const int total = g.dac_db + g.boost_db;
        CHECK(g.dac_db <= 0 && g.boost_db >= 0);
        CHECK(g.dac_db == 0 || g.boost_db == 0);  // one or the other, never both
        if (previous != -1000) {
            CHECK(total - previous == SPEAKER_VOLUME_STEP_DB);
        }
        previous = total;
        // The boost factor is the boost in dB, within 0.5 %.
        CHECK(fabs(g.boost_q8 / 256.0 - pow(10.0, g.boost_db / 20.0)) < 0.005 * pow(10.0, g.boost_db / 20.0));
        CHECK((level <= SPEAKER_VOLUME_CLEAN_MAX) == (g.boost_q8 == SPEAKER_BOOST_UNITY_Q8));
    }
    CHECK(speaker_volume_gain(SPEAKER_VOLUME_CLEAN_MAX).dac_db == 0);
    CHECK(speaker_volume_gain(SPEAKER_VOLUME_MAX).boost_db == 6);
    CHECK(speaker_volume_gain(SPEAKER_VOLUME_MIN).dac_db == -21);
    // The default stays within 1 dB of the -2.4 dB the 1.43C shipped with.
    CHECK(fabs(speaker_volume_gain(SPEAKER_VOLUME_DEFAULT).dac_db + 2.4) < 1.0);
    CHECK(speaker_volume_clamp(0) == SPEAKER_VOLUME_MIN && speaker_volume_clamp(99) == SPEAKER_VOLUME_MAX);
    CHECK(speaker_volume_clamp(5) == 5);
    // The boost scales samples, both signs, without overflow at full scale.
    CHECK(speaker_volume_boost(1000, 256) == 1000);
    CHECK(speaker_volume_boost(-32768, 511) == -65408);
    CHECK(speaker_volume_boost(32767, 362) == 46334);
    printf("speaker_volume: %d checks passed\n", checks);
    return 0;
}
