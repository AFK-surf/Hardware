#pragma once

// The Cue logo at rest, one per screen size: ui/<size>/cue_logo.bin from tools/gen_cue_logo.py,
// 4 bpp, high nibble = left pixel.

#include "ui_card.h"

#define CUE_LOGO_SIZE  UI_CARD_SIZE
#define CUE_LOGO_BYTES (CUE_LOGO_SIZE * CUE_LOGO_SIZE / 2)
