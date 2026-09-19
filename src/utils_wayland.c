// SPDX-License-Identifier: GPL-3.0-only

#include "utils_wayland.h"

#include "state.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

#include <stdint.h>
#include <time.h>
#include <wayland-client.h>

static void _apply_transform(
    uint32_t *x, uint32_t *y, uint32_t *width, uint32_t *height,
    enum wl_output_transform transform
) {
    uint32_t temp;

    switch (transform) {
    case WL_OUTPUT_TRANSFORM_NORMAL:
        break;

    case WL_OUTPUT_TRANSFORM_90:
        temp = *x;
        *x   = *y;
        *y   = *width - temp;

        temp    = *width;
        *width  = *height;
        *height = temp;
        break;

    case WL_OUTPUT_TRANSFORM_180:
        *x = *width - *x;
        *y = *height - *y;
        break;

    case WL_OUTPUT_TRANSFORM_270:
        temp = *x;
        *x   = *height - *y;
        *y   = temp;

        temp    = *width;
        *width  = *height;
        *height = temp;
        break;

    case WL_OUTPUT_TRANSFORM_FLIPPED:
        *x = *width - *x;
        break;

    case WL_OUTPUT_TRANSFORM_FLIPPED_90:
        *x = *width - *x;
        _apply_transform(x, y, width, height, WL_OUTPUT_TRANSFORM_90);
        break;

    case WL_OUTPUT_TRANSFORM_FLIPPED_180:
        *x = *width - *x;
        _apply_transform(x, y, width, height, WL_OUTPUT_TRANSFORM_180);
        break;

    case WL_OUTPUT_TRANSFORM_FLIPPED_270:
        *x = *width - *x;
        _apply_transform(x, y, width, height, WL_OUTPUT_TRANSFORM_270);
        break;
    }
}

void move_pointer(
    struct state *state, uint32_t x, uint32_t y, enum click click
) {
    if (!state->wl_virtual_pointer_mgr) {
        // We running in `--print-only` mode.
        return;
    }

    wl_display_roundtrip(state->wl_display);

    struct zwlr_virtual_pointer_v1 *virt_pointer =
        zwlr_virtual_pointer_manager_v1_create_virtual_pointer_with_output(
            state->wl_virtual_pointer_mgr,
            ((struct seat *)state->seats.next)->wl_seat,
            state->current_output->wl_output
        );

    uint32_t output_width  = state->current_output->width;
    uint32_t output_height = state->current_output->height;

    _apply_transform(
        &x, &y, &output_width, &output_height, state->current_output->transform
    );

    zwlr_virtual_pointer_v1_motion_absolute(
        virt_pointer, 0, x, y, output_width, output_height
    );
    zwlr_virtual_pointer_v1_frame(virt_pointer);
    wl_display_roundtrip(state->wl_display);

    if (state->click != CLICK_NONE) {
        int btn = 271 + click;

        zwlr_virtual_pointer_v1_button(
            virt_pointer, 0, btn, WL_POINTER_BUTTON_STATE_PRESSED
        );
        zwlr_virtual_pointer_v1_frame(virt_pointer);
        wl_display_roundtrip(state->wl_display);

        zwlr_virtual_pointer_v1_button(
            virt_pointer, 0, btn, WL_POINTER_BUTTON_STATE_RELEASED
        );
        zwlr_virtual_pointer_v1_frame(virt_pointer);
        wl_display_roundtrip(state->wl_display);
    }

    zwlr_virtual_pointer_v1_destroy(virt_pointer);
}

// A drag is a press, a path, and a release. It is one function rather than
// three calls because the button must not outlive this process: a caller that
// could hold it across a return is a caller that can leave the desktop with a
// button down.
//
// The path is walked rather than jumped because a jump is not a drag. Clients
// see drag-and-drop as a stream of motion events under a held button, and one
// motion from A to B gives them a single event to infer everything from --
// toolkits that arm on a movement threshold, autoscroll on dwell, or animate a
// drop target never get the chance. ~8ms between frames is a little finer than
// a 120Hz screen can show, so it costs nothing to be smooth.
//
// The holds at each end are for the same reason. A press and a motion in the
// same millisecond can be coalesced into a click that happened to move; a
// motion and a release in the same millisecond can land the drop before the
// client has processed where the pointer got to.
//
// Both ends are layout coordinates -- the ones the compositor lays its outputs
// out in, which is also what a drag over two screens has to be said in. So the
// virtual pointer is created WITHOUT an output: one bound to an output has its
// absolute motion mapped into that output and can never leave it, however the
// path is phrased. Unbound, the same motion is mapped over the whole layout,
// which is the one device walking the whole path -- and one device is what
// makes the crossing invisible to the client holding the drag.
static void _sleep_ms(uint32_t ms) {
    if (ms == 0) {
        return;
    }

    struct timespec ts = {
        .tv_sec  = ms / 1000,
        .tv_nsec = (long)(ms % 1000) * 1000000L,
    };
    nanosleep(&ts, NULL);
}

