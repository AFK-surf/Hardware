// SPDX-License-Identifier: Apache-2.0
#pragma once

// Mirror of the screen on the USB Serial/JTAG port (CONFIG_COMMA_SCREEN_MIRROR), for
// tools/screen_mirror.py. Off until the computer sends "comma screen on", which it repeats as a
// keepalive; mirroring stops on "comma screen off" or 30 s after the last keepalive. USB only.
//
// Each frame that reached the panel and differs from the last one sent goes out as text lines
//   @@F <frame> <part> <parts> <hash> <base64>
// whose decoded parts, joined, are: width and height (u16, little-endian), then the 16 gray levels
// run-length coded, one byte per run: level << 4 | (length - 1) for 1-15 pixels, or level << 4 | 15
// followed by a u16 length. hash is FNV-1a (32 bit) of the joined bytes; a frame that arrives
// damaged, for example cut by a log line, is dropped by the tool, which then asks for it again with
// "comma screen refresh". "@@S off" says the panel went to sleep; the next frame says it woke.

#include <stdbool.h>
#include <stdint.h>

// From the USB console: on (also the keepalive), off, or send the current frame again.
void screen_mirror_request(bool on);
void screen_mirror_refresh(void);

// From the display task: a frame of UI_CARD_W x UI_CARD_H panel pixels just reached the panel.
// Returns at once; the frame is coded here and sent by the mirror's own task.
void screen_mirror_capture(const uint16_t *fb);

// From the display task: the panel went to sleep.
void screen_mirror_sleep(void);
