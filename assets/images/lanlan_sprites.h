/* Cyber Lanlan sprite descriptors.
 *
 * The pixel data in assets/images/lanlan_sprites.c is PLACEHOLDER ART and not
 * the owner-approved final appearance; it exists to validate layout, animation
 * timing and rendering. See main/lanlan_model.h for the animation states.
 *
 * Frames are 96x96 RGB565 stored big-endian (byte 0 = high byte), 18,432 bytes
 * each, kept in flash and copied one frame at a time into a single canvas
 * buffer; the whole set is never loaded at once. */
#pragma once

#include <stdint.h>

#define LANLAN_SPRITE_WIDTH 96
#define LANLAN_SPRITE_HEIGHT 96
#define LANLAN_SPRITE_BYTES (LANLAN_SPRITE_WIDTH * LANLAN_SPRITE_HEIGHT * 2)

typedef struct {
    const uint8_t *pixels;
    uint16_t width;
    uint16_t height;
} lanlan_sprite_t;

enum {
    LANLAN_SPRITE_IDLE_0 = 0,
    LANLAN_SPRITE_IDLE_1,
    LANLAN_SPRITE_BLINK,
    LANLAN_SPRITE_HAPPY,
    LANLAN_SPRITE_BARK,
    LANLAN_SPRITE_COUNT
};

extern const lanlan_sprite_t lanlan_sprites[LANLAN_SPRITE_COUNT];
