#pragma once

// What each voice board implements (boards/<board>/): the screen or key that starts and ends a
// call, and the BOOT key. call.c polls both from the UI task; app_poll() owns BOOT holds.

#include <stdbool.h>

typedef enum {
    BOARD_INPUT_NONE = 0,
    BOARD_INPUT_TAP,  // a short tap on the screen, reported when the finger lifts
} board_input_event_t;

// Set up the touch controller and the BOOT key. Safe to call if the touch chip is absent.
void board_input_init(void);

// Sample the inputs. Call about every 10-20 ms from one task. BOOT never produces an event here.
board_input_event_t board_input_poll(void);

// BOOT, debounced, as of the last poll.
bool board_input_boot_held(void);
