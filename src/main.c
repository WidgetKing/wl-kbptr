// SPDX-License-Identifier: GPL-3.0-only

#include "config.h"
#include "fractional-scale-v1-client-protocol.h"
#include "log.h"
#include "mode.h"
#include "state.h"
#include "surface_buffer.h"
#include "utils_cairo.h"
#include "utils_wayland.h"
#include "viewporter-client-protocol.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"

#include <cairo/cairo.h>
#include <getopt.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/inotify.h>
#include <sys/signalfd.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client-protocol.h>
#include <wayland-client.h>
#include <wayland-util.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// What is on screen while the double-click window is open: nothing.
//
// A selection has been made and clicked, and leaving the overlay up would hide
// the very thing just clicked -- which is also the thing you are deciding
// whether to click again. So the surface is cleared and left clear, and marking
// the spot is someone else's business: imthemousenow draws that itself, told
// where the click went by WL_KBPTR_CLICK_REPORT (see report_click).
//
// Buffers are recycled, so the destination still holds the last frame drawn
// into it -- the whole overlay -- and has to be cleared rather than just not
// drawn over.
static void render_double_click_window(cairo_t *cairo) {
    cairo_save(cairo);
    cairo_set_operator(cairo, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cairo, 0, 0, 0, 0);
    cairo_paint(cairo);
    cairo_restore(cairo);
}

static void send_frame(struct state *state) {
    int32_t scale_120 = state->fractional_scale;
    if (scale_120 == 0) {
        // Falling back to the output scale if fractional scale is not received.
        scale_120 =
            (state->current_output == NULL ? 1 : state->current_output->scale) *
            120;
    }

    struct surface_buffer *surface_buffer = get_next_buffer(
        state->wl_shm, &state->surface_buffer_pool,
        state->surface_width * scale_120 / 120,
        state->surface_height * scale_120 / 120
    );
    if (surface_buffer == NULL) {
        return;
    }
    surface_buffer->state = SURFACE_BUFFER_BUSY;

    cairo_t *cairo = surface_buffer->cairo;
    cairo_identity_matrix(cairo);
    cairo_scale(cairo, scale_120 / 120.0, scale_120 / 120.0);
    if (state->double_click_sym != XKB_KEY_NoSymbol) {
        render_double_click_window(cairo);
    } else if (state->peeking) {
        // Peek: draw the overlay into a group and composite the whole thing at
        // a low alpha, so what is underneath can be read through it. Done here
        // rather than by scaling every colour because it is the *overlay* that
        // is being faded, not one of its parts -- dimming, labels, borders and
        // the bisect pointer all go together, and no mode has to know.
        //
        // Buffers are recycled, so the destination still holds the last frame
        // drawn into it. A mode normally overwrites every pixel of it; a group
        // composited on top does not, so it has to be cleared first or the
        // previous frame shows through at full strength underneath.
        cairo_save(cairo);
        cairo_set_operator(cairo, CAIRO_OPERATOR_SOURCE);
        cairo_set_source_rgba(cairo, 0, 0, 0, 0);
        cairo_paint(cairo);
        cairo_restore(cairo);

        cairo_push_group(cairo);
        mode_render(state, cairo);
        cairo_pop_group_to_source(cairo);
        cairo_paint_with_alpha(cairo, state->config.general.peek_alpha);
    } else {
        mode_render(state, cairo);
    }

    wl_surface_set_buffer_scale(state->wl_surface, 1);

    wl_surface_attach(state->wl_surface, surface_buffer->wl_buffer, 0, 0);
    wp_viewport_set_destination(
        state->wp_viewport, state->surface_width, state->surface_height
    );
    wl_surface_damage(
        state->wl_surface, 0, 0, state->surface_width, state->surface_height
    );
    wl_surface_commit(state->wl_surface);
}

/**
 * Send a 1x1px transparent surface.
 *
 * This is used so that the surface is shown on the screen which triggers the
 * `surface.enter()` event callback.
 */
static void send_transparent_frame(struct state *state) {
    struct surface_buffer *surface_buffer =
        get_next_buffer(state->wl_shm, &state->surface_buffer_pool, 1, 1);
    if (surface_buffer == NULL) {
        return;
    }
    surface_buffer->state = SURFACE_BUFFER_BUSY;
    cairo_t *cairo        = surface_buffer->cairo;
    cairo_set_operator(cairo, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cairo, 0, 0, 0, 0);
    cairo_fill(cairo);
    wl_surface_attach(state->wl_surface, surface_buffer->wl_buffer, 0, 0);
    wp_viewport_set_destination(
        state->wp_viewport, state->surface_width, state->surface_height
    );
    wl_surface_damage(state->wl_surface, 0, 0, 1, 1);
    wl_surface_commit(state->wl_surface);
}

static void surface_callback_done(
    void *data, struct wl_callback *callback, uint32_t callback_data
) {
    struct state *state = data;
    send_frame(state);

    wl_callback_destroy(state->wl_surface_callback);
    state->wl_surface_callback = NULL;
}

const struct wl_callback_listener surface_callback_listener = {
    .done = surface_callback_done,
};

static void request_frame(struct state *state) {
    if (state->wl_surface_callback != NULL) {
        return;
    }

    state->wl_surface_callback = wl_surface_frame(state->wl_surface);
    wl_callback_add_listener(
        state->wl_surface_callback, &surface_callback_listener, state
    );
    wl_surface_commit(state->wl_surface);
}

// Hold a key, see through the overlay. The whole surface is the plugin's one
// blind spot: it covers the very thing being aimed at, and a label that lands
// on top of the word you were reading is the common way a selection goes
// wrong. Holding the key fades the overlay almost away without disturbing it,
// so the labels stay where they are and the target underneath becomes legible.
//
// Refused in two cases, both of them "space already means something here":
// a mode that commits on space (bisect, split -- where a peek would fire a
// click instead), and peek_alpha = 1, which is how the feature is turned off.
static void peek_set(struct state *state, bool peeking) {
    if (state->config.general.peek_alpha >= 1 || mode_takes_space(state)) {
        return;
    }
    if (state->peeking == peeking) {
        return;
    }

    state->peeking = peeking;
    request_frame(state);
}

