#pragma once
#include "FreeRTOS.h"
#include <stddef.h>
typedef struct test_queue *QueueHandle_t;
QueueHandle_t xQueueCreate(size_t len, size_t item_size);
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t wait);
BaseType_t xQueueReceive(QueueHandle_t q, void *item, TickType_t wait);
