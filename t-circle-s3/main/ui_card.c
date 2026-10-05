#include "ui_card.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "ui_font.h"

#define CARD_LINES_MAX 10
#define CARD_LINE_CHARS 64
#define CARD_GAP 8
#define CARD_MARGIN 14
#define LEVEL_LABEL 8
#define LEVEL_TEXT 15

typedef struct {
    const ui_font_t *font;
    uint8_t level;
    int gap_before;
    char text[CARD_LINE_CHARS];
} card_line_t;

static const ui_glyph_t *glyph_for(const ui_font_t *font, char c)
{
    const char *at = c ? strchr(font->chars, c) : NULL;
    if (!at) {
        at = strchr(font->chars, '?');
    }
    return at ? &font->glyphs[at - font->chars] : NULL;
}

static int text_width(const ui_font_t *font, const char *text, size_t len)
{
    int w = 0;
    for (size_t i = 0; i < len; ++i) {
        const ui_glyph_t *g = glyph_for(font, text[i]);
        w += g ? g->advance : 0;
    }
    return w;
}

static int line_height(const ui_font_t *font)
{
    return font == &ui_font_large ? font->ink_bottom - font->ink_top : font->line_height;
}

// Usable width of the round glass across rows [top, bottom).
static int chord_width(int top, int bottom)
{
    const float r = UI_CARD_SIZE / 2.0f;
    const float d = fmaxf(fabsf(top - r), fabsf(bottom - r));
    const int w = d >= r ? 0 : (int)(2.0f * sqrtf(r * r - d * d)) - CARD_MARGIN;
    return w < 48 ? 48 : w;
}

// Copy text with non-ASCII code points shown as '?', so a UTF-8 SSID keeps its length.
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

// Greedy word wrap. Each line gets the chord width at the row it lands on, starting at *y.
static int wrap(const char *text, const ui_font_t *font, uint8_t level, int gap_before, int *y,
                card_line_t *lines, int count)
{
    char folded[sizeof(((ui_card_t *)0)->body)];
    ascii_fold(text, folded, sizeof(folded));
    const int h = line_height(font);
    const char *p = folded;
    bool first = true;
    while (*p && count < CARD_LINES_MAX) {
        if (*p == '\n') {
            ++p;
            continue;
        }
        const int gap = first ? gap_before : 0;
        const int max_w = chord_width(*y + gap, *y + gap + h);
        size_t len = 0, fit = 0;
        while (p[len] && p[len] != '\n' && len + 1 < CARD_LINE_CHARS) {
            ++len;
            if (text_width(font, p, len) > max_w) {
                --len;
                break;
            }
            if (p[len] == ' ' || p[len] == '\0' || p[len] == '\n') {
                fit = len;
            }
        }
        if (fit == 0) {
            fit = len ? len : 1;  // one long word: break it
        }
        card_line_t *line = &lines[count++];
        line->font = font;
        line->level = level;
        line->gap_before = gap;
        memcpy(line->text, p, fit);
        line->text[fit] = '\0';
        *y += gap + h;
        p += fit;
        while (*p == ' ') {
            ++p;
        }
        first = false;
    }
    return count;
}

static int layout(const ui_card_t *card, int start_y, card_line_t *lines, int *height)
{
    int y = start_y;
    int count = 0;
    if (card->label[0]) {
        count = wrap(card->label, &ui_font_small, LEVEL_LABEL, 0, &y, lines, count);
    }
    if (card->big[0] && count < CARD_LINES_MAX) {
        const int gap = count ? CARD_GAP : 0;
        card_line_t *line = &lines[count++];
        line->font = &ui_font_large;
        line->level = LEVEL_TEXT;
        line->gap_before = gap;
        strlcpy(line->text, card->big, sizeof(line->text));
        y += gap + line_height(&ui_font_large);
    }
    if (card->body[0]) {
        count = wrap(card->body, &ui_font_small, LEVEL_TEXT, count ? CARD_GAP : 0, &y, lines, count);
    }
    *height = y - start_y;
    return count;
}

static void draw_glyph(uint16_t *fb, const uint16_t *lut, const ui_font_t *font, const ui_glyph_t *g, int x0, int y0,
                       uint8_t level)
{
    const int row_bytes = (g->w + 1) / 2;
    const uint8_t *src = font->bitmap + g->offset;
    for (int y = 0; y < g->h; ++y) {
        const int py = y0 + g->y + y;
        if (py < 0 || py >= UI_CARD_SIZE) {
            continue;
        }
        for (int x = 0; x < g->w; ++x) {
            const int px = x0 + g->x + x;
            if (px < 0 || px >= UI_CARD_SIZE) {
                continue;
            }
            const uint8_t b = src[y * row_bytes + x / 2];
            const uint8_t a = (x & 1) ? (b & 0x0f) : (b >> 4);
            if (a) {
                fb[py * UI_CARD_SIZE + px] = lut[(a * level + 7) / 15];
            }
        }
    }
}

void ui_card_render(const ui_card_t *card, uint16_t *fb, const uint16_t lut[16])
{
    card_line_t lines[CARD_LINES_MAX];
    int height = 0;
    int count = layout(card, 0, lines, &height);
    // Re-wrap against the rows the block really lands on once it is centred.
    for (int pass = 0; pass < 3; ++pass) {
        count = layout(card, (UI_CARD_SIZE - height) / 2, lines, &height);
    }
    memset(fb, 0, UI_CARD_SIZE * UI_CARD_SIZE * sizeof(uint16_t));
    int y = (UI_CARD_SIZE - height) / 2;
    for (int i = 0; i < count; ++i) {
        const card_line_t *line = &lines[i];
        y += line->gap_before;
        const int top = line->font == &ui_font_large ? y - line->font->ink_top : y;
        int x = (UI_CARD_SIZE - text_width(line->font, line->text, strlen(line->text))) / 2;
        for (const char *c = line->text; *c; ++c) {
            const ui_glyph_t *g = glyph_for(line->font, *c);
            if (g) {
                draw_glyph(fb, lut, line->font, g, x, top, line->level);
                x += g->advance;
            }
        }
        y += line_height(line->font);
    }
}
