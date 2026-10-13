// SPDX-License-Identifier: Apache-2.0
#pragma once

// M5PM1 power chip on the StickS3 (pmic.c).

#include <stdbool.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

// LCD/audio rail on, amp off, 5 V out and LED off, then 20 ms. Logs what it reads back once.
// Run before anything else on the board: the LCD and the codec are unpowered until this.
esp_err_t pmic_init(i2c_master_bus_handle_t bus);

// The speaker amp (AW8737) enable, PMIC GPIO3.
esp_err_t pmic_speaker(bool on);
