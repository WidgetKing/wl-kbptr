// SPDX-License-Identifier: GPL-3.0-only

#ifndef __STATE_H_INCLUDED__
#define __STATE_H_INCLUDED__

#include "config.h"
#include "fractional-scale-v1-client-protocol.h"
#include "label.h"
#include "screencopy.h"
#include "surface_buffer.h"
#include "utils.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

#include <stdbool.h>
#include <stdint.h>
#include <wayland-client.h>
#include <wayland-util.h>
#include <xkbcommon/xkbcommon.h>

#define NO_AREA_SELECTION     -1
#define HOME_ROW_LEN          8
#define HOME_ROW_LEN_WITH_BTN 11
#define HOME_ROW_BUFFER_LEN   128
#define HOME_ROW_LEFT_CLICK   8
#define HOME_ROW_RIGHT_CLICK  9
#define HOME_ROW_MIDDLE_CLICK 10

// This should cover a initial maximum area with a width and height of 65536
// pixels.
#define BISECT_MAX_HISTORY 16

// Split history of up to a resolution of 65536x65536 assuming equal number of
// divisions each way.
#define SPLIT_MAX_HISTORY 32

#define MAX_NUM_MODES   3
#define NO_MODE_ENTERED -1

struct mode_interface;

struct tile_mode_state {
    struct rect area;

    int sub_area_rows;
    int sub_area_width;
    int sub_area_width_off;

    int sub_area_columns;
    int sub_area_height;
    int sub_area_height_off;

    label_selection_t *label_selection;
    label_symbols_t   *label_symbols;

    cairo_font_face_t *label_font_face;
};

struct floating_mode_state {
    struct rect *areas;
    int          num_areas;

    label_selection_t *label_selection;
    label_symbols_t   *label_symbols;

    cairo_font_face_t *label_font_face;
};

struct bisect_mode_state {
    struct rect areas[BISECT_MAX_HISTORY];
    int         current;

    cairo_font_face_t *label_font_face;
};

struct split_mode_state {
    struct rect areas[SPLIT_MAX_HISTORY];
    int         current;
};

struct output {
    struct wl_list           link; // type: struct output
    struct wl_output        *wl_output;
    struct zxdg_output_v1   *xdg_output;
    char                    *name;
    int32_t                  scale;
    int32_t                  width;
    int32_t                  height;
    int32_t                  x;
    int32_t                  y;
    enum wl_output_transform transform;
};

enum modifier {
    MODIFIER_CTRL  = 1 << 0,
    MODIFIER_ALT   = 1 << 1,
    MODIFIER_SHIFT = 1 << 2,
    MODIFIER_SUPER = 1 << 3,
};

struct seat {
    struct wl_list      link; // type: struct seat
    struct wl_seat     *wl_seat;
    struct wl_keyboard *wl_keyboard;
    struct xkb_context *xkb_context;
    struct xkb_keymap  *xkb_keymap;
    struct xkb_state   *xkb_state;
    struct state       *state;
};

struct state {
    struct config                           config;
    struct wl_display                      *wl_display;
    struct wl_registry                     *wl_registry;
    struct wl_compositor                   *wl_compositor;
    struct wl_shm                          *wl_shm;
    struct zwlr_layer_shell_v1             *wl_layer_shell;
    struct zwlr_virtual_pointer_manager_v1 *wl_virtual_pointer_mgr;
    struct zwp_virtual_keyboard_manager_v1 *wl_virtual_keyboard_mgr;
    struct wp_viewporter                   *wp_viewporter;
    struct wp_viewport                     *wp_viewport;
    struct wp_fractional_scale_manager_v1  *fractional_scale_mgr;
    struct surface_buffer_pool              surface_buffer_pool;
    struct wl_surface                      *wl_surface;
    struct wl_callback                     *wl_surface_callback;
    struct zwlr_layer_surface_v1           *wl_layer_surface;
    bool                                    surface_configured;
#if OPENCV_ENABLED
    struct zwlr_screencopy_manager_v1 *wl_screencopy_manager;
#endif
    struct zxdg_output_manager_v1 *xdg_output_manager;
    struct wl_list                 outputs;
    struct wl_list                 seats;
    struct output                 *current_output;
    uint32_t                       surface_height;
    uint32_t                       surface_width;
    uint32_t                       fractional_scale; // scale / 120
    bool                           running;
    struct rect                    initial_area;
    char                           home_row_buffer[HOME_ROW_BUFFER_LEN];
    char                         **home_row;
    struct rect                    result;
    struct mode_interface         *mode_interfaces[MAX_NUM_MODES];
    void                          *mode_states[MAX_NUM_MODES];
    int                            current_mode;
    enum click                     click;
    bool                           peeking;
    // When the first real frame went out, which is when the intro starts. 0
    // until then.
    int64_t                        intro_start_ms;
    // The transition picked for this overlay, and the seed it plays with.
    const struct transition       *intro;
    uint32_t                       intro_seed;
    // The double-click window. `double_click_sym` is the key that committed
    // the selection, and its being anything but NoSymbol is what says the
    // window is open: the selection is made, its click is out, and the overlay
    // is staying up until `double_click_deadline_ms` to see whether that same
    // key is pressed again.
    xkb_keysym_t                   double_click_sym;
    int64_t                        double_click_deadline_ms;
    // Clicks the loop owes the pointer. A key handler cannot emit one itself:
    // move_pointer round-trips the display, and a round trip from inside a
    // dispatch is a dispatch inside a dispatch.
    int                            pending_clicks;
    // Whether any click has gone out already, so the end of main knows not to
    // make a second one out of the same result.
    bool                           clicked;
    // --only-print. On state rather than a local in main() because the double-
    // click window has to refuse to arm under it, and a virtual pointer that
    // exists is not the same question: --only-print still binds the manager,
    // it just never presses with it.
    bool                           only_print;
    // --modifiers: MODIFIER_* bits held down around every press this run
    // makes, so a click can be a Ctrl click. 0 is a plain click, and is also
    // what keeps a run that was not asked for any from touching the keyboard.
    uint32_t                       modifiers;
    // --modifiers-file: read at each press and, when it can be read, used in
    // place of `modifiers`, so the wrapper can change what a click holds
    // without relaunching the overlay (which is a visible flicker). Missing
    // or empty means none; unparseable keeps `modifiers`.
    const char                    *modifiers_file;
    // --overrides-file: config lines applied over the launch config each time
    // SIGUSR1 arrives, so the wrapper can switch left and right click, tint
    // and all, without relaunching. NULL leaves SIGUSR1 alone.
    const char                    *overrides_file;
};

#endif
