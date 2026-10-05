#pragma once

#include "esp_err.h"

typedef enum {
    CUE_VOICE_IDLE = 0,
    CUE_VOICE_CONNECTING,
    CUE_VOICE_UP,
    CUE_VOICE_ENDED,
    CUE_VOICE_FAILED,
} cue_voice_state_t;

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