// The mode chain has returned: the selection is made, and normally that is the
// end of the run -- the loop stops, the surface comes down, and main puts the
// pointer on the result and presses.
//
// With mode_click.double_click_ms set, the click is asked for here instead, at
// once, and the overlay stays up for that long watching for `sym` -- the very
// key that just committed -- to be pressed again. A second press clicks a
// second time straight away, and the two presses reach the application close
// enough together for it to read them as one double click.
//
// Emitting the first click now, rather than holding it back until the window
// closes, is the whole point. The first click of a double click IS a single
// click; a mouse does not know which one it is making either. Waiting to find
// out would put the window's whole length in front of every single click in
// the session to buy the occasional double one.
//
// The cost of it is paid by the other side: for as long as the window is open
// the overlay still owns the keyboard, so a key typed immediately after a
// click is eaten rather than reaching what was clicked. That is why any key
// that is not `sym` closes the window at once instead of being swallowed for
// the rest of it -- one lost keystroke rather than a window's worth.
//
// Nothing is armed when there is no click to double: move, both halves of a
// drag, and hold all select with CLICK_NONE, and --only-print has no pointer
// to press with.
static void selection_committed(struct state *state, xkb_keysym_t sym) {
    if (state->config.mode_click.double_click_ms <= 0 ||
        state->click == CLICK_NONE || state->only_print) {
        state->running = false;
        return;
    }

    state->pending_clicks++;
    state->double_click_sym = sym;
    state->double_click_deadline_ms =
        now_ms() + state->config.mode_click.double_click_ms;
    request_frame(state);
}

// A key pressed while the double-click window is open. Everything that arrives
// here arrives after the selection was made and clicked, so no mode is
// listening any more and exactly one key still means anything: the one that
// committed, pressed again. Anything else ends the run.
static void double_click_key(struct state *state, xkb_keysym_t sym) {
    if (sym == state->double_click_sym) {
        state->pending_clicks++;
    }

    state->running = false;
}

// Where a click just went, for whoever else is drawing on the screen.
//
// imthemousenow marks every click with an effect of its own, drawn by a
// separate process that has no way to see the click happen. With
// WL_KBPTR_CLICK_REPORT=<path> set, each click this run makes rewrites that
// file with one line:
//
//     <output name> <x> <y> <ms>
//
// x and y are in the output's own logical coordinates -- the space the overlay
// surface and move_pointer() both work in -- and ms is a monotonic timestamp,
// so the second click of a double click, landing on the same spot, is still a
// change a watcher can see. Unset, nothing is written; a failed write is not
// an error either, since the click itself has already gone out.
static void report_click(struct state *state, int x, int y) {
    const char *path = getenv("WL_KBPTR_CLICK_REPORT");
    if (path == NULL || *path == 0 || state->current_output == NULL) {
        return;
    }

    FILE *file = fopen(path, "w");
    if (file == NULL) {
        return;
    }
    const char *name = state->current_output->name;
    fprintf(
        file, "%s %d %d %lld\n", name ? name : "-", x, y,
        (long long)now_ms()
    );
    fclose(file);
}

bool compute_initial_area(struct state *state, struct rect *initial_area) {
    if (initial_area->w == -1) {
        initial_area->x = 0;
        initial_area->y = 0;
        initial_area->w = state->surface_width;
        initial_area->h = state->surface_height;
    } else {
        if (initial_area->x < 0) {
            initial_area->w += initial_area->x;
            initial_area->x  = 0;
        }

        if (initial_area->y < 0) {
            initial_area->h += initial_area->y;
            initial_area->y  = 0;
        }

        if (initial_area->w + initial_area->x > state->current_output->width) {
            initial_area->w = state->current_output->width - initial_area->x;
        }

        if (initial_area->h + initial_area->y > state->current_output->height) {
            initial_area->h = state->current_output->height - initial_area->y;
        }
    }

    if (initial_area->w <= 0 || initial_area->h <= 0) {
        LOG_ERR(
            "Initial area (%dx%d) is too small.", initial_area->w,
            initial_area->h
        );
        return false;
    }

    return true;
}

static void noop() {}

static void load_home_row(
    struct xkb_keymap *keymap, char **home_row, char *home_row_buffer
) {
    static const xkb_keycode_t key_codes[] = {
        0x26, // a
        0x27, // s
        0x28, // d
        0x29, // f
        0x2c, // j
        0x2d, // k
        0x2e, // l
        0x2f, // m
        0x2a, // g
        0x2b, // h
        0x38, // b
    };

    struct xkb_state *xkb_state   = xkb_state_new(keymap);
    char             *buffer      = home_row_buffer;
    size_t            buffer_size = HOME_ROW_BUFFER_LEN;

    for (int i = 0; i < sizeof(key_codes) / sizeof(key_codes[0]); i++) {
        xkb_keysym_t keysym =
            xkb_state_key_get_one_sym(xkb_state, key_codes[i]);
        int char_len = xkb_keysym_to_utf8(keysym, buffer, buffer_size);
        if (char_len < 0) {
            LOG_ERR("Could not load home row keys. Buffer is too small.");
            exit(1);
        }

        if (char_len == 0) {
            LOG_ERR(
                "0x%x symkey does not have a UTF-8 representation in given "
                "keymap.",
                key_codes[i]
            );
            exit(1);
        }

        home_row[i]  = buffer;
        buffer      += char_len;
        buffer_size -= char_len;
    }

    xkb_state_unref(xkb_state);
}

static void handle_keyboard_keymap(
    void *data, struct wl_keyboard *keyboard, uint32_t format, int fd,
    uint32_t size
) {
    struct seat *seat = data;
    if (seat->xkb_state != NULL) {
        xkb_state_unref(seat->xkb_state);
        seat->xkb_state = NULL;
    }
    if (seat->xkb_keymap != NULL) {
        xkb_keymap_unref(seat->xkb_keymap);
        seat->xkb_keymap = NULL;
    }

    switch (format) {
    case WL_KEYBOARD_KEYMAP_FORMAT_NO_KEYMAP:
        seat->xkb_keymap = xkb_keymap_new_from_names(
            seat->xkb_context, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS
        );
        break;

    case WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1:;
        void *buffer = mmap(NULL, size - 1, PROT_READ, MAP_PRIVATE, fd, 0);
        if (buffer == MAP_FAILED) {
            LOG_ERR("Could not mmap keymap data.");
            return;
        }

        seat->xkb_keymap = xkb_keymap_new_from_buffer(
            seat->xkb_context, buffer, size - 1, XKB_KEYMAP_FORMAT_TEXT_V1,
            XKB_KEYMAP_COMPILE_NO_FLAGS
        );

        munmap(buffer, size - 1);
        close(fd);
        break;
    }

    if (seat->state->config.general.home_row_keys == NULL) {
        load_home_row(
            seat->xkb_keymap, seat->state->home_row,
            seat->state->home_row_buffer
        );
    }
    seat->xkb_state = xkb_state_new(seat->xkb_keymap);
}

