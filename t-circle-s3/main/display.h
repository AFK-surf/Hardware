#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t display_init(void);

// Show the logo: still while idle, looping while animating is set. Does not return.
void display_run_logo(void);

// Animate the logo (during a call) or let it come to rest on its last frame.
void display_set_animating(bool animating);
