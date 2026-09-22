// SPDX-License-Identifier: GPL-3.0-only

#ifndef __UTILS_WAYLAND_H_INCLUDED__
#define __UTILS_WAYLAND_H_INCLUDED__

#include "state.h"

#include <stdbool.h>
#include <stdio.h>

void move_pointer(
    struct state *state, uint32_t x, uint32_t y, enum click click
);

// Press `click` at (x1, y1), travel to (x2, y2) over `duration_ms`, release.
// Coordinates are relative to state->current_output.
void drag_pointer(
    struct state *state, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
    uint32_t duration_ms, enum click click
);

// Press `click` at (x, y) and keep it down, moving to each `x y` point read
// from `commands`, until that stream ends or says anything else. Layout
// coordinates, like drag_pointer's. The button is released before this
// returns, however it returns.
void hold_pointer(
    struct state *state, int32_t x, int32_t y, enum click click, FILE *commands
);

// Turn the wheel under the pointer, one notch per `up`/`down`/`left`/`right`
// line read from `commands`, with the modifiers held throughout; `mods LIST`
// changes what is held. Moves to (x, y), layout coordinates, first if `move`.
// Anything else, or EOF, ends it, and nothing is left held.
void scroll_pointer(
    struct state *state, bool move, int32_t x, int32_t y, FILE *commands
);

// A --modifiers list, commas or whitespace between names, into MODIFIER_*
// bits. Non-zero for a name it does not know. Defined in main.c.
int parse_modifiers(const char *list, uint32_t *modifiers);

#endif