// --- key channel -----------------------------------------------------------
// A file the compositor appends keysym names to, one per line, read here
// instead of a wl_keyboard. Enabled with WL_KBPTR_KEY_CHANNEL=<path>.
//
// A plain file rather than a fifo or a socket: the writer is the compositor's
// own config thread, and opening a fifo that has no reader blocks whoever
// opens it -- a wedged compositor is a worse failure than a lost keystroke.
// Appending to a file can never block, in any order of startup or teardown.
static const char *key_channel_path = NULL;
static int         key_channel_fd   = -1;
static int         key_channel_wd   = -1;
static off_t       key_channel_off  = 0;

static void key_channel_open(struct state *state) {
    // Truncate: anything written before this overlay existed was meant for a
    // previous one, and replaying it would type into the wrong overlay.
    int fd = open(key_channel_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) {
        close(fd);
    }

    key_channel_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (key_channel_fd < 0) {
        LOG_ERR("Failed to watch the key channel.");
        return;
    }
    key_channel_wd =
        inotify_add_watch(key_channel_fd, key_channel_path, IN_MODIFY);
    if (key_channel_wd < 0) {
        LOG_ERR("Failed to watch the key channel file.");
        close(key_channel_fd);
        key_channel_fd = -1;
    }
}

// One line of the channel, handled exactly as a key off a wl_keyboard.
//
// A bare keysym name is a press, which is every line the compositor wrote
// before peeking existed and is why the prefix marks the new case rather than
// both: an older imthemousenow driving a newer wl-kbptr keeps working, and its
// keys are not silently reinterpreted.
//
//   space    press
//   -space   release
//
// Only the keys that care about being let go are worth relaying twice, so a
// release for anything else simply finds nothing to do.
static void key_channel_handle_line(struct state *state, char *line) {
    bool pressed = true;
    if (*line == '-') {
        pressed = false;
        line++;
    }

    if (*line == 0) {
        return;
    }

    char         text[64];
    xkb_keysym_t key_sym = xkb_keysym_from_name(line, XKB_KEYSYM_NO_FLAGS);
    if (key_sym == XKB_KEY_NoSymbol) {
        key_sym =
            xkb_keysym_from_name(line, XKB_KEYSYM_CASE_INSENSITIVE);
    }
    if (key_sym == XKB_KEY_NoSymbol) {
        return;
    }
    xkb_keysym_to_utf8(key_sym, text, sizeof(text));

    // Before the peek, because the key a grid commits on is space: once the
    // window is open, a space is a second click rather than a look underneath.
    if (state->double_click_sym != XKB_KEY_NoSymbol) {
        if (pressed) {
            double_click_key(state, key_sym);
        }
        return;
    }

    if (key_sym == XKB_KEY_space && !mode_takes_space(state)) {
        peek_set(state, pressed);
        return;
    }

    if (!pressed) {
        return;
    }

    bool redraw = mode_handle_key(state, key_sym, text);
    if (has_last_mode_returned(state)) {
        selection_committed(state, key_sym);
    } else if (redraw) {
        request_frame(state);
    }
}

// Drain whatever the compositor appended since the last read.
static void key_channel_drain(struct state *state) {
    char buf[4096];
    while (read(key_channel_fd, buf, sizeof(buf)) > 0) {}

    int fd = open(key_channel_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return;
    }
    if (lseek(fd, key_channel_off, SEEK_SET) < 0) {
        close(fd);
        return;
    }

    char    line[256];
    size_t  len = 0;
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        key_channel_off += n;
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                line[len] = 0;
                key_channel_handle_line(state, line);
                len = 0;
            } else if (len < sizeof(line) - 1) {
                line[len++] = buf[i];
            }
        }
    }
    close(fd);
}

static void handle_keyboard_key(
    void *data, struct wl_keyboard *keyboard, uint32_t serial, uint32_t time,
    uint32_t key, uint32_t key_state
) {
    struct seat        *seat = data;
    char                text[64];
    const xkb_keycode_t key_code = key + 8;
    const xkb_keysym_t  key_sym =
        xkb_state_key_get_one_sym(seat->xkb_state, key_code);
    xkb_keysym_to_utf8(key_sym, text, sizeof(text));

    // Before the peek, for the reason given on the channel's copy of this: a
    // grid commits on space, so inside the window a space is a second click.
    if (seat->state->double_click_sym != XKB_KEY_NoSymbol) {
        if (key_state == WL_KEYBOARD_KEY_STATE_PRESSED) {
            double_click_key(seat->state, key_sym);
        }
        return;
    }

    if (key_sym == XKB_KEY_space && !mode_takes_space(seat->state)) {
        peek_set(
            seat->state, key_state == WL_KEYBOARD_KEY_STATE_PRESSED
        );
        return;
    }

    if (key_state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        bool redraw = mode_handle_key(seat->state, key_sym, text);
        if (has_last_mode_returned(seat->state)) {
            selection_committed(seat->state, key_sym);
        } else if (redraw) {
            request_frame(seat->state);
        }
    }
}

static void handle_keyboard_modifiers(
    void *data, struct wl_keyboard *keyboard, uint32_t serial,
    uint32_t mods_depressed, uint32_t mods_latched, uint32_t mods_locked,
    uint32_t group
) {
    struct seat *seat = data;
    xkb_state_update_mask(
        seat->xkb_state, mods_depressed, mods_latched, mods_locked, 0, 0, group
    );
}

static const struct wl_keyboard_listener wl_keyboard_listener = {
    .keymap      = handle_keyboard_keymap,
    .enter       = noop,
    .leave       = noop,
    .key         = handle_keyboard_key,
    .modifiers   = handle_keyboard_modifiers,
    .repeat_info = noop,
};

static void handle_seat_capabilities(
    void *data, struct wl_seat *wl_seat, uint32_t capabilities
) {
    struct seat *seat = data;
    if (capabilities & WL_SEAT_CAPABILITY_KEYBOARD) {
        seat->wl_keyboard = wl_seat_get_keyboard(seat->wl_seat);
        wl_keyboard_add_listener(
            seat->wl_keyboard, &wl_keyboard_listener, data
        );
    }
}

const struct wl_seat_listener wl_seat_listener = {
    .name         = noop,
    .capabilities = handle_seat_capabilities,
};

