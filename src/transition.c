// SPDX-License-Identifier: GPL-3.0-only

#include "transition.h"

#include "log.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// A fixed, well-scattered number in [0, 1) for a grid cell, so a cell's turn
// comes at the same moment in every frame and the order looks random rather
// than swept.
static double cell_noise(uint32_t x, uint32_t y, uint32_t seed) {
    uint32_t h = x * 0x8da6b343u ^ y * 0xd8163841u ^ seed * 0xcb1ab31fu;
    h ^= h >> 15;
    h *= 0x2c1b3c6du;
    h ^= h >> 12;
    h *= 0x297a2d39u;
    h ^= h >> 15;
    return (h & 0xffffff) / (double)0x1000000;
}

// Bytes: the overlay is eaten into the screen a square at a time, in no order.
// `chunk` is the side of a square.
//
// The mask is one pixel per square, scaled up with no smoothing -- so however
// many squares there are, it is one small image and one paint, not a path of
// thousands of rectangles.
static void draw_bytes(
    cairo_t *cairo, cairo_pattern_t *overlay, double t, double w, double h,
    double chunk, uint32_t seed
) {
    int cols = (int)ceil(w / chunk);
    int rows = (int)ceil(h / chunk);

    cairo_surface_t *mask =
        cairo_image_surface_create(CAIRO_FORMAT_A8, cols, rows);
    if (cairo_surface_status(mask) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(mask);
        return;
    }
    cairo_surface_flush(mask);
    unsigned char *data   = cairo_image_surface_get_data(mask);
    int            stride = cairo_image_surface_get_stride(mask);
    for (int y = 0; y < rows; y++) {
        for (int x = 0; x < cols; x++) {
            data[y * stride + x] = cell_noise(x, y, seed) < t ? 0xff : 0;
        }
    }
    cairo_surface_mark_dirty(mask);

    cairo_pattern_t *pattern = cairo_pattern_create_for_surface(mask);
    cairo_matrix_t   matrix;
    cairo_matrix_init_scale(&matrix, 1 / chunk, 1 / chunk);
    cairo_pattern_set_matrix(pattern, &matrix);
    cairo_pattern_set_filter(pattern, CAIRO_FILTER_NEAREST);

    cairo_set_source(cairo, overlay);
    cairo_mask(cairo, pattern);

    cairo_pattern_destroy(pattern);
    cairo_surface_destroy(mask);
}

// Scanline: the overlay arrives in horizontal bands, each thrown sideways by
// its own amount and settling into place, the way a picture does when the
// sync catches. `chunk` is a quarter of a band's height, so the same setting
// gives thin lines here and big squares in bytes, and neither looks wrong.
//
// A band is shown from the moment its turn comes, already displaced, so the
// screen goes from torn to whole rather than from empty to whole.
static void draw_scanline(
    cairo_t *cairo, cairo_pattern_t *overlay, double t, double w, double h,
    double chunk, uint32_t seed
) {
    double band = fmax(2, chunk / 4);
    int    rows = (int)ceil(h / band);
    // How far off true a band still is: a lot at the start, none at the end.
    double left = (1 - t) * (1 - t);

    for (int y = 0; y < rows; y++) {
        // Half the bands are there from the start; the rest come in over the
        // first half, so it never reads as empty.
        if (cell_noise(y, 1, seed) * 0.5 > t) {
            continue;
        }
        double offset = (cell_noise(y, 0, seed) * 2 - 1) * w * 0.35 * left;

        cairo_save(cairo);
        cairo_rectangle(cairo, 0, y * band, w, band);
        cairo_clip(cairo);
        cairo_translate(cairo, offset, 0);
        cairo_set_source(cairo, overlay);
        cairo_paint(cairo);
        cairo_restore(cairo);
    }
}

static const struct transition transitions[] = {
    {"bytes", draw_bytes},
    {"scanline", draw_scanline},
};

#define NUM_TRANSITIONS (sizeof(transitions) / sizeof(transitions[0]))

static const struct transition *transition_find(const char *name) {
    for (size_t i = 0; i < NUM_TRANSITIONS; i++) {
        if (strcmp(transitions[i].name, name) == 0) {
            return &transitions[i];
        }
    }
    return NULL;
}

int transition_set_parse(struct transition_set *out, const char *value) {
    out->count = 0;
    if (strcmp(value, "none") == 0) {
        return 0;
    }
    if (strcmp(value, "random") == 0) {
        for (size_t i = 0; i < NUM_TRANSITIONS && i < MAX_TRANSITIONS; i++) {
            out->items[out->count++] = &transitions[i];
        }
        return 0;
    }

    char *copy = strdup(value);
    char *save = NULL;
    for (char *name = strtok_r(copy, ",", &save); name != NULL;
         name       = strtok_r(NULL, ",", &save)) {
        const struct transition *found = transition_find(name);
        if (found == NULL) {
            LOG_ERR(
                "Invalid transition '%s'. Should be 'none', 'random', or a "
                "comma-separated list of: bytes, scanline.",
                name
            );
            free(copy);
            out->count = 0;
            return 1;
        }
        if (out->count < MAX_TRANSITIONS) {
            out->items[out->count++] = found;
        }
    }
    free(copy);
    return 0;
}
