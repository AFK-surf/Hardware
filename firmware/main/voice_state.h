// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include "sdkconfig.h"
#if CONFIG_COMMA_HAS_VOICE
#include "comma_voice.h"
#endif
static inline bool comma_voice_active(void)
{
#if CONFIG_COMMA_HAS_VOICE
    const comma_voice_state_t state = comma_voice_state();
    return state == COMMA_VOICE_UP || state == COMMA_VOICE_CONNECTING;
#else
    return false;
#endif
}
