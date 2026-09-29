#pragma once

#include <stdbool.h>

typedef enum {
    BOARD_TOUCH_NONE = 0,
    BOARD_TOUCH_CLICK,
    BOARD_TOUCH_DOUBLE,
} board_touch_gesture_t;

// Probe the CST816D and the BOOT button. Safe to call if the touch chip is absent.
void board_touch_init(void);

// Sample the screen and BOOT button. Call about every 10-20 ms from one task.
// A tap shorter than 280 ms becomes CLICK after 450 ms with no second tap.
// Two such taps become DOUBLE. A longer press is a hold, not a click.
// BOOT never produces a gesture.
board_touch_gesture_t board_touch_poll(void);

bool board_touch_screen_held(void);
bool board_touch_boot_held(void);
bool board_touch_talk_held(void);
