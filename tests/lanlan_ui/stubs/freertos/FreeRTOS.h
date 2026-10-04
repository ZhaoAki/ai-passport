/* Host stub for FreeRTOS's FreeRTOS.h.
 *
 * Only present so the host harness keeps compiling if an ESP-IDF-facing header
 * pulled in by main/lanlan_ui.c starts including it. Nothing here is linked
 * into firmware. */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef unsigned int UBaseType_t;

#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdFAIL 0

#define configTICK_RATE_HZ 1000
#define portTICK_PERIOD_MS 1
#define portMAX_DELAY ((TickType_t)0xFFFFFFFFu)

#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define pdTICKS_TO_MS(ticks) ((uint32_t)(ticks))
