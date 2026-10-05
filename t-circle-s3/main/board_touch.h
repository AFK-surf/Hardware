#pragma once

#include <stdbool.h>

typedef enum {
    BOARD_TOUCH_NONE = 0,
    BOARD_TOUCH_CLICK,
} board_touch_gesture_t;

// Probe the CST816D and the BOOT button. Safe to call if the touch chip is absent.
void board_touch_init(void);

// Sample the screen and BOOT button. Call about every 10-20 ms from one task.
// A tap shorter than 280 ms becomes CLICK when the finger lifts.
// A longer press is not a click. BOOT never produces a gesture.
board_touch_gesture_t board_touch_poll(void);

bool board_touch_screen_held(void);
bool board_touch_boot_held(void);