static void free_seats(struct wl_list *seats) {
    struct seat *seat;
    struct seat *tmp;
    wl_list_for_each_safe (seat, tmp, seats, link) {
        if (seat->wl_keyboard != NULL) {
            wl_keyboard_destroy(seat->wl_keyboard);
        }

        if (seat->xkb_state != NULL) {
            xkb_state_unref(seat->xkb_state);
        }
        if (seat->xkb_keymap != NULL) {
            xkb_keymap_unref(seat->xkb_keymap);
        }
        xkb_context_unref(seat->xkb_context);

        wl_seat_destroy(seat->wl_seat);
        wl_list_remove(&seat->link);
        free(seat);
    }
}

static void free_outputs(struct wl_list *outputs) {
    struct output *output;
    struct output *tmp;
    wl_list_for_each_safe (output, tmp, outputs, link) {
        wl_output_destroy(output->wl_output);
        zxdg_output_v1_destroy(output->xdg_output);
        wl_list_remove(&output->link);
        free(output->name);
        free(output);
    }
}

static struct output *find_output_from_wl_output(
    struct wl_list *outputs, struct wl_output *wl_output
) {
    struct output *output;
    wl_list_for_each (output, outputs, link) {
        if (wl_output == output->wl_output) {
            return output;
        }
    }

    return NULL;
}

static void
handle_output_scale(void *data, struct wl_output *wl_output, int32_t scale) {
    struct output *output = data;
    output->scale         = scale;
}

static void handle_output_geometry(
    void *data, struct wl_output *wl_output, int32_t x, int32_t y,
    int32_t physical_width, int32_t physical_height, int32_t subpixel,
    const char *make, const char *model, int32_t transform
) {
    struct output *output = data;
    output->transform     = transform;
}

const static struct wl_output_listener output_listener = {
    .name        = noop,
    .geometry    = handle_output_geometry,
    .mode        = noop,
    .scale       = handle_output_scale,
    .description = noop,
    .done        = noop,
};

static void handle_xdg_output_logical_position(
    void *data, struct zxdg_output_v1 *xdg_output, int32_t x, int32_t y
) {
    struct output *output = data;
    output->x             = x;
    output->y             = y;
}

static void handle_xdg_output_logical_size(
    void *data, struct zxdg_output_v1 *xdg_output, int32_t w, int32_t h
) {
    struct output *output = data;
    output->width         = w;
    output->height        = h;
}

static void handle_xdg_output_name(
    void *data, struct zxdg_output_v1 *xdg_output, const char *name
) {
    struct output *output = data;
    output->name          = strdup(name);
}

const static struct zxdg_output_v1_listener xdg_output_listener = {
    .logical_position = handle_xdg_output_logical_position,
    .logical_size     = handle_xdg_output_logical_size,
    .done             = noop,
    .name             = handle_xdg_output_name,
    .description      = noop,
};

static void load_xdg_outputs(struct state *state) {
    struct output *output;
    wl_list_for_each (output, &state->outputs, link) {
        output->xdg_output = zxdg_output_manager_v1_get_xdg_output(
            state->xdg_output_manager, output->wl_output
        );
        zxdg_output_v1_add_listener(
            output->xdg_output, &xdg_output_listener, output
        );
    }

    wl_display_roundtrip(state->wl_display);
}

static void enter_first_mode(struct state *state) {
    if (state->current_mode == NO_MODE_ENTERED) {
        if (!compute_initial_area(state, &state->initial_area)) {
            state->running = false;
            return;
        }

        LOG_DEBUG(
            "Initial area: %dx%d+%d+%d", state->initial_area.w,
            state->initial_area.h, state->initial_area.x, state->initial_area.y
        );

        LOG_DEBUG(
            "Output: %s (position: %dx%d+%d+%d, transform: %d)",
            state->current_output->name, state->current_output->width,
            state->current_output->height, state->current_output->x,
            state->current_output->y, state->current_output->transform
        );

        enter_next_mode(state, state->initial_area);

        if (state->running) {
            send_frame(state);
        }
    }
}

static void handle_surface_enter(
    void *data, struct wl_surface *surface, struct wl_output *wl_output
) {
    struct state  *state = data;
    struct output *output =
        find_output_from_wl_output(&state->outputs, wl_output);
    state->current_output = output;

    if (state->surface_configured) {
        enter_first_mode(state);
    }
}

static const struct wl_surface_listener surface_listener = {
    .enter                      = handle_surface_enter,
    .leave                      = noop,
    .preferred_buffer_transform = noop,
    .preferred_buffer_scale     = noop,
};

static void handle_registry_global(
    void *data, struct wl_registry *registry, uint32_t name,
    const char *interface, uint32_t version
) {
    struct state *state = data;

    if (strcmp(interface, wl_compositor_interface.name) == 0) {
        state->wl_compositor =
            wl_registry_bind(registry, name, &wl_compositor_interface, 4);

    } else if (strcmp(interface, wl_shm_interface.name) == 0) {
        state->wl_shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);

    } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
        state->wl_layer_shell =
            wl_registry_bind(registry, name, &zwlr_layer_shell_v1_interface, 2);

    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        struct seat *seat = calloc(1, sizeof(struct seat));
        seat->wl_seat = wl_registry_bind(registry, name, &wl_seat_interface, 7);
        seat->wl_keyboard = NULL;
        seat->xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        seat->xkb_state   = NULL;
        seat->xkb_keymap  = NULL;
        seat->state       = state;

        wl_seat_add_listener(seat->wl_seat, &wl_seat_listener, seat);
        wl_list_insert(&state->seats, &seat->link);

    } else if (strcmp(interface, wl_output_interface.name) == 0) {
        struct wl_output *wl_output =
            wl_registry_bind(registry, name, &wl_output_interface, 3);
        struct output *output = calloc(1, sizeof(struct output));
        output->wl_output     = wl_output;
        output->scale         = 1;

        wl_output_add_listener(output->wl_output, &output_listener, output);
        wl_list_insert(&state->outputs, &output->link);

    } else if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0) {
        state->xdg_output_manager = wl_registry_bind(
            registry, name, &zxdg_output_manager_v1_interface, 2
        );

    } else if (strcmp(
                   interface, zwlr_virtual_pointer_manager_v1_interface.name
               ) == 0) {
        state->wl_virtual_pointer_mgr = wl_registry_bind(
            registry, name, &zwlr_virtual_pointer_manager_v1_interface, 2
        );
    } else if (strcmp(
                   interface, zwp_virtual_keyboard_manager_v1_interface.name
               ) == 0) {
        state->wl_virtual_keyboard_mgr = wl_registry_bind(
            registry, name, &zwp_virtual_keyboard_manager_v1_interface, 1
        );
    } else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
        state->wp_viewporter =
            wl_registry_bind(registry, name, &wp_viewporter_interface, 1);
    } else if (strcmp(
                   interface, wp_fractional_scale_manager_v1_interface.name
               ) == 0) {
        state->fractional_scale_mgr = wl_registry_bind(
            registry, name, &wp_fractional_scale_manager_v1_interface, 1
        );
#if OPENCV_ENABLED
    } else if (strcmp(interface, zwlr_screencopy_manager_v1_interface.name) ==
               0) {
        state->wl_screencopy_manager = wl_registry_bind(
            registry, name, &zwlr_screencopy_manager_v1_interface, 1
        );
#endif
    }
}

