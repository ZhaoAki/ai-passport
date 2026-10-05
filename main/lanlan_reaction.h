#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    LANLAN_REACTION_BLINK = 0,
    LANLAN_REACTION_TILT,
    LANLAN_REACTION_HAPPY,
    LANLAN_REACTION_BARK,
    LANLAN_REACTION_COUNT
} lanlan_reaction_t;

typedef struct {
    uint8_t remaining;
    uint8_t last;
    bool has_last;
} lanlan_reaction_bag_t;

/* Zero-initialize once. Each round visits every reaction; round boundaries
 * also exclude the previous reaction. Entropy is supplied by the caller. */
lanlan_reaction_t lanlan_reaction_next(lanlan_reaction_bag_t *bag, uint32_t entropy);
