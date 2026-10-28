// SPDX-License-Identifier: Apache-2.0
// Waveshare ESP32-S3-Touch-AMOLED-1.43C: CST820 touch on the shared I2C bus, BOOT on GPIO0
// (cst816_touch.h), as on the 1.32 with this board's reset and interrupt pins. The CST820 naps
// between touches unless told not to, and a read while it naps fails: that counts as no finger.
// A tap starts or ends a call; so does a short BOOT press (CONFIG_COMMA_BOOT_PRESS_CALLS). A 1 s
// press on the idle screen opens the settings (CONFIG_COMMA_SETTINGS_UI, ui/settings.c).

#include "board_input.h"
#include "board_pins.h"
#include "cst816_touch.h"

// CST816-family "DisAutoSleep". Best effort: the polling works either way.
#define CST820_REG_DIS_AUTO_SLEEP 0xFE

void board_input_init(void)
{
    const cst816_touch_config_t config = {
        .chip = "CST820",
        .bus = ws_i2c_bus(),
        .reset = WS_TOUCH_RST,
        .interrupt = WS_TOUCH_INT,
        .boot = WS_BOOT,
        .setup_reg = CST820_REG_DIS_AUTO_SLEEP,
        .setup_value = 0x01,
        .failed_read_is_release = true,
        // Positions for the on-screen settings. The glass is mounted turned, like the panel (MADCTL
        // 0xC0 in panel.c): Waveshare's example maps the touch as (466 - x, 466 - y).
        .points = true,
        .width = 466,
        .height = 466,
        .mirror_x = true,
        .mirror_y = true,
    };
    cst816_touch_init(&config);
}