const struct wl_registry_listener wl_registry_listener = {
    .global        = handle_registry_global,
    .global_remove = noop,
};

static void handle_layer_surface_configure(
    void *data, struct zwlr_layer_surface_v1 *layer_surface, uint32_t serial,
    uint32_t width, uint32_t height
) {
    struct state *state   = data;
    state->surface_width  = width;
    state->surface_height = height;
    zwlr_layer_surface_v1_ack_configure(layer_surface, serial);

    if (state->current_output != NULL) {
        enter_first_mode(state);
    } else if (!state->surface_configured) {
        send_transparent_frame(state);
    }

    state->surface_configured = true;
}

static void handle_layer_surface_closed(
    void *data, struct zwlr_layer_surface_v1 *layer_surface
) {
    struct state *state = data;
    state->running      = false;
}

const struct zwlr_layer_surface_v1_listener wl_layer_surface_listener = {
    .configure = handle_layer_surface_configure,
    .closed    = handle_layer_surface_closed,
};

static void fractional_scale_preferred(
    void *data, struct wp_fractional_scale_v1 *fractional_scale, uint32_t scale
) {
    struct state *state     = data;
    int32_t       old_scale = state->fractional_scale;
    state->fractional_scale = scale;

    if (old_scale != 0 && old_scale != scale) {
        request_frame(state);
    }
}

const struct wp_fractional_scale_v1_listener fractional_scale_listener = {
    .preferred_scale = fractional_scale_preferred,
};

static struct output *
find_output_from_rect(struct state *state, struct rect *rect) {
    struct output *output;
    wl_list_for_each (output, &state->outputs, link) {
        if (output->x <= rect->x && output->y <= rect->y &&
            output->x + output->width > rect->x &&
            output->y + output->height > rect->y) {
            return output;
        }
    }

    return NULL;
}

static struct output *find_output_by_name(struct state *state, char *name) {
    struct output *output;
    wl_list_for_each (output, &state->outputs, link) {
        if (strcmp(output->name, name) == 0) {
            return output;
        }
    }

    return NULL;
}

static void print_result(struct state *state) {
    char click;
    switch (state->click) {
    case CLICK_LEFT_BTN:
        click = 'l';
        break;
    case CLICK_MIDDLE_BTN:
        click = 'm';
        break;
    case CLICK_RIGHT_BTN:
        click = 'r';
        break;
    case CLICK_NONE:
        click = 'n';
    }

    printf(
        "%dx%d+%d+%d +%d+%d %c\n", state->result.w, state->result.h,
        state->result.x, state->result.y, state->current_output->x,
        state->current_output->y, click
    );
}

// --modifiers: a list of ctrl, alt, shift and super, in any order, separated
// by commas or whitespace (so --modifiers-file can hold what the wrapper's
// session writes, "ctrl alt\n"). Empty is allowed and means none, so a caller
// can always pass the flag rather than decide whether to. A name it does not
// know is an error rather than skipped: a click made without the Ctrl that was
// asked for is a different click, not a slightly worse one.
int parse_modifiers(const char *list, uint32_t *modifiers) {
    static const struct {
        const char *name;
        uint32_t    bit;
    } names[] = {
        {"ctrl", MODIFIER_CTRL},
        {"alt", MODIFIER_ALT},
        {"shift", MODIFIER_SHIFT},
        {"super", MODIFIER_SUPER},
    };
    static const char *separators = ", \t\r\n";

    *modifiers = 0;
    const char *start = list;
    while (*(start += strspn(start, separators)) != 0) {
        size_t len   = strcspn(start, separators);
        bool   known = false;

        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
            if (strlen(names[i].name) == len &&
                strncmp(names[i].name, start, len) == 0) {
                *modifiers |= names[i].bit;
                known       = true;
            }
        }
        if (!known) {
            return 1;
        }
        start += len;
    }

    return 0;
}

static void print_usage() {
    puts("wl-kbptr [OPTION...]\n");

    puts(" -h, --help          show this help");
    puts(" --help-config       show help on configuration");
    puts(" -v, --version       show version");
    puts(" -c, --config=FILE   use given configuration file");
    puts(" -r, --restrict=AREA restrict to given area (wxh+x+y)");
    puts(" -o, --option        set configuration option");
    puts(" -O, --output        specify display output to use");
    puts(" -p, --only-print    only print, don't move the cursor or click");
    puts(" --hold=X,Y          don't show an overlay: press at X,Y and hold");
    puts("                     it, moving to each 'x y' line read on stdin,");
    puts("                     releasing on EOF or 'release'");
    puts(" --drag=PATH         don't show an overlay: press, travel and");
    puts("                     release along x1,y1,x2,y2,duration_ms, in");
    puts("                     layout coordinates");
    puts(" --modifiers=LIST    hold ctrl,alt,shift,super down around every");
    puts("                     press: a click, a drag, a hold");
    puts(" --modifiers-file=F  read that list from F at each press instead,");
    puts("                     so it can change while the overlay is up");
    puts(" --overrides-file=F  on SIGUSR1, rebuild the configuration as at");
    puts("                     launch, apply each section.key=value line of");
    puts("                     F on top, and redraw");
}

// What the configuration was built from at launch, kept so --overrides-file
// can build it again from scratch: a reload is always "launch config plus the
// file", never "whatever was last loaded plus the file", so an empty file
// puts the launch config back.
static char  *launch_config_filename = NULL;
static char **launch_cli_configs     = NULL;
static int    launch_num_cli_configs = 0;

