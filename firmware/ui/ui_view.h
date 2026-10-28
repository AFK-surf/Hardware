// SPDX-License-Identifier: Apache-2.0
#pragma once

// Screens made of rounded boxes and one-line labels, for the on-screen settings (ui/settings.c).
// The owner of the screen builds a view and hands it to display_show_view(); display.c draws it
// like a card. Pure C, like ui_card.c, so it can be rendered on a host.

#include <stdint.h>

#include "ui_card.h"  // UI_CARD_W, UI_CARD_H

#define UI_VIEW_ITEMS 56
#define UI_VIEW_TEXT 40

typedef enum { UI_VIEW_FONT_SMALL = 0, UI_VIEW_FONT_MENU } ui_view_font_t;
typedef enum { UI_VIEW_CENTER = 0, UI_VIEW_LEFT, UI_VIEW_RIGHT } ui_view_align_t;

typedef struct {
    int16_t x, y, w, h;      // the box; its label is laid out inside it
    uint8_t fill;            // gray level (0-15) of the box; 0: no box
    uint8_t level;           // gray level of the label
    uint8_t font;            // ui_view_font_t
    uint8_t align;           // ui_view_align_t
    uint8_t radius;          // corner radius of the box
    char text[UI_VIEW_TEXT]; // one line, cut with "..." when too wide; non-ASCII shows as '?'
} ui_view_item_t;

// Items are drawn in order. A label without a box blends into the box drawn under its centre.
typedef struct ui_view {
    uint8_t count;
    ui_view_item_t items[UI_VIEW_ITEMS];
} ui_view_t;

// Draw the view over a black background. lut maps 16 gray levels to panel pixels.
void ui_view_render(const ui_view_t *view, uint16_t *fb, const uint16_t lut[16]);

// Width in pixels of text in a ui_view_font_t, for layout.
int ui_view_text_width(uint8_t font, const char *text);
