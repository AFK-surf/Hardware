// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void playback_init(void);

// Queue agent PCM16. Extra bytes are dropped if the 2 s buffer is full.
void playback_push(const uint8_t *pcm, size_t len);

// Mark the end of the audio queued so far.
void playback_mark(const char *name);

// Drop queued audio and answer every mark that had not been played.
void playback_clear(void);

// Copy played mark names out. Returns how many were copied.
int playback_take_played(char names[][80], int max_names);

// Queued audio in bytes and marks not yet played, for diagnostics.
void playback_stats(int *queued_bytes, int *pending_marks);

// Play the ringback until the agent's first speech arrives, or until turned off. Turning it on
// cuts a hang-up tone short.
void playback_ringback(bool on);

bool playback_ringing(void);

typedef enum {
    PLAYBACK_TONE_NONE = 0,
    PLAYBACK_TONE_NORMAL,   // one soft 200 ms tone: the call ended cleanly
    PLAYBACK_TONE_DROPPED,  // three short beeps: the call was lost
    PLAYBACK_TONE_CONNECTED,  // two rising notes: connected, go ahead and speak
} playback_tone_t;

// Stop the ringback, drop queued agent audio and play a tone once.
void playback_tone(playback_tone_t tone);

// True while the ringback or a tone plays, or queued audio that holds speech (not just silence) has yet to reach the speaker.
bool playback_speaking(void);

// Agent audio received since the last call: bytes and peak sample. Resets both.
void playback_take_rx(int *bytes, int *peak);
