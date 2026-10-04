/* Host stub for FreeRTOS's semphr.h. See freertos/FreeRTOS.h in this directory. */
#pragma once

#include "freertos/FreeRTOS.h"

typedef void *SemaphoreHandle_t;

static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) { return NULL; }
static inline SemaphoreHandle_t xSemaphoreCreateBinary(void) { return NULL; }
static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t timeout) {
    (void)semaphore;
    (void)timeout;
    return pdTRUE;
}
static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore) {
    (void)semaphore;
    return pdTRUE;
}