// Rebuilds the configuration and swaps it in. Anything that fails to parse
// throws the whole attempt away: the config in force stays in force and one
// error is logged, as --modifiers-file does. Only what is read afresh at each
// frame or each press -- colours, the button, the double-click window --
// changes what the overlay does; things read once at mode entry (label
// symbols, the floating source) or at launch (the mode chain) stay as they
// were.
static void reload_overrides(struct state *state) {
    struct config        fresh;
    struct config_loader loader;
    config_set_default(&fresh);
    config_loader_init(&loader, &fresh);

    bool ok = config_loader_load_file(&loader, launch_config_filename) == 0;
    for (int i = 0; ok && i < launch_num_cli_configs; i++) {
        ok = config_loader_load_cli_param(&loader, launch_cli_configs[i]) == 0;
    }

    FILE *f = ok ? fopen(state->overrides_file, "r") : NULL;
    if (f != NULL) {
        char   *line = NULL;
        size_t  cap  = 0;
        ssize_t len;
        while (ok && (len = getline(&line, &cap, f)) >= 0) {
            while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
                line[--len] = '\0';
            }
            if (len > 0) {
                ok = config_loader_load_cli_param(&loader, line) == 0;
            }
        }
        free(line);
        fclose(f);
    }

    if (!ok) {
        LOG_ERR("Could not apply --overrides-file; keeping the configuration.");
        config_free_values(&fresh);
        return;
    }

    config_free_values(&state->config);
    state->config = fresh;
    // The one pointer into config that outlives a frame.
    if (state->config.general.home_row_keys != NULL) {
        state->home_row = state->config.general.home_row_keys;
    }
    request_frame(state);
}

static void print_version() {
    printf("wl-kbptr %s", VERSION);
#if OPENCV_ENABLED
    printf(" (opencv)");
#endif
    puts("");
}

