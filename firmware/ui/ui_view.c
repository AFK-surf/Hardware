// SPDX-License-Identifier: Apache-2.0
#include "ui_view.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "ui_font.h"

// Space between a box's edge and a left- or right-aligned label, and around a centred one.
#define TEXT_PAD 20
#define CENTER_PAD 4

static const ui_font_t *font_of(uint8_t font)
{
    return font == UI_VIEW_FONT_MENU ? &ui_font_menu : &ui_font_small;
}

static const ui_glyph_t *glyph_for(const ui_font_t *font, char c)
{
    const char *at = c ? strchr(font->chars, c) : NULL;
    if (!at) {
        at = strchr(font->chars, '?');
    }
    return at ? &font->glyphs[at - font->chars] : NULL;
}

static int width_of(const ui_font_t *font, const char *text, size_t len)
{
    int w = 0;
    for (size_t i = 0; i < len; ++i) {
        const ui_glyph_t *g = glyph_for(font, text[i]);
        w += g ? g->advance : 0;
    }
    return w;
}

int ui_view_text_width(uint8_t font, const char *text)
{
    return width_of(font_of(font), text, strlen(text));
}

// Copy text with each non-ASCII code point shown as '?'.
static void ascii_fold(const char *in, char *out, size_t out_len)
{
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && n + 1 < out_len; ++p) {
        if (*p < 0x80) {
            out[n++] = (char)*p;
        } else if ((*p & 0xc0) != 0x80) {
            out[n++] = '?';
        }
    }
    out[n] = '\0';
}

// Fit text into max_w, cutting its end and adding "..." when it is too wide.
static void fit(const ui_font_t *font, char *text, int max_w)
{
    size_t len = strlen(text);
    if (width_of(font, text, len) <= max_w) {
        return;
    }
    const int dots = width_of(font, "...", 3);
    while (len > 0 && width_of(font, text, len) + dots > max_w) {
        --len;
    }
    if (len + 4 > UI_VIEW_TEXT) {
        len = UI_VIEW_TEXT - 4;
    }
    memcpy(text + len, "...", 4);
}

// Columns [*x0, *x1) of row py inside a rounded box.
static bool row_span(const ui_view_item_t *it, int r, int py, int *x0, int *x1)
{
    if (py < it->y || py >= it->y + it->h) {
        return false;
    }
    int inset = 0;
    float dy = 0;
    if (py < it->y + r) {
        dy = (float)(it->y + r - py) - 0.5f;
    } else if (py >= it->y + it->h - r) {
        dy = (float)(py - (it->y + it->h - r)) + 0.5f;
    }
    if (dy > 0) {
        inset = r - (int)lroundf(sqrtf(fmaxf(0.0f, (float)(r * r) - dy * dy)));
    }
    *x0 = it->x + inset;
    *x1 = it->x + it->w - inset;
    if (*x0 < 0) *x0 = 0;
    if (*x1 > UI_CARD_W) *x1 = UI_CARD_W;
    return *x1 > *x0;
}

static void fill_box(uint16_t *fb, const ui_view_item_t *it, uint16_t color)
{
    int r = it->radius;
    if (r * 2 > it->h) r = it->h / 2;
    if (r * 2 > it->w) r = it->w / 2;
    for (int py = it->y; py < it->y + it->h; ++py) {
        int x0, x1;
        if (py < 0 || py >= UI_CARD_H || !row_span(it, r, py, &x0, &x1)) {
            continue;
        }
        uint16_t *row = fb + py * UI_CARD_W;
        for (int px = x0; px < x1; ++px) {
            row[px] = color;
        }
    }
}

// The fill of the last box drawn before item i that holds its centre: what its label blends into.
static uint8_t background_of(const ui_view_t *view, int i)
{
    const ui_view_item_t *it = &view->items[i];
    if (it->fill) {
        return it->fill;
    }
    const int cx = it->x + it->w / 2, cy = it->y + it->h / 2;
    for (int j = i - 1; j >= 0; --j) {
        const ui_view_item_t *b = &view->items[j];
        if (b->fill && cx >= b->x && cx < b->x + b->w && cy >= b->y && cy < b->y + b->h) {
            return b->fill;
        }
    }
    return 0;
}

static void draw_glyph(uint16_t *fb, const uint16_t *lut, const ui_font_t *font, const ui_glyph_t *g, int x0, int y0,
                       int level, int bg)
{
    const int row_bytes = (g->w + 1) / 2;
    const uint8_t *src = font->bitmap + g->offset;
    for (int y = 0; y < g->h; ++y) {
        const int py = y0 + g->y + y;
        if (py < 0 || py >= UI_CARD_H) {
            continue;
        }
        for (int x = 0; x < g->w; ++x) {
            const int px = x0 + g->x + x;
            if (px < 0 || px >= UI_CARD_W) {
                continue;
            }
            const uint8_t b = src[y * row_bytes + x / 2];
            const int a = (x & 1) ? (b & 0x0f) : (b >> 4);
            if (a) {
                // Coverage blends the label's gray into the box's.
                const int v = bg * 15 + (level - bg) * a;
                fb[py * UI_CARD_W + px] = lut[(v + 7) / 15];
            }
        }
    }
}

static void draw_label(uint16_t *fb, const uint16_t *lut, const ui_view_item_t *it, int bg)
{
    const ui_font_t *font = font_of(it->font);
    char text[UI_VIEW_TEXT];
    ascii_fold(it->text, text, sizeof(text));
    // A centred label keeps a small margin each side; an aligned one its padding on that side.
    const int room = it->w - (it->align == UI_VIEW_CENTER ? 2 * CENTER_PAD : TEXT_PAD + CENTER_PAD);
    fit(font, text, room > 0 ? room : 0);
    const int w = width_of(font, text, strlen(text));
    int x = it->x + (it->w - w) / 2;
    if (it->align == UI_VIEW_LEFT) {
        x = it->x + TEXT_PAD;
    } else if (it->align == UI_VIEW_RIGHT) {
        x = it->x + it->w - TEXT_PAD - w;
    }
    // Centre the ink of a typical line (cap height to baseline) in the box.
    const int top = it->y + (it->h - font->line_height) / 2;
    for (const char *c = text; *c; ++c) {
        const ui_glyph_t *g = glyph_for(font, *c);
        if (g) {
            draw_glyph(fb, lut, font, g, x, top, it->level, bg);
            x += g->advance;
        }
    }
}

void ui_view_render(const ui_view_t *view, uint16_t *fb, const uint16_t lut[16])
{
    memset(fb, 0, UI_CARD_W * UI_CARD_H * sizeof(uint16_t));
    const int count = view->count < UI_VIEW_ITEMS ? view->count : UI_VIEW_ITEMS;
    for (int i = 0; i < count; ++i) {
        const ui_view_item_t *it = &view->items[i];
        if (it->fill) {
            fill_box(fb, it, lut[it->fill & 0x0f]);
        }
        if (it->text[0]) {
            draw_label(fb, lut, it, background_of(view, i) & 0x0f);
        }
    }
}
