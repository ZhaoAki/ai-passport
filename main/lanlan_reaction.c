#include "lanlan_reaction.h"

lanlan_reaction_t lanlan_reaction_next(lanlan_reaction_bag_t *bag, uint32_t entropy) {
    if (!bag) return LANLAN_REACTION_HAPPY;
    if (!bag->remaining) bag->remaining = (1u << LANLAN_REACTION_COUNT) - 1u;
    uint8_t candidates[LANLAN_REACTION_COUNT];
    unsigned count = 0;
    for (unsigned i = 0; i < LANLAN_REACTION_COUNT; ++i) {
        if ((bag->remaining & (1u << i)) && (!bag->has_last || i != bag->last)) {
            candidates[count++] = (uint8_t)i;
        }
    }
    /* Valid bags always have a candidate, including on a round boundary. */
    if (!count) {
        bag->remaining = 0;
        bag->has_last = false;
        return lanlan_reaction_next(bag, entropy);
    }
    uint8_t choice = candidates[entropy % count];
    bag->remaining &= (uint8_t)~(1u << choice);
    bag->last = choice;
    bag->has_last = true;
    return (lanlan_reaction_t)choice;
}
