// SPDX-License-Identifier: Apache-2.0
#pragma once

// Waveshare ESP32-S3-Touch-AMOLED-1.43C (ESP32-S3-PICO-1-N8R8). Pins from Waveshare's schematic
// (ESP32-S3-Touch-AMOLED-1.43C-Schematic.pdf) and its ESP-IDF examples (bsp_config.h and
// codec_board/board_cfg.txt, entry S3_AMOLED_1_43C, in waveshareteam/ESP32-S3-Touch-AMOLED-1.43C).
// Facts only, no code copied. The pins differ from the 1.32's even where the parts are the same.

#include "driver/gpio.h"
#include "driver/i2c_master.h"

// I2C: ES8311 (0x18), ES7210 (0x40) and the CST820 touch (0x15) share one bus, 4.7 k pull-ups.
#define WS_I2C_SDA GPIO_NUM_47
#define WS_I2C_SCL GPIO_NUM_48

// One I2S port, full duplex. The ES8311 takes the speaker samples (DSDIN); the ES7210 drives the mic
// samples (SDOUT1). Both are clock slaves on MCLK/SCLK/LRCK.
#define WS_I2S_MCLK GPIO_NUM_38
#define WS_I2S_SCLK GPIO_NUM_39
#define WS_I2S_WS GPIO_NUM_40
#define WS_I2S_DOUT GPIO_NUM_41  // to the ES8311's DAC
#define WS_I2S_DIN GPIO_NUM_42   // from the ES7210's ADC
// ES7210 in standard I2S mode puts MIC1 on the left slot and MIC2 on the right slot of SDOUT1.
// MIC3 is the speaker loopback (the AEC reference); it is only on SDOUT2/TDM and is not used yet.
#define WS_MIC_SLOT 0
// NS4150B speaker amplifier enable (CTRL), active high.
#define WS_PA GPIO_NUM_46
// CODEC_EN: power switch for the codecs' supply, active high, pulled up on the board (R18).
#define WS_CODEC_EN GPIO_NUM_18

// CO5300 AMOLED, QSPI. No backlight pin: brightness is a panel command. TE (GPIO8) is not used.
#define WS_LCD_CS GPIO_NUM_15
#define WS_LCD_PCLK GPIO_NUM_14
#define WS_LCD_D0 GPIO_NUM_9
#define WS_LCD_D1 GPIO_NUM_10
#define WS_LCD_D2 GPIO_NUM_11
#define WS_LCD_D3 GPIO_NUM_12
#define WS_LCD_RST GPIO_NUM_13

// CST820 touch.
#define WS_TOUCH_RST GPIO_NUM_16
#define WS_TOUCH_INT GPIO_NUM_17

#define WS_BOOT GPIO_NUM_0

// The shared I2C bus, created by board_init() before audio and touch. NULL if that failed.
i2c_master_bus_handle_t ws_i2c_bus(void);
