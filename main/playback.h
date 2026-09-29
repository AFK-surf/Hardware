#pragma once

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
