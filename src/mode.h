// SPDX-License-Identifier: GPL-3.0-only

#ifndef __MODE_H_INCLUDED__
#define __MODE_H_INCLUDED__

#include "state.h"

#include <cairo.h>
#include <stdbool.h>
#include <xkbcommon/xkbcommon.h>

struct mode_interface {
    char *name;

    // True when this mode's own key handler acts on space, which makes space
    // unavailable as the peek key while the mode is up. Declared here rather
    // than tested in main.c so that a mode which starts using space says so in
    // the one place it is already saying what it does with keys.
    bool takes_space;

    void *(*enter)(struct state *, struct rect area);
    void (*reenter)(struct state *, void *mode_state);
    bool (*key)(struct state *, void *mode_state, xkb_keysym_t, char *text);
    void (*render)(struct state *, void *mode_state, cairo_t *);
    void (*free)(void *mode_state);
};

extern struct mode_interface *mode_interfaces[];

/**
 * Load modes from given coma seperated list of mode names.
 * Returns 0 on success.
 */
int load_modes(struct state *, char *);

void enter_next_mode(struct state *, struct rect area);
bool has_last_mode_returned(struct state *);
bool reenter_prev_mode(struct state *);
void free_mode_states(struct state *);
bool mode_handle_key(struct state *, xkb_keysym_t, char *text);
bool mode_takes_space(struct state *);
void mode_render(struct state *, cairo_t *);

#endif
