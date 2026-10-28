// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t display_init(void);

// Draw what was last asked for, and sleep the screen when idle. Does not return.
void display_run(void);

// Replace the logo with a text card until display_show_logo(). Any part may be NULL.
// label: small gray line(s) on top. big: digits, spaces and ':' only (pairing code, countdown,
// call time). body: small white text, wrapped to the round glass; '\n' breaks a line.
// Both wake the screen. Asking for what is already shown doesn't redraw.
void display_show_card(const char *label, const char *big, const char *body);
void display_show_logo(void);

// A tap or BOOT press: wake the screen and restart the idle timer.
void display_wake(void);

// While set, the screen stays on (a call, pairing mode with its passkey, a notice).
void display_keep_awake(bool on);

bool display_asleep(void);

// Draw a view (ui_view.h) instead of a card, until the next card or logo: the on-screen settings
// (CONFIG_COMMA_SETTINGS_UI only). It is copied; asking for what is already shown doesn't redraw.
struct ui_view;
void display_show_view(const struct ui_view *view);
