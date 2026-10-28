// SPDX-License-Identifier: Apache-2.0
#pragma once

// board_input.h for a board with a Hynitron CST816-family touch controller (CST816D, CST820: finger
// count at 0x02, chip ID at 0xA7, I2C address 0x15) and a BOOT key. The controller is polled: its
// finger count shows both press and release without the interrupt line. A tap shorter than 280 ms
// becomes BOARD_INPUT_TAP when the finger lifts; a longer press is not a tap. With points set, a
// press held still for 1 s is BOARD_INPUT_LONG_PRESS, a slower tap BOARD_INPUT_PRESS, and a finger
// that moved a swipe (or nothing), never a tap. BOOT is debounced here; app_poll() owns the holds.

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"

typedef struct {
    const char *chip;              // for the log
    i2c_master_bus_handle_t bus;   // NULL: no touch; BOOT still works
    gpio_num_t reset;              // pulsed before the probe; GPIO_NUM_NC when not wired
    gpio_num_t interrupt;          // unused, held as a quiet input; GPIO_NUM_NC when not wired
    gpio_num_t boot;
    uint8_t setup_reg;             // one register written after the probe, best effort
    uint8_t setup_value;
    // A failed read counts as no finger, for a controller that naps between touches (a read then
    // NACKs). Otherwise a failed read changes nothing.
    bool failed_read_is_release;
    // Read the finger's position too (0x03-0x06, with the finger count in one read), for
    // board_input_point() and the PRESS, LONG_PRESS and SWIPE events. Off: only taps, as before. The
    // position is mapped to a width x height screen, mirrored on an axis when the glass is turned.
    bool points;
    uint16_t width, height;
    bool mirror_x, mirror_y;
} cst816_touch_config_t;

// A board's board_input_init() calls this once.
void cst816_touch_init(const cst816_touch_config_t *config);
