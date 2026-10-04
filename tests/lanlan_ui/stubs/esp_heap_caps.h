/* Host stub for ESP-IDF's esp_heap_caps.h.
 *
 * main/lanlan_ui.c allocates its single sprite buffer with
 * heap_caps_malloc(LANLAN_SPRITE_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT).
 * On the host the capability flags are meaningless, so the call maps to the C
 * allocator and the flags are accepted and ignored. */
#pragma once

#include <stddef.h>
#include <stdlib.h>

#define MALLOC_CAP_8BIT (1 << 2)
#define MALLOC_CAP_32BIT (1 << 1)
#define MALLOC_CAP_SPIRAM (1 << 10)
#define MALLOC_CAP_INTERNAL (1 << 11)
#define MALLOC_CAP_DEFAULT (1 << 12)
#define MALLOC_CAP_DMA (1 << 3)

static inline void *heap_caps_malloc(size_t size, int caps) {
    (void)caps;
    return malloc(size);
}

static inline void *heap_caps_calloc(size_t count, size_t size, int caps) {
    (void)caps;
    return calloc(count, size);
}

static inline void *heap_caps_realloc(void *pointer, size_t size, int caps) {
    (void)caps;
    return realloc(pointer, size);
}

static inline void heap_caps_free(void *pointer) { free(pointer); }

static inline size_t heap_caps_get_free_size(int caps) {
    (void)caps;
    return 0;
}
