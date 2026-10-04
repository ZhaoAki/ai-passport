/* Host stub for ESP-IDF's esp_timer.h.
 *
 * main/lanlan_ui.c drives its animation from lv_tick_get(), but the ESP-IDF
 * timer API is provided here so the harness keeps compiling if the UI or the
 * application model starts using microsecond timestamps. */
#pragma once

#include <stdint.h>
#include <time.h>

static inline int64_t esp_timer_get_time(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000000 + (int64_t)now.tv_nsec / 1000;
}
