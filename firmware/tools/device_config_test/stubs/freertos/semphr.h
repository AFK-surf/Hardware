// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <pthread.h>
#include <stdlib.h>
typedef pthread_mutex_t *SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) {
    SemaphoreHandle_t m = malloc(sizeof(*m));
    if (m) pthread_mutex_init(m, NULL);
    return m;
}
static inline int xSemaphoreTake(SemaphoreHandle_t m, unsigned timeout) {
    (void)timeout; return pthread_mutex_lock(m) == 0;
}
static inline int xSemaphoreGive(SemaphoreHandle_t m) { return pthread_mutex_unlock(m) == 0; }