// The box every output sits inside, which is the extent absolute motion is
// mapped over. Computed from the outputs rather than assumed to start at 0,0:
// a layout can have an output left of or above the origin.
static void _layout_box(
    struct state *state, int32_t *x, int32_t *y, uint32_t *width,
    uint32_t *height
) {
    struct output *output;
    int32_t        left = INT32_MAX, top = INT32_MAX;
    int32_t        right = INT32_MIN, bottom = INT32_MIN;

    wl_list_for_each (output, &state->outputs, link) {
        if (output->x < left) {
            left = output->x;
        }
        if (output->y < top) {
            top = output->y;
        }
        if (output->x + output->width > right) {
            right = output->x + output->width;
        }
        if (output->y + output->height > bottom) {
            bottom = output->y + output->height;
        }
    }

    *x      = left;
    *y      = top;
    *width  = right > left ? (uint32_t)(right - left) : 1;
    *height = bottom > top ? (uint32_t)(bottom - top) : 1;
}

// One absolute motion at a layout point. The protocol takes unsigned
// coordinates against an extent, so the layout's own origin is subtracted
// here and the result clamped: a point outside the layout is a caller error,
// and the nearest point inside it is a better answer than a wrap-around.
static void _motion_layout(
    struct state *state, struct zwlr_virtual_pointer_v1 *virt_pointer,
    int32_t x, int32_t y, int32_t layout_x, int32_t layout_y,
    uint32_t layout_width, uint32_t layout_height
) {
    int64_t rx = (int64_t)x - layout_x;
    int64_t ry = (int64_t)y - layout_y;

    if (rx < 0) {
        rx = 0;
    }
    if (ry < 0) {
        ry = 0;
    }
    if (rx > layout_width) {
        rx = layout_width;
    }
    if (ry > layout_height) {
        ry = layout_height;
    }

    zwlr_virtual_pointer_v1_motion_absolute(
        virt_pointer, 0, (uint32_t)rx, (uint32_t)ry, layout_width, layout_height
    );
    zwlr_virtual_pointer_v1_frame(virt_pointer);
    wl_display_roundtrip(state->wl_display);
}

void drag_pointer(
    struct state *state, int32_t x1, int32_t y1, int32_t x2, int32_t y2,
    uint32_t duration_ms, enum click click
) {
    if (!state->wl_virtual_pointer_mgr || click == CLICK_NONE) {
        return;
    }

    wl_display_roundtrip(state->wl_display);

    int32_t  layout_x, layout_y;
    uint32_t layout_width, layout_height;
    _layout_box(state, &layout_x, &layout_y, &layout_width, &layout_height);

    struct zwlr_virtual_pointer_v1 *virt_pointer =
        zwlr_virtual_pointer_manager_v1_create_virtual_pointer(
            state->wl_virtual_pointer_mgr,
            ((struct seat *)state->seats.next)->wl_seat
        );

    // No transform is applied to either end. An output's transform describes
    // the step from its own surface coordinates to the screen; layout
    // coordinates are already on the far side of it, which is why the caller
    // can name a point on one output and a point on another in the same
    // breath.
    uint32_t hold_ms = duration_ms / 4;
    if (hold_ms > 50) {
        hold_ms = 50;
    }
    uint32_t travel_ms =
        duration_ms > 2 * hold_ms ? duration_ms - 2 * hold_ms : 0;

    int steps = (int)(travel_ms / 8);
    if (steps < 1) {
        steps = 1;
    }

    _motion_layout(
        state, virt_pointer, x1, y1, layout_x, layout_y, layout_width,
        layout_height
    );

    int btn = 271 + click;

    zwlr_virtual_pointer_v1_button(
        virt_pointer, 0, btn, WL_POINTER_BUTTON_STATE_PRESSED
    );
    zwlr_virtual_pointer_v1_frame(virt_pointer);
    wl_display_roundtrip(state->wl_display);
    _sleep_ms(hold_ms);

    for (int i = 1; i <= steps; i++) {
        // Computed from the endpoints every step rather than accumulated, so
        // the last step lands exactly on (x2, y2) whatever the rounding did on
        // the way -- a drop one pixel short of where you aimed is a drop on
        // the wrong thing.
        int32_t x = x1 + (int32_t)(((int64_t)x2 - x1) * i / steps);
        int32_t y = y1 + (int32_t)(((int64_t)y2 - y1) * i / steps);

        _motion_layout(
            state, virt_pointer, x, y, layout_x, layout_y, layout_width,
            layout_height
        );

        if (i < steps) {
            _sleep_ms(travel_ms / (uint32_t)steps);
        }
    }

    _sleep_ms(hold_ms);
    zwlr_virtual_pointer_v1_button(
        virt_pointer, 0, btn, WL_POINTER_BUTTON_STATE_RELEASED
    );
    zwlr_virtual_pointer_v1_frame(virt_pointer);
    wl_display_roundtrip(state->wl_display);

    zwlr_virtual_pointer_v1_destroy(virt_pointer);
}
