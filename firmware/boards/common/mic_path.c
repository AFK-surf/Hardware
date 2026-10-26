// SPDX-License-Identifier: Apache-2.0
#include "mic_path.h"

#include <stddef.h>

int mic_take_slot(const int16_t *stereo, int frames, int slot, int16_t *out)
{
    for (int i = 0; i < frames; ++i) {
        out[i] = stereo[2 * i + slot];
    }
    return frames;
}

void mic_path_init(mic_path_t *mp)
{
    resample_3to2_init(&mp->rs);
}

void mic_path_reset(mic_path_t *mp)
{
    resample_3to2_reset(&mp->rs);
}

int mic_path_24k_to_16k(mic_path_t *mp, const int16_t *stereo, int frames, int slot, int16_t *out)
{
    if (!mp || !stereo || !out || frames <= 0 || frames > MIC_PATH_MAX_FRAMES || frames % 3 != 0 ||
        (slot != 0 && slot != 1)) {
        return -1;
    }
    mic_take_slot(stereo, frames, slot, mp->slot24);
    return resample_3to2(&mp->rs, mp->slot24, frames, out);
}
