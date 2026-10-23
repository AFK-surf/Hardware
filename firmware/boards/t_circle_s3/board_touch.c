// SPDX-License-Identifier: Apache-2.0
// LilyGO T-Circle-S3 V1.1: CST816D touch on its own I2C bus, BOOT on GPIO0 (cst816_touch.h). The
// controller is set to report periodically, so the polled finger count follows the finger.

#include "board_input.h"
#include "cst816_touch.h"

#include "esp_log.h"

#define T_CIRCLE_TOUCH_SDA GPIO_NUM_11
#define T_CIRCLE_TOUCH_SCL GPIO_NUM_14
#define T_CIRCLE_BOOT_GPIO GPIO_NUM_0

#define CST816D_REG_IRQ_MODE 0xFA
#define CST816D_IRQ_PERIODIC 0x40

void board_input_init(void)
{
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = T_CIRCLE_TOUCH_SDA,
        .scl_io_num = T_CIRCLE_TOUCH_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    const esp_err_t err = i2c_new_master_bus(&bus_cfg, &bus);
    if (err != ESP_OK) {
        ESP_LOGE("touch", "CST816D I2C bus init failed: %s", esp_err_to_name(err));
        bus = NULL;
    }
    const cst816_touch_config_t config = {
        .chip = "CST816D",
        .bus = bus,
        .reset = GPIO_NUM_NC,
        .interrupt = GPIO_NUM_NC,
        .boot = T_CIRCLE_BOOT_GPIO,
        .setup_reg = CST816D_REG_IRQ_MODE,
        .setup_value = CST816D_IRQ_PERIODIC,
        .failed_read_is_release = false,
    };
    cst816_touch_init(&config);
}
