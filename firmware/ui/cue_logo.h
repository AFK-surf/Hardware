// SPDX-License-Identifier: Apache-2.0
#pragma once

// The Comma logo at rest, one per screen size: ui/<size>/cue_logo.bin from tools/gen_cue_logo.py,
// 4 bpp, high nibble = left pixel, two pixels per byte in raster order (a row of odd width shares
// its last byte with the next row).

#include "ui_card.h"

#define CUE_LOGO_W     UI_CARD_W
#define CUE_LOGO_H     UI_CARD_H
#define CUE_LOGO_BYTES (CUE_LOGO_W * CUE_LOGO_H / 2)
