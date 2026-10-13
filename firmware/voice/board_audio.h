// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define AUDIO_SAMPLE_RATE_HZ 24000
#define AUDIO_FRAME_MS       20
#define AUDIO_FRAME_SAMPLES  (AUDIO_SAMPLE_RATE_HZ * AUDIO_FRAME_MS / 1000)

_Static_assert(AUDIO_FRAME_SAMPLES == 480, "20 ms at 24 kHz is 480 samples");
_Static_assert(AUDIO_FRAME_SAMPLES * (int)sizeof(int16_t) == 960, "one uplink frame is 960 bytes");

// The mic runs at the speaker's 24 kHz, or at 16 kHz for the noise suppressor (voice_clean.h).
#include "voice_clean.h"
#if COMMA_VOICE_CLEAN
#define MIC_SAMPLE_RATE_HZ VOICE_CLEAN_RATE_HZ
#else
#define MIC_SAMPLE_RATE_HZ AUDIO_SAMPLE_RATE_HZ
#endif
#define MIC_FRAME_SAMPLES (MIC_SAMPLE_RATE_HZ * AUDIO_FRAME_MS / 1000)

// What each voice board implements (boards/<board>/). The rates are the voice pipeline's, not the
// hardware's: a board whose codec runs at another rate resamples in its own layer.
esp_err_t board_audio_init(void);
esp_err_t board_audio_set_speaker_enabled(bool enabled);
esp_err_t board_audio_set_recording(bool enabled);

// Read up to MIC_FRAME_SAMPLES mono PCM16 samples at MIC_SAMPLE_RATE_HZ. Returns the count, or -1.
int board_audio_read_mono(int16_t *mono_buf, int samples);

// Play mono PCM16. The amplifier is enabled on the first write.
esp_err_t board_audio_play_pcm16_mono(const int16_t *pcm, int samples);
