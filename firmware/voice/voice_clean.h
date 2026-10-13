// SPDX-License-Identifier: Apache-2.0
#pragma once

// Noise suppression and voice activity detection on the mic, so the uplink carries speech, not
// room noise (the voice model's server-side turn detection kept hearing office noise as "still talking").
//
// ESP-SR AFE (voice-communication type, one mic): vadnet1 VAD, optionally after nsnet2 noise
// suppression (COMMA_VOICE_NS_OFF), models in the "model" partition. The mic runs at 16 kHz, the
// AFE's rate; its output (raw, or denoised) is upsampled 2:3 to the wire's 24 kHz. comma_voice decides
// what reaches the wire (pre-roll, hangover, zeros).

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

// 1: mic at 16 kHz through the AFE (default). 0: no AFE, raw 24 kHz mic audio on the wire.
// Build without it: `idf.py -B build-clean-off -DCOMMA_VOICE_CLEAN_OFF=1 build` (separate build directory:
// the CMake cache keeps the value).
#ifndef COMMA_VOICE_CLEAN
#define COMMA_VOICE_CLEAN 1
#endif

// 1 (default): VAD only. vadnet runs on the raw 16 kHz mic and the wire carries that raw audio in
// speech, zeros otherwise; nsnet2 cost ~0.8 s of CPU per second in a call (measured on the T-Circle). 0: nsnet2
// denoises first, and the wire carries the denoised audio. `-DCOMMA_VOICE_NS_OFF=0` at configure time.
#ifndef COMMA_VOICE_NS_OFF
#define COMMA_VOICE_NS_OFF 1
#endif

#define VOICE_CLEAN_RATE_HZ 16000
#define VOICE_CLEAN_FRAME_SAMPLES (VOICE_CLEAN_RATE_HZ * 20 / 1000)

// Load the models and create the AFE. Once, at boot; it stays for every call.
esp_err_t voice_clean_init(void);

// Start of a call: drop audio and state left from the last one.
void voice_clean_reset(void);

// Feed one 20 ms mic frame (320 samples at 16 kHz).
void voice_clean_feed(const int16_t *mic16k);

// Take the next 20 ms frame of AFE output (denoised only with nsnet2 on), at 24 kHz (480 samples), with the VAD's verdict for it.
// The AFE works in 32 ms chunks, so a feed yields zero, one or two frames: call until false.
bool voice_clean_pop(int16_t *out24k, bool *speech);

// Microseconds spent in the AFE since the last call (CPU use for the stats line).
int64_t voice_clean_take_busy_us(void);

#ifdef VOICE_CLEAN_BENCH
void voice_clean_bench(void);
#endif
