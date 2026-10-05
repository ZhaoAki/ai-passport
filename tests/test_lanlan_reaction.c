#include <assert.h>
#include <stdio.h>
#include "lanlan_reaction.h"

int main(void) {
    for (unsigned seed = 0; seed < 100; ++seed) {
        lanlan_reaction_bag_t bag = {0};
        uint32_t random = seed, seen = 0;
        int previous = -1;
        for (unsigned n = 0; n < 10000; ++n) {
            random = random * 1664525u + 1013904223u;
            int choice = lanlan_reaction_next(&bag, seed ? random : 0);
            assert(choice >= 0 && choice < LANLAN_REACTION_COUNT);
            assert(choice != previous);
            assert(!(seen & (1u << choice)));
            seen |= 1u << choice;
            if (n % LANLAN_REACTION_COUNT == LANLAN_REACTION_COUNT - 1) {
                assert(seen == (1u << LANLAN_REACTION_COUNT) - 1u);
                seen = 0;
            }
            previous = choice;
        }
    }
    puts("Lanlan reactions: PASS (one million selections, complete rounds, no adjacent repeat)");
    return 0;
}
