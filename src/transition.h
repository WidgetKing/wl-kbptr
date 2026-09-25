// SPDX-License-Identifier: GPL-3.0-only

#ifndef __TRANSITION_H_INCLUDED__
#define __TRANSITION_H_INCLUDED__

#include <cairo/cairo.h>
#include <stdint.h>

// How the overlay arrives. A transition never knows what the overlay is: it is
// handed the finished frame as a pattern and draws it `t` of the way in, from
// 0 (nothing yet) to 1 (all of it). So a new one is a function and a row in the
// table in transition.c, and no mode has to know it exists.
//
// `cairo` is in surface coordinates (logical pixels, `w` by `h`) and starts
// out cleared, so drawing nothing somewhere leaves that part of the screen
// showing through. `chunk` is the size of the pieces it breaks the overlay
// into, in logical pixels, which each transition reads its own way. `seed` is
// fixed for one overlay and different for the next, so the same transition
// does not play out the same way twice.
struct transition {
    const char *name;
    void (*draw)(
        cairo_t *cairo, cairo_pattern_t *overlay, double t, double w, double h,
        double chunk, uint32_t seed
    );
};

#define MAX_TRANSITIONS 16

// The transitions one is picked from. Empty is `none`.
struct transition_set {
    const struct transition *items[MAX_TRANSITIONS];
    int                      count;
};

// Parse "none", "random" (every transition there is), or a comma-separated
// list of names into `out`. Returns 0 on success; on failure logs why.
int transition_set_parse(struct transition_set *out, const char *value);

#endif
