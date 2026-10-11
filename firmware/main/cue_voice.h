#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef enum {
    CUE_VOICE_IDLE = 0,
    CUE_VOICE_CONNECTING,
    CUE_VOICE_UP,
    CUE_VOICE_ENDED,
    CUE_VOICE_FAILED,
} cue_voice_state_t;

typedef enum {
    CUE_VOICE_END_NONE = 0,
    CUE_VOICE_END_NORMAL,   // caller tap, idle hangup, or the server ended it cleanly (close 1000)
    CUE_VOICE_END_DROPPED,  // any other close, a send stall, a lost connection, a failed dial
} cue_voice_end_t;

// Open one comma.voice.v1 session. Wi-Fi must already have an IP.
// A rejected key (close 4401) refuses later start calls until reboot.
esp_err_t cue_voice_start(void);

// New credentials were provisioned: allow calls again after a 4401.
void cue_voice_credentials_changed(void);

// Ask the session to send session.end. Safe to call when idle.
void cue_voice_end(void);

// True when a new session may be opened.
bool cue_voice_can_start(void);

cue_voice_state_t cue_voice_state(void);
int cue_voice_close_code(void);

// True from the moment a call is over (hung up, closed or lost) until its socket is torn down and
// a new call can start; cue_voice_state() may still read CONNECTING or UP meanwhile.
bool cue_voice_ending(void);

// How the last call ended, set as its hang-up tone starts. NONE from cue_voice_start() until then.
cue_voice_end_t cue_voice_last_end(void);
