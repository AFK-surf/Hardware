#pragma once

// What each display board implements (boards/<board>/). ui/display.c decides what to show and when
// to sleep; the board owns the panel, its framebuffer and the transfer.
//
// The framebuffer is UI_CARD_SIZE x UI_CARD_SIZE RGB565 pixels in the panel's byte order (the gray
// table in display.c swaps bytes, as both panels take big-endian pixels).

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

// Panel up, dark, and the framebuffer allocated. Called once from display_init().
esp_err_t board_display_init(void);

// The framebuffer, once the previous frame no longer reads it. NULL if it is still busy after the
// board's own time limit; display.c then keeps the frame pending and tries again.
uint16_t *board_display_frame(void);

// Send the framebuffer just drawn. An error means the frame did not reach the panel; display.c
// logs it and draws the frame again later. Never waits forever.
esp_err_t board_display_flush(void);

// SLPIN and friends: light off, let a transfer finish, panel off. Pixels may be lost.
void board_display_sleep(void);
// Out of sleep, ready for a frame. Light and display-on come after the first frame.
void board_display_wake(void);
// Display on after a wake, once its first frame is drawn.
void board_display_on(void);
// Backlight (or panel brightness) on or off.
void board_display_light(bool on);
