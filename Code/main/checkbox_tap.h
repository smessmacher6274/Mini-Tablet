#pragma once
#include <stdbool.h>

typedef struct { bool held; unsigned target, release_samples; } checkbox_tap_t;

// Return the stable item identity once per press, after three released polls.
static inline unsigned checkbox_tap_update(checkbox_tap_t *tap, bool down,
                                           bool sampled, unsigned hit) {
    if (down) {
        tap->release_samples = 0;
        if (sampled) {
            if (!tap->held) { tap->held = true; tap->target = hit; }
            else if (hit != tap->target) tap->target = 0;
        }
    } else if (tap->held && ++tap->release_samples >= 3) {
        unsigned target = tap->target;
        *tap = (checkbox_tap_t){0};
        return target;
    }
    return 0;
}
