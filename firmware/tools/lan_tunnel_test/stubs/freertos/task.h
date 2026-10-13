// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "FreeRTOS.h"
#include <stddef.h>
typedef void *TaskHandle_t;
// Runs fn on a pthread.
BaseType_t xTaskCreateWithCaps(void (*fn)(void *), const char *name, size_t stack, void *arg, int prio,
                               TaskHandle_t *out, unsigned caps);
void vTaskDelay(TickType_t ticks);
