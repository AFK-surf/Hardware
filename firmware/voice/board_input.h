// SPDX-License-Identifier: Apache-2.0
#pragma once

// What each touch board implements (boards/<board>/): the screen or key that starts and ends a
// call (or, on a board without voice, wakes the screen), and the BOOT key. call.c, or the board's
// own UI loop, polls both from the UI task; app_poll() owns BOOT holds.

#include <stdbool.h>

typedef enum {
    BOARD_INPUT_NONE = 0,
    BOARD_INPUT_TAP,  // a short tap on the screen, reported when the finger lifts
    // Only from a touch board that reports where the finger is (board_input_point):
    // A finger that moved is never a tap on such a board, only a swipe or nothing.
    BOARD_INPUT_PRESS,       // a slower tap: lifted after 280 ms but before a long press, not moved
    BOARD_INPUT_LONG_PRESS,  // held still for 1 s; reported once, while the finger is still down
    BOARD_INPUT_SWIPE_UP,    // moved mostly up, at least 40 px, then lifted (the finger moved up)
    BOARD_INPUT_SWIPE_DOWN,
} board_input_event_t;

// Set up the touch controller and the BOOT key. Safe to call if the touch chip is absent.
void board_input_init(void);

// Sample the inputs. Call about every 10-20 ms from one task. BOOT never produces an event here.
board_input_event_t board_input_poll(void);

// BOOT, debounced, as of the last poll.
bool board_input_boot_held(void);

// Where the finger was at its last sample on the screen, in screen pixels, for the event just
// reported. False on a board that does not report positions.
bool board_input_point(int *x, int *y);
