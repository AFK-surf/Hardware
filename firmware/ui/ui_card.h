#pragma once

// Text cards for the round screen: a small gray label, an optional line of big digits, and
// wrapped body text, centred as a block. Pure C, so tools/card_preview.c can render it on a host.
// The screen is UI_CARD_SIZE pixels square (160 on the T-Circle, 466 on the Waveshare AMOLED);
// fonts for that size come from ui/<size>/ui_font.c.

#include <stdint.h>

#ifndef UI_CARD_SIZE
#include "sdkconfig.h"
#define UI_CARD_SIZE CONFIG_COMMA_DISPLAY_SIZE
#endif

typedef struct {
    char label[64];
    char big[16];   // digits, spaces and ':' only
    char body[192];
} ui_card_t;

// Draw the card over a black background. lut maps 16 gray levels to panel pixels.
void ui_card_render(const ui_card_t *card, uint16_t *fb, const uint16_t lut[16]);
