#pragma once

#include "esp_err.h"

esp_err_t display_init(void);

// Loop the Cue logo animation. Does not return.
void display_run_logo(void);
