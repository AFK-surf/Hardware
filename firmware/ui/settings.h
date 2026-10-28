// SPDX-License-Identifier: Apache-2.0
#pragma once

// On-screen settings for a round touch board that reports where the finger is
// (CONFIG_COMMA_SETTINGS_UI): Wi-Fi status, a scan and a switch to another network with an
// on-screen keyboard (wifi_switch.h), Wi-Fi off and on, and Bluetooth pairing mode. A long press on
// the idle screen opens them (voice/call.c); while they are open they get the screen and the touch
// input, and a tap never dials.

#include <stdbool.h>
#include <stdint.h>

#include "board_input.h"

bool settings_active(void);

// Open on the main page. The caller has checked that no call, pairing mode or BOOT hold is on.
void settings_open(int64_t now);

// Close without drawing anything: whatever takes over (a call, pairing mode) draws its own screen.
// A password being typed is wiped.
void settings_close(void);

// One pass of the UI loop while open: handle the touch event and redraw. back is a short BOOT
// press. paused while a BOOT hold is tracked: the hold's own card stays on screen.
void settings_poll(int64_t now, board_input_event_t event, bool back, bool paused);
