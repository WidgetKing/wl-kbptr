// SPDX-License-Identifier: GPL-3.0-only

#include "utils_wayland.h"

#include "state.h"
#include "log.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

#include <signal.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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

// --modifiers, held down around a press. Applications decide what a Ctrl
// click means from the modifier state their wl_keyboard reports, so this is a
// keyboard: a virtual one on the same seat, which says "these are down" and
// nothing else. No key is ever pressed on it. Only the modifier state moves,
// so no compositor binding can fire and no application sees a keystroke.
//
// It has to be given a keymap before it may say anything, and the keymap is
// the risk. A virtual keyboard with a keymap of its own becomes the seat's
// keymap while it is the active keyboard, and a foreign one there is what
// breaks this program's next start (wtype does exactly that). So it is given
// the seat's own keymap back, the one this process was sent, and the seat is
// left with the keymap it already had.
//
// One keyboard per press, created here and destroyed by modifiers_up, so
// nothing about the keyboard outlives the press it was made for.
static struct zwp_virtual_keyboard_v1 *_modifier_keyboard = NULL;
static uint32_t                        _modifier_locked, _modifier_group;

static uint32_t _modifier_mask(struct xkb_keymap *keymap, uint32_t modifiers) {
    static const struct {
        uint32_t    bit;
        const char *name;
    } names[] = {
        {MODIFIER_CTRL, XKB_MOD_NAME_CTRL},
        {MODIFIER_ALT, XKB_MOD_NAME_ALT},
        {MODIFIER_SHIFT, XKB_MOD_NAME_SHIFT},
        {MODIFIER_SUPER, XKB_MOD_NAME_LOGO},
    };

    uint32_t mask = 0;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (!(modifiers & names[i].bit)) {
            continue;
        }
        xkb_mod_index_t index = xkb_keymap_mod_get_index(keymap, names[i].name);
        if (index != XKB_MOD_INVALID) {
            mask |= 1u << index;
        }
    }
    return mask;
}

// --modifiers-file, read now rather than at launch, so what was toggled while
// the overlay was up is what this press holds. The file is written by someone
// else at any moment: a missing one is none, a garbled one is logged and the
// launch-time list kept.
static void _modifiers_from_file(struct state *state) {
    if (state->modifiers_file == NULL) {
        return;
    }
    char  buf[128] = {0};
    FILE *file     = fopen(state->modifiers_file, "r");
    if (file != NULL) {
        size_t n = fread(buf, 1, sizeof(buf) - 1, file);
        buf[n]   = 0;
        fclose(file);
    }
    uint32_t modifiers;
    if (parse_modifiers(buf, &modifiers) != 0) {
        LOG_ERR("Could not parse %s: keeping --modifiers.", state->modifiers_file);
        return;
    }
    state->modifiers = modifiers;
}

