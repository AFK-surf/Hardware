// SPDX-License-Identifier: Apache-2.0
#pragma once

// The Comma logo at rest, one per screen size: ui/<size>/comma_logo.bin from tools/gen_comma_logo.py,
// 4 bpp, high nibble = left pixel, two pixels per byte in raster order (a row of odd width shares
// its last byte with the next row).

#include "ui_card.h"

#define COMMA_LOGO_W     UI_CARD_W
#define COMMA_LOGO_H     UI_CARD_H
#define COMMA_LOGO_BYTES (COMMA_LOGO_W * COMMA_LOGO_H / 2)
