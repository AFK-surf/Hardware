// SPDX-License-Identifier: Apache-2.0
#pragma once

// The call controller shared by the voice boards: tap (or a short BOOT press, where the board has
// COMMA_BOOT_PRESS_CALLS) to call and to hang up, the call screen, notices, and the chip
// temperature log. Boards provide audio (board_audio.h), input (board_input.h) and the panel
// (board_display.h).

// ESP-SR AFE (voice_clean.h). Before the display and Wi-Fi take their share of internal RAM.
void call_voice_clean_init(void);

// Temperature sensor and the playback task.
void call_init(void);

// The UI loop; never returns. help: one log line saying how to call on this board.
void call_run_ui(const char *help);
