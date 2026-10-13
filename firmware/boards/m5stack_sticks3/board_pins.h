// SPDX-License-Identifier: Apache-2.0
#pragma once

// M5Stack StickS3 (K150, ESP32-S3-PICO-1-N8R8). Pins from M5's K150 schematic v0.6, as used by
// muse-gadget-sdk's board_m5stack_sticks3.c (Apache-2.0, (c) Meta Platforms).

#include "driver/gpio.h"
#include "driver/i2c_master.h"

// I2C0, 100 kHz: the M5PM1 power chip (0x6E) and the ES8311 codec. The codec is reachable only
// once the PMIC's L3B rail is on.
#define STICK_I2C_SDA GPIO_NUM_47
#define STICK_I2C_SCL GPIO_NUM_48
#define STICK_I2C_HZ 100000

// ES8311, one I2S port, full duplex. One mic, on the left slot of a stereo frame.
#define STICK_I2S_MCLK GPIO_NUM_18
#define STICK_I2S_BCLK GPIO_NUM_17
#define STICK_I2S_WS GPIO_NUM_15
#define STICK_I2S_DOUT GPIO_NUM_14  // to the codec's DAC
#define STICK_I2S_DIN GPIO_NUM_16   // from the codec's ADC
#define STICK_MIC_SLOT 0            // left

// ST7789 1.14" 135x240, SPI3. Backlight by LEDC PWM.
#define STICK_LCD_SCLK GPIO_NUM_40
#define STICK_LCD_MOSI GPIO_NUM_39
#define STICK_LCD_CS GPIO_NUM_41
#define STICK_LCD_DC GPIO_NUM_45
#define STICK_LCD_RST GPIO_NUM_21
#define STICK_LCD_BL GPIO_NUM_38

// Keys, active low. KEY1: the big key on the front, under the screen. KEY2: on the side.
#define STICK_KEY1 GPIO_NUM_11
#define STICK_KEY2 GPIO_NUM_12

// The shared I2C bus, created first by board_init(). NULL if that failed.
i2c_master_bus_handle_t stick_i2c_bus(void);
