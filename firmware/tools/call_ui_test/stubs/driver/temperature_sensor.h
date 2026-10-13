// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "esp_err.h"
typedef void *temperature_sensor_handle_t;
typedef int temperature_sensor_config_t;
#define TEMPERATURE_SENSOR_CONFIG_DEFAULT(a,b) 0
esp_err_t temperature_sensor_install(const temperature_sensor_config_t *, temperature_sensor_handle_t *);
esp_err_t temperature_sensor_enable(temperature_sensor_handle_t);
esp_err_t temperature_sensor_get_celsius(temperature_sensor_handle_t, float *);