static void modifiers_down(struct state *state) {
    if (_modifier_keyboard != NULL) {
        return;
    }
    _modifiers_from_file(state);
    if (state->modifiers == 0) {
        return;
    }
    if (state->wl_virtual_keyboard_mgr == NULL) {
        LOG_ERR("No virtual keyboard: pressing without the modifiers.");
        return;
    }

    struct seat *seat = (struct seat *)state->seats.next;
    if (seat->xkb_keymap == NULL) {
        LOG_ERR("No keymap from the seat: pressing without the modifiers.");
        return;
    }

    char *keymap =
        xkb_keymap_get_as_string(seat->xkb_keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
    if (keymap == NULL) {
        LOG_ERR("Could not serialise the keymap: pressing without modifiers.");
        return;
    }
    // The terminating NUL is part of what is sent: wl_keyboard.keymap sizes
    // include it, which is why the keymap handler above maps size - 1.
    size_t size = strlen(keymap) + 1;
    int    fd   = memfd_create("wl-kbptr-keymap", MFD_CLOEXEC);
    if (fd < 0 || write(fd, keymap, size) != (ssize_t)size) {
        LOG_ERR("Could not share the keymap: pressing without the modifiers.");
        if (fd >= 0) {
            close(fd);
        }
        free(keymap);
        return;
    }
    free(keymap);

    // Locks and layout are carried over, not zeroed: this keyboard is about
    // to be the one whose state the focused window sees, and a Ctrl click
    // should not also be a click with Caps Lock switched off.
    _modifier_locked = 0;
    _modifier_group  = 0;
    if (seat->xkb_state != NULL) {
        _modifier_locked =
            xkb_state_serialize_mods(seat->xkb_state, XKB_STATE_MODS_LOCKED);
        _modifier_group = xkb_state_serialize_layout(
            seat->xkb_state, XKB_STATE_LAYOUT_EFFECTIVE
        );
    }

    _modifier_keyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(
        state->wl_virtual_keyboard_mgr, seat->wl_seat
    );
    zwp_virtual_keyboard_v1_keymap(
        _modifier_keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, size
    );
    close(fd);
    zwp_virtual_keyboard_v1_modifiers(
        _modifier_keyboard, _modifier_mask(seat->xkb_keymap, state->modifiers),
        0, _modifier_locked, _modifier_group
    );
    wl_display_roundtrip(state->wl_display);
}

// Let go of what modifiers_down held. Safe to call when nothing is held, so
// every press can be followed by it without asking.
static void modifiers_up(struct state *state) {
    if (_modifier_keyboard == NULL) {
        return;
    }

    zwp_virtual_keyboard_v1_modifiers(
        _modifier_keyboard, 0, 0, _modifier_locked, _modifier_group
    );
    wl_display_roundtrip(state->wl_display);
    zwp_virtual_keyboard_v1_destroy(_modifier_keyboard);
    _modifier_keyboard = NULL;
    wl_display_roundtrip(state->wl_display);
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

        modifiers_down(state);
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
        modifiers_up(state);
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

    modifiers_down(state);
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
    modifiers_up(state);

    zwlr_virtual_pointer_v1_destroy(virt_pointer);
}

// A hold is the same press and the same held button as a drag, with the path
// left open: press here, then go where you are told, then let go. It is what
// steering the pointer by hand needs and a drag cannot give, because a drag
// has to know both ends before it starts.
//
// The same rule still holds -- the button must not outlive this process --
// and it is kept the same way: one function owns the press and the release,
// and there is no way out of it that skips the second. The loop ends on EOF,
// on the word `release`, or on a signal; all three fall through to the same
// release below. A signal is why the handler only sets a flag: a release
// emitted from inside a handler would race the one down here.
static volatile sig_atomic_t _hold_stop = 0;

static void _hold_signal(int signal) {
    (void)signal;
    _hold_stop = 1;
}

// Each move is walked rather than jumped, for the reason a drag's path is:
// a client reading drag-and-drop wants a stream of motion under the button,
// not one teleport per keystroke. Short, because a keypress must feel like it
// landed -- long enough to be a movement, over before the key repeats.
#define HOLD_MOVE_STEPS 4
#define HOLD_MOVE_STEP_MS 4

void hold_pointer(
    struct state *state, int32_t x, int32_t y, enum click click, FILE *commands
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

    // Without SA_RESTART, so a signal arriving while we are blocked on the
    // next command breaks the read rather than resuming it. That is what makes
    // a TERM -- the panic key, a crashing run loop -- let go of the button
    // instead of leaving the desktop with it down.
    struct sigaction action = { .sa_handler = _hold_signal };
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGHUP, &action, NULL);

    _motion_layout(
        state, virt_pointer, x, y, layout_x, layout_y, layout_width,
        layout_height
    );

    int btn = 271 + click;

    modifiers_down(state);
    zwlr_virtual_pointer_v1_button(
        virt_pointer, 0, btn, WL_POINTER_BUTTON_STATE_PRESSED
    );
    zwlr_virtual_pointer_v1_frame(virt_pointer);
    wl_display_roundtrip(state->wl_display);
    _sleep_ms(50);

    char line[256];
    while (!_hold_stop && fgets(line, sizeof(line), commands) != NULL) {
        int32_t next_x, next_y;
        if (sscanf(line, "%d %d", &next_x, &next_y) != 2) {
            // Anything that is not a point is the end of the hold. `release`
            // is the word the caller sends; a line it garbled means the same,
            // because a held button is not a thing to keep holding while
            // guessing what was meant.
            break;
        }

        for (int i = 1; i <= HOLD_MOVE_STEPS; i++) {
            int32_t step_x =
                x + (int32_t)(((int64_t)next_x - x) * i / HOLD_MOVE_STEPS);
            int32_t step_y =
                y + (int32_t)(((int64_t)next_y - y) * i / HOLD_MOVE_STEPS);

            _motion_layout(
                state, virt_pointer, step_x, step_y, layout_x, layout_y,
                layout_width, layout_height
            );

            if (i < HOLD_MOVE_STEPS) {
                _sleep_ms(HOLD_MOVE_STEP_MS);
            }
        }

        x = next_x;
        y = next_y;
    }

    // The same pause a drag leaves before letting go: a motion and a release
    // in the same millisecond can land the drop before the client has
    // processed where the pointer got to.
    _sleep_ms(50);
    zwlr_virtual_pointer_v1_button(
        virt_pointer, 0, btn, WL_POINTER_BUTTON_STATE_RELEASED
    );
    zwlr_virtual_pointer_v1_frame(virt_pointer);
    wl_display_roundtrip(state->wl_display);
    modifiers_up(state);

    zwlr_virtual_pointer_v1_destroy(virt_pointer);
}

