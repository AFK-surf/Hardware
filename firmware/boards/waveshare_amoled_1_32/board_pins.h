#pragma once

// Waveshare ESP32-S3-Touch-AMOLED-1.32 (ESP32-S3-PICO-1). Pins from the Waveshare BSP
// (waveshare/esp32_s3_touch_amoled_1_32, include/bsp/esp32_s3_touch_amoled_1_32.h, Apache-2.0).

#include "driver/gpio.h"
#include "driver/i2c_master.h"

// I2C: ES8311 codec control and CST820 touch share one bus.
#define WS_I2C_SDA GPIO_NUM_47
#define WS_I2C_SCL GPIO_NUM_48

// ES8311, one I2S port, full duplex.
#define WS_I2S_MCLK GPIO_NUM_38
#define WS_I2S_SCLK GPIO_NUM_39
#define WS_I2S_WS GPIO_NUM_41
#define WS_I2S_DOUT GPIO_NUM_42  // to the codec's DAC
#define WS_I2S_DIN GPIO_NUM_40   // from the codec's ADC
// Speaker power amplifier enable, active high. Driven here, never by the codec driver.
#define WS_PA GPIO_NUM_46

// CO5300 AMOLED, QSPI. No backlight pin: brightness is a panel command.
#define WS_LCD_CS GPIO_NUM_10
#define WS_LCD_PCLK GPIO_NUM_11
#define WS_LCD_D0 GPIO_NUM_12
#define WS_LCD_D1 GPIO_NUM_13
#define WS_LCD_D2 GPIO_NUM_14
#define WS_LCD_D3 GPIO_NUM_15
#define WS_LCD_RST GPIO_NUM_8

// CST820 touch.
#define WS_TOUCH_RST GPIO_NUM_7
#define WS_TOUCH_INT GPIO_NUM_6

#define WS_BOOT GPIO_NUM_0

// The shared I2C bus, created by board_init() before audio and touch. NULL if that failed.
i2c_master_bus_handle_t ws_i2c_bus(void);
