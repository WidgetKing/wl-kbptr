// SPDX-License-Identifier: GPL-3.0-only

#include "utils_wayland.h"

#include "state.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

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

void drag_pointer(
    struct state *state, uint32_t x1, uint32_t y1, uint32_t x2, uint32_t y2,
    uint32_t duration_ms, enum click click
) {
    if (!state->wl_virtual_pointer_mgr || click == CLICK_NONE) {
        return;
    }

    wl_display_roundtrip(state->wl_display);

    struct zwlr_virtual_pointer_v1 *virt_pointer =
        zwlr_virtual_pointer_manager_v1_create_virtual_pointer_with_output(
            state->wl_virtual_pointer_mgr,
            ((struct seat *)state->seats.next)->wl_seat,
            state->current_output->wl_output
        );

    // Both ends are transformed up front, and the path is then interpolated in
    // the transformed space. Every transform here is a rotation or a flip, so
    // interpolating after transforming and transforming after interpolating
    // give the same points -- and doing it once means the width and height
    // _apply_transform swaps are read back only once, from the same call that
    // produced the coordinates using them.
    uint32_t output_width  = state->current_output->width;
    uint32_t output_height = state->current_output->height;
    uint32_t end_width     = state->current_output->width;
    uint32_t end_height    = state->current_output->height;

    _apply_transform(
        &x1, &y1, &output_width, &output_height,
        state->current_output->transform
    );
    _apply_transform(
        &x2, &y2, &end_width, &end_height, state->current_output->transform
    );

    uint32_t hold_ms = duration_ms / 4;
    if (hold_ms > 50) {
        hold_ms = 50;
    }
    uint32_t travel_ms = duration_ms > 2 * hold_ms ? duration_ms - 2 * hold_ms
                                                   : 0;

    int steps = (int)(travel_ms / 8);
    if (steps < 1) {
        steps = 1;
    }

    zwlr_virtual_pointer_v1_motion_absolute(
        virt_pointer, 0, x1, y1, output_width, output_height
    );
    zwlr_virtual_pointer_v1_frame(virt_pointer);
    wl_display_roundtrip(state->wl_display);

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
        uint32_t x = (uint32_t)((int64_t)x1 + ((int64_t)x2 - x1) * i / steps);
        uint32_t y = (uint32_t)((int64_t)y1 + ((int64_t)y2 - y1) * i / steps);

        zwlr_virtual_pointer_v1_motion_absolute(
            virt_pointer, 0, x, y, output_width, output_height
        );
        zwlr_virtual_pointer_v1_frame(virt_pointer);
        wl_display_roundtrip(state->wl_display);

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
