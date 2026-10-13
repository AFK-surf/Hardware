// SPDX-License-Identifier: Apache-2.0
#pragma once
// FreeRTOS on pthreads, just what lan_tunnel.c uses. A tick is a millisecond.
#include <stdint.h>
typedef uint32_t TickType_t;
typedef int BaseType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY 0xFFFFFFFFu
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
