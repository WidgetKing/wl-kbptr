// SPDX-License-Identifier: GPL-3.0-only

#ifndef __UTILS_WAYLAND_H_INCLUDED__
#define __UTILS_WAYLAND_H_INCLUDED__

#include "state.h"

void move_pointer(
    struct state *state, uint32_t x, uint32_t y, enum click click
);

// Press `click` at (x1, y1), travel to (x2, y2) over `duration_ms`, release.
// Coordinates are relative to state->current_output.
void drag_pointer(
    struct state *state, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
    uint32_t duration_ms, enum click click
);

#endif
