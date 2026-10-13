// SPDX-License-Identifier: Apache-2.0
#pragma once

// Text cards for the screen: a small gray label, an optional line of big digits, and wrapped body
// text, centred as a block. Pure C, so tools/card_preview.c can render it on a host.
// The screen is UI_CARD_W x UI_CARD_H pixels. A round screen is UI_CARD_SIZE square (160 on the
// T-Circle, 466 on the Waveshare AMOLED) and lines wrap to the glass; a rectangular one (135 x 240
// on the StickS3) wraps to its width. Fonts for the screen come from ui/<size>/ui_font.c, where
// <size> is UI_CARD_SIZE or WxH.

#include <stdint.h>

#if !defined(UI_CARD_SIZE) && !defined(UI_CARD_W)
#include "sdkconfig.h"
#ifdef CONFIG_COMMA_DISPLAY_WIDTH
#define UI_CARD_W CONFIG_COMMA_DISPLAY_WIDTH
#define UI_CARD_H CONFIG_COMMA_DISPLAY_HEIGHT
#define UI_CARD_ROUND 0
#else
#define UI_CARD_SIZE CONFIG_COMMA_DISPLAY_SIZE
#endif
#endif
#ifndef UI_CARD_W
#define UI_CARD_W UI_CARD_SIZE
#define UI_CARD_H UI_CARD_SIZE
#define UI_CARD_ROUND 1
#endif

typedef struct {
    char label[64];
    char big[16];   // digits, spaces and ':' only
    char body[192];
} ui_card_t;

// Draw the card over a black background. lut maps 16 gray levels to panel pixels.
void ui_card_render(const ui_card_t *card, uint16_t *fb, const uint16_t lut[16]);
