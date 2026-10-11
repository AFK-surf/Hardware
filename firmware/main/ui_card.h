#pragma once

// Text cards for the round 160x160 screen: a small gray label, an optional line of big digits,
// and wrapped body text, centred as a block. Pure C, so tools/card_preview.c can render it on a
// host.

#include <stdint.h>

#define UI_CARD_SIZE 160

typedef struct {
    char label[64];
    char big[16];   // digits, spaces and ':' only
    char body[192];
} ui_card_t;

// Draw the card over a black background. lut maps 16 gray levels to panel pixels.
void ui_card_render(const ui_card_t *card, uint16_t *fb, const uint16_t lut[16]);