int main(int argc, char **argv) {
    // Set by whoever launches us when the compositor, not a wl_keyboard, is
    // the thing that will be feeding us keys. See key_channel_open.
    key_channel_path = getenv("WL_KBPTR_KEY_CHANNEL");
    if (key_channel_path != NULL && *key_channel_path == 0) {
        key_channel_path = NULL;
    }

    struct state state = {
        .wl_display          = NULL,
        .wl_registry         = NULL,
        .wl_compositor       = NULL,
        .wl_shm              = NULL,
        .wl_layer_shell      = NULL,
        .wl_surface          = NULL,
        .wl_surface_callback = NULL,
        .wl_layer_surface    = NULL,
        .surface_configured  = false,
#if OPENCV_ENABLED
        .wl_screencopy_manager = NULL,
#endif
        .wp_viewporter        = NULL,
        .fractional_scale_mgr = NULL,
        .running              = true,
        .fractional_scale     = 0,
        .result               = (struct rect){-1, -1, -1, -1},
        .initial_area         = (struct rect){-1, -1, -1, -1},
        .home_row = (char *[]){"", "", "", "", "", "", "", "", "", "", ""},
        .click    = CLICK_NONE,
        // Anything but NoSymbol means the double-click window is open, so this
        // is what says it is not.
        .double_click_sym = XKB_KEY_NoSymbol,
    };

    config_set_default(&state.config);
    struct config_loader config_loader;
    config_loader_init(&config_loader, &state.config);

    static struct option long_options[] = {
        {"help", no_argument, 0, 'h'},
        {"help-config", no_argument, 0, 'H'},
        {"version", no_argument, 0, 'v'},
        {"restrict", required_argument, 0, 'r'},
        {"config", required_argument, 0, 'c'},
        {"output", required_argument, 0, 'O'},
        {"only-print", no_argument, 0, 'p'},
        {"drag", required_argument, 0, 'D'},
        {"hold", required_argument, 0, 'L'},
        {"modifiers", required_argument, 0, 'M'},
        {"modifiers-file", required_argument, 0, 'F'},
        {"overrides-file", required_argument, 0, 'Y'},
        {NULL, 0, NULL, 0}
    };

    int    num_cli_configs      = 0;
    char **cli_configs          = malloc(10 * sizeof(char*));
    int    cli_configs_len      = 10;
    int    option_char          = 0;
    int    option_index         = 0;
    char  *config_filename      = NULL;
    char  *selected_output_name = NULL;
    // A drag is not a selection: there is no overlay, no keyboard and no mode
    // chain, only a path to walk. -1 means no --drag was given.
    int drag_x1 = -1, drag_y1 = -1, drag_x2 = -1, drag_y2 = -1;
    int drag_duration_ms = -1;
    // A hold is the same idea with the far end left open: it presses where it
    // is told and then reads where to go. `held` is what says one was asked
    // for, because 0,0 is a real point.
    int  hold_x = 0, hold_y = 0;
    bool held = false;
    while ((option_char = getopt_long(
                argc, argv, "hvr:o:c:O:RpD:L:M:", long_options, &option_index
            )) != -1) {
        switch (option_char) {
        case 'h':
            print_usage();
            config_free_values(&state.config);
            return 0;

        case 'v':
            print_version();
            config_free_values(&state.config);
            return 0;

        case 'r':
            if (sscanf(
                    optarg, "%dx%d+%d+%d", &state.initial_area.w,
                    &state.initial_area.h, &state.initial_area.x,
                    &state.initial_area.y
                ) != 4) {
                LOG_ERR("Could not parse --restrict argument.");
                return 1;
            }
            break;

        case 'o':
            if (num_cli_configs >= cli_configs_len) {
                cli_configs_len += 10;
                cli_configs =
                    realloc(cli_configs, cli_configs_len * sizeof(char*));
            }
            cli_configs[num_cli_configs++] = optarg;
            break;

        case 'c':
            config_filename = strdup(optarg);
            break;

        case 'H':
            print_default_config();
            config_free_values(&state.config);
            return 0;

        case 'O':
            selected_output_name = strdup(optarg);
            break;

        case 'p':
            state.only_print = true;
            break;

        case 'D':
            if (sscanf(
                    optarg, "%d,%d,%d,%d,%d", &drag_x1, &drag_y1, &drag_x2,
                    &drag_y2, &drag_duration_ms
                ) != 5) {
                LOG_ERR("Could not parse --drag argument.");
                return 1;
            }
            if (drag_duration_ms < 0) {
                LOG_ERR("--drag duration must not be negative.");
                return 1;
            }
            break;

        case 'L':
            if (sscanf(optarg, "%d,%d", &hold_x, &hold_y) != 2) {
                LOG_ERR("Could not parse --hold argument.");
                return 1;
            }
            held = true;
            break;

        case 'M':
            if (parse_modifiers(optarg, &state.modifiers) != 0) {
                LOG_ERR("Could not parse --modifiers argument.");
                return 1;
            }
            break;

        case 'F':
            state.modifiers_file = optarg;
            break;

        case 'Y':
            state.overrides_file = optarg;
            break;

        default:
            LOG_ERR("Unknown argument.");
            config_free_values(&state.config);
            return 1;
        }
    }

    int err = config_loader_load_file(&config_loader, config_filename);
    if (err) {
        LOG_ERR("Failed to read configuration file.");
        return 1;
    }
    if (state.overrides_file != NULL) {
        launch_config_filename = config_filename;
    } else if (config_filename != NULL) {
        free(config_filename);
        config_filename = NULL;
    }

    for (int i = 0; i < num_cli_configs; i++) {
        if (config_loader_load_cli_param(&config_loader, cli_configs[i])) {
            return 1;
        }
    }
    if (state.overrides_file != NULL) {
        launch_cli_configs     = cli_configs;
        launch_num_cli_configs = num_cli_configs;
    } else {
        free(cli_configs);
    }
    cli_configs = NULL;

    if (state.config.general.home_row_keys != NULL) {
        state.home_row = state.config.general.home_row_keys;
    }

    if (load_modes(&state, state.config.general.modes) != 0) {
        LOG_ERR("Could not load modes.");
        return 1;
    }

    wl_list_init(&state.outputs);
    wl_list_init(&state.seats);

    state.wl_display = wl_display_connect(NULL);
    if (state.wl_display == NULL) {
        LOG_ERR("Failed to connect to Wayland compositor.");
        return 1;
    }

    state.wl_registry = wl_display_get_registry(state.wl_display);
    if (state.wl_registry == NULL) {
        LOG_ERR("Failed to get Wayland registry.");
        return 1;
    }

    wl_registry_add_listener(state.wl_registry, &wl_registry_listener, &state);
    wl_display_roundtrip(state.wl_display);

    if (state.wl_compositor == NULL) {
        LOG_ERR("Failed to get wl_compositor object.");
        return 1;
    }

    if (state.wl_shm == NULL) {
        LOG_ERR("Failed to get wl_shm object.");
        return 1;
    }

    if (state.wl_layer_shell == NULL) {
        LOG_ERR("Failed to get zwlr_layer_shell_v1 object.");
        return 1;
    }

    if (state.wl_virtual_pointer_mgr == NULL && !state.only_print) {
        LOG_ERR("Failed to get wlr_virtual_pointer_manager_v1 object.");
        return 1;
    }

    if (state.xdg_output_manager == NULL) {
        LOG_ERR("Failed to get xdg_output_manager object.");
        return 1;
    }

    if (state.wp_viewporter == NULL) {
        LOG_ERR("Failed to get wp_viewporter object.");
        return 1;
    }

    load_xdg_outputs(&state);

    // This round trip should load the keymap which is needed to determine the
    // home row keys.
    wl_display_roundtrip(state.wl_display);

    if (selected_output_name) {
        state.current_output =
            find_output_by_name(&state, selected_output_name);

        if (!state.current_output) {
            LOG_ERR("Could not find output '%s'.", selected_output_name);
            return 1;
        }

        free(selected_output_name);
        selected_output_name = NULL;
    } else if (state.initial_area.w != -1) {
        state.current_output =
            find_output_from_rect(&state, &state.initial_area);

        if (!state.current_output) {
            LOG_ERR("Could not find output containing given area.");
            return 1;
        }

        state.initial_area.x -= state.current_output->x;
        state.initial_area.y -= state.current_output->y;
    }

    // --drag stops here, before anything is drawn. It shares everything above
    // -- the registry, the seat, the outputs -- and needs none of what
    // follows: no surface, no keyboard grab, no mode chain. The caller has
    // already decided both ends; this only walks between them.
    //
    // Its coordinates are layout coordinates -- the space the compositor lays
    // its outputs out in, which is the only space a path over two of them can
    // be said in -- so --output has nothing left to say about a drag.
    // A hold stops in the same place and for the same reasons, and is steered
    // from outside instead of planned in advance: stdin says where to go next
    // for as long as the button is down.
    if (held) {
        hold_pointer(
            &state, hold_x, hold_y, state.config.mode_click.button, stdin
        );

        config_free_values(&state.config);
        return 0;
    }

    if (drag_duration_ms >= 0) {
        drag_pointer(
            &state, drag_x1, drag_y1, drag_x2, drag_y2, drag_duration_ms,
            state.config.mode_click.button
        );

        config_free_values(&state.config);
        return 0;
    }

    surface_buffer_pool_init(&state.surface_buffer_pool);

    state.wl_surface = wl_compositor_create_surface(state.wl_compositor);
    wl_surface_add_listener(state.wl_surface, &surface_listener, &state);
    state.wl_layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        state.wl_layer_shell, state.wl_surface,
        state.current_output == NULL ? NULL : state.current_output->wl_output,
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "selection"
    );
    zwlr_layer_surface_v1_add_listener(
        state.wl_layer_surface, &wl_layer_surface_listener, &state
    );
    zwlr_layer_surface_v1_set_exclusive_zone(state.wl_layer_surface, -1);
    zwlr_layer_surface_v1_set_anchor(
        state.wl_layer_surface, ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                    ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT |
                                    ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                    ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM
    );
    // Taking keyboard focus is what dismisses an xdg-popup: the compositor
    // drops the popup's seat grab when a layer surface with any keyboard
    // interactivity but NONE maps, and the client is told its popup is done.
    // With a key channel we are fed keys by the compositor instead and never
    // ask for focus, so a menu or an extension popup stays open underneath.
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        state.wl_layer_surface, key_channel_path == NULL
    );

    struct wp_fractional_scale_v1 *fractional_scale = NULL;
    if (state.fractional_scale_mgr) {
        fractional_scale = wp_fractional_scale_manager_v1_get_fractional_scale(
            state.fractional_scale_mgr, state.wl_surface
        );
        wp_fractional_scale_v1_add_listener(
            fractional_scale, &fractional_scale_listener, &state
        );
    }

    state.wp_viewport =
        wp_viewporter_get_viewport(state.wp_viewporter, state.wl_surface);

    struct wl_region *wl_region =
        wl_compositor_create_region(state.wl_compositor);
    wl_region_add(wl_region, 0, 0, 0, 0);
    wl_surface_set_input_region(state.wl_surface, wl_region);

    wl_surface_commit(state.wl_surface);

    // One loop, whether or not there is a key channel. It used to be two --
    // a bare wl_display_dispatch when the keyboard was the only source, and
    // this poll when the channel was a second one -- and the bare version
    // cannot be given a deadline, which the double-click window needs. Rather
    // than keep two loops and teach only one of them to wake up on time, the
    // poll does both jobs: with no channel, key_channel_fd stays -1 and it
    // simply waits on one fd.
    //
    // This is the standard Wayland prepare/read dance: without it, events
    // queued between the poll and the read are events we sleep through.
    if (key_channel_path != NULL) {
        key_channel_open(&state);
    }

    // SIGUSR1 means "reload the overrides", but only when asked for with
    // --overrides-file. Without it the signal keeps its default action, as in
    // a stock build, which is why the wrapper probes before sending it.
    int signal_fd = -1;
    if (state.overrides_file != NULL) {
        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGUSR1);
        if (sigprocmask(SIG_BLOCK, &mask, NULL) == 0) {
            signal_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
        }
        if (signal_fd < 0) {
            LOG_ERR("Could not listen for SIGUSR1; overrides will not reload.");
        }
    }

    {
        // A -1 fd is ignored by poll, so all three are always passed.
        struct pollfd fds[3] = {
            {.fd = wl_display_get_fd(state.wl_display), .events = POLLIN},
            {.fd = key_channel_fd, .events = POLLIN},
            {.fd = signal_fd, .events = POLLIN},
        };

        while (state.running) {
            while (wl_display_prepare_read(state.wl_display) != 0) {
                if (wl_display_dispatch_pending(state.wl_display) < 0) {
                    goto loop_done;
                }
            }
            if (wl_display_flush(state.wl_display) < 0 && errno != EAGAIN) {
                wl_display_cancel_read(state.wl_display);
                goto loop_done;
            }

            // Wait forever unless a double-click window is open, in which case
            // only until it closes. Clamped at 0 rather than left negative: a
            // deadline already past must not become "no timeout".
            int timeout = -1;
            if (state.double_click_sym != XKB_KEY_NoSymbol) {
                int64_t left = state.double_click_deadline_ms - now_ms();
                timeout      = left > 0 ? (int)left : 0;
            }

            if (poll(fds, 3, timeout) < 0) {
                wl_display_cancel_read(state.wl_display);
                if (errno == EINTR) {
                    continue;
                }
                goto loop_done;
            }

            if (fds[0].revents & POLLIN) {
                if (wl_display_read_events(state.wl_display) < 0) {
                    goto loop_done;
                }
            } else {
                wl_display_cancel_read(state.wl_display);
            }

            if (wl_display_dispatch_pending(state.wl_display) < 0) {
                goto loop_done;
            }
            if (state.running && (fds[1].revents & POLLIN)) {
                key_channel_drain(&state);
            }
            if (state.running && (fds[2].revents & POLLIN)) {
                struct signalfd_siginfo info;
                bool                    got = false;
                while (read(signal_fd, &info, sizeof(info)) == sizeof(info)) {
                    got = true;
                }
                if (got) {
                    reload_overrides(&state);
                }
            }

            // Clicks are emitted here rather than by the key handler that
            // decided on them. move_pointer round-trips the display, and a
            // round trip from inside a dispatch is a dispatch inside a
            // dispatch.
            while (state.pending_clicks > 0) {
                state.pending_clicks--;
                move_pointer(
                    &state, state.result.x + state.result.w / 2,
                    state.result.y + state.result.h / 2, state.click
                );
                report_click(
                    &state, state.result.x + state.result.w / 2,
                    state.result.y + state.result.h / 2
                );
                state.clicked = true;
            }

            // The window closed with nothing pressed in it: one click was all
            // it was, and the run is over.
            if (state.double_click_sym != XKB_KEY_NoSymbol &&
                now_ms() >= state.double_click_deadline_ms) {
                state.running = false;
            }
        }
    loop_done:;
    }

    // The frame callback holds a pointer to the surface and to the buffer
    // pool, both destroyed just below. Left armed, it is still dispatched by
    // the roundtrip that follows and calls `send_frame()` on freed memory.
    if (state.wl_surface_callback != NULL) {
        wl_callback_destroy(state.wl_surface_callback);
        state.wl_surface_callback = NULL;
    }

    wp_viewport_destroy(state.wp_viewport);

    zwlr_layer_surface_v1_destroy(state.wl_layer_surface);
    wl_surface_destroy(state.wl_surface);
    wl_region_destroy(wl_region);

    surface_buffer_pool_destroy(&state.surface_buffer_pool);
    wl_display_roundtrip(state.wl_display);

    int status_code = 0;
    if (state.result.x != -1) {
        print_result(&state);
        // Not `if (!state.only_print)` alone any more: a double-click
        // window clicks while the overlay is still up, and the same result
        // must not be pressed a second time on the way out.
        if (!state.only_print && !state.clicked) {
            move_pointer(
                &state, state.result.x + state.result.w / 2,
                state.result.y + state.result.h / 2, state.click
            );
            // A run that only moves the pointer has not clicked anything.
            if (state.click != CLICK_NONE) {
                report_click(
                    &state, state.result.x + state.result.w / 2,
                    state.result.y + state.result.h / 2
                );
            }
        }
    } else {
        status_code = state.config.general.cancellation_status_code;
    }

    if (state.wl_virtual_keyboard_mgr != NULL) {
        zwp_virtual_keyboard_manager_v1_destroy(state.wl_virtual_keyboard_mgr);
    }
    if (state.wl_virtual_pointer_mgr != NULL) {
        zwlr_virtual_pointer_manager_v1_destroy(state.wl_virtual_pointer_mgr);
    }

    free_seats(&state.seats);
    free_outputs(&state.outputs);

    zxdg_output_manager_v1_destroy(state.xdg_output_manager);

    if (state.fractional_scale_mgr) {
        wp_fractional_scale_v1_destroy(fractional_scale);
        wp_fractional_scale_manager_v1_destroy(state.fractional_scale_mgr);
    }

    wp_viewporter_destroy(state.wp_viewporter);
    wl_shm_destroy(state.wl_shm);
    wl_compositor_destroy(state.wl_compositor);
    wl_registry_destroy(state.wl_registry);
    zwlr_layer_shell_v1_destroy(state.wl_layer_shell);

#if OPENCV_ENABLED
    if (state.wl_screencopy_manager) {
        zwlr_screencopy_manager_v1_destroy(state.wl_screencopy_manager);
    }
#endif

    wl_display_disconnect(state.wl_display);

    config_free_values(&state.config);
    free_mode_states(&state);

#if DEBUG
    cairo_debug_reset_static_data();
#endif

    return status_code;
}
