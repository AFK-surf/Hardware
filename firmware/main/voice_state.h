// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include "sdkconfig.h"
#if CONFIG_COMMA_HAS_VOICE
#include "cue_voice.h"
#endif
static inline bool comma_voice_active(void)
{
#if CONFIG_COMMA_HAS_VOICE
    const cue_voice_state_t state = cue_voice_state();
    return state == CUE_VOICE_UP || state == CUE_VOICE_CONNECTING;
#else
    return false;
#endif
}
