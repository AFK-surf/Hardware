#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t display_init(void);

// Show the logo: still while idle, looping while animating is set. Does not return.
void display_run_logo(void);

// Animate the logo (during a call) or let it come to rest on its last frame.
void display_set_animating(bool animating);

// Replace the logo with a text card until display_show_logo(). Any part may be NULL.
// label: small gray line(s) on top. big: digits and spaces only (pairing code, countdown).
// body: small white text, wrapped to the round glass; '\n' breaks a line.
void display_show_card(const char *label, const char *big, const char *body);

void display_show_logo(void);
