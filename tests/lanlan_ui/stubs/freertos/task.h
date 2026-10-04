/* Host stub for FreeRTOS's task.h. See freertos/FreeRTOS.h in this directory. */
#pragma once

#include "freertos/FreeRTOS.h"

typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);

static inline TickType_t xTaskGetTickCount(void) { return 0; }
static inline void vTaskDelay(TickType_t ticks) { (void)ticks; }
static inline BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack,
                                     void *arg, UBaseType_t priority, TaskHandle_t *handle) {
    (void)fn;
    (void)name;
    (void)stack;
    (void)arg;
    (void)priority;
    if (handle) *handle = NULL;
    return pdFAIL;
}