// A scroll is a wheel turned under the pointer, one notch per command, with
// the modifiers held for as long as it lasts: Ctrl held around a wheel is
// what zooms a browser, Shift what turns it sideways in most toolkits. Unlike
// a hold there is no button to guard, but there are modifiers, and the same
// rule keeps them: one function holds them and lets go of them, and every way
// out -- EOF, `stop`, a signal -- falls through to the same let-go.
//
// Commands, one per line: `up`, `down`, `left` or `right` turns the wheel one
// notch that way; `mods LIST` lets go of what is held and holds LIST instead
// (empty for nothing). Anything else ends the scroll.
//
// The pointer is only moved if a point was given. Without one the wheel turns
// wherever the pointer already is, which is the whole of "scroll here".
static volatile sig_atomic_t _scroll_stop = 0;

static void _scroll_signal(int signal) {
    (void)signal;
    _scroll_stop = 1;
}

// What one notch of a real wheel reports, in surface units: libinput's 15
// degrees per click, which clients treat as one line-ish step.
#define SCROLL_NOTCH 15

void scroll_pointer(
    struct state *state, bool move, int32_t x, int32_t y, FILE *commands
) {
    if (!state->wl_virtual_pointer_mgr) {
        return;
    }

    wl_display_roundtrip(state->wl_display);

    struct zwlr_virtual_pointer_v1 *virt_pointer =
        zwlr_virtual_pointer_manager_v1_create_virtual_pointer(
            state->wl_virtual_pointer_mgr,
            ((struct seat *)state->seats.next)->wl_seat
        );

    struct sigaction action = { .sa_handler = _scroll_signal };
    sigemptyset(&action.sa_mask);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGHUP, &action, NULL);

    if (move) {
        int32_t  layout_x, layout_y;
        uint32_t layout_width, layout_height;
        _layout_box(state, &layout_x, &layout_y, &layout_width, &layout_height);
        _motion_layout(
            state, virt_pointer, x, y, layout_x, layout_y, layout_width,
            layout_height
        );
    }

    modifiers_down(state);

    char line[256];
    while (!_scroll_stop && fgets(line, sizeof(line), commands) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';

        if (strncmp(line, "mods", 4) == 0 &&
            (line[4] == '\0' || line[4] == ' ')) {
            uint32_t modifiers;
            if (parse_modifiers(line + 4, &modifiers) != 0) {
                LOG_ERR("Could not parse '%s': keeping the modifiers.", line);
                continue;
            }
            modifiers_up(state);
            state->modifiers      = modifiers;
            // Said here, so a --modifiers-file cannot overrule it.
            state->modifiers_file = NULL;
            modifiers_down(state);
            continue;
        }

        uint32_t axis;
        int      sign;
        if (strcmp(line, "up") == 0) {
            axis = WL_POINTER_AXIS_VERTICAL_SCROLL;
            sign = -1;
        } else if (strcmp(line, "down") == 0) {
            axis = WL_POINTER_AXIS_VERTICAL_SCROLL;
            sign = 1;
        } else if (strcmp(line, "left") == 0) {
            axis = WL_POINTER_AXIS_HORIZONTAL_SCROLL;
            sign = -1;
        } else if (strcmp(line, "right") == 0) {
            axis = WL_POINTER_AXIS_HORIZONTAL_SCROLL;
            sign = 1;
        } else {
            break;
        }

        // Said as a wheel, with a discrete step, rather than as a finger on a
        // touchpad: a wheel notch is what a browser counts to zoom one level,
        // and what a list moves by a fixed number of lines.
        zwlr_virtual_pointer_v1_axis_source(
            virt_pointer, WL_POINTER_AXIS_SOURCE_WHEEL
        );
        zwlr_virtual_pointer_v1_axis_discrete(
            virt_pointer, 0, axis, wl_fixed_from_int(sign * SCROLL_NOTCH), sign
        );
        zwlr_virtual_pointer_v1_frame(virt_pointer);
        wl_display_roundtrip(state->wl_display);
    }

    modifiers_up(state);
    zwlr_virtual_pointer_v1_destroy(virt_pointer);
}
