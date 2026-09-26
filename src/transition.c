// SPDX-License-Identifier: GPL-3.0-only

#include "transition.h"

#include "log.h"

#include <math.h>
#include <stdbool.h>
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

// A whole number of cells, so nothing here ever lands half a cell out. Every
// transition that moves the overlay moves it by one of these: a piece of a
// picture going wrong jumps a pixel at a time, it does not slide.
static double snap(double v, double chunk) {
    return floor(v / chunk) * chunk;
}

// How much of the overlay a cell shows, in [0, 1]. Called once per cell per
// frame, and nothing else about the cell is its business: `ctx` carries
// whatever the transition needs.
typedef double (*cell_level_fn)(int x, int y, void *ctx);

// The workhorse behind every transition that reveals the overlay in place
// rather than moving it: build one mask, one pixel per cell, and composite the
// whole overlay through it. However many cells there are, that is one small
// image and one paint -- not a path of thousands of rectangles -- and scaling
// it up with NEAREST is what keeps the edges hard.
//
// Cells are `cw` by `ch` logical pixels, which are the same for a grid of
// squares and different for bands or columns.
static void paint_cells(
    cairo_t *cairo, cairo_pattern_t *overlay, double w, double h, double cw,
    double ch, cell_level_fn level, void *ctx
) {
    int cols = (int)ceil(w / cw);
    int rows = (int)ceil(h / ch);

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
            double a = level(x, y, ctx);
            a        = a < 0 ? 0 : a > 1 ? 1 : a;
            data[y * stride + x] = (unsigned char)(a * 0xff);
        }
    }
    cairo_surface_mark_dirty(mask);

    cairo_pattern_t *pattern = cairo_pattern_create_for_surface(mask);
    cairo_matrix_t   matrix;
    cairo_matrix_init_scale(&matrix, 1 / cw, 1 / ch);
    cairo_pattern_set_matrix(pattern, &matrix);
    cairo_pattern_set_filter(pattern, CAIRO_FILTER_NEAREST);

    cairo_set_source(cairo, overlay);
    cairo_mask(cairo, pattern);

    cairo_pattern_destroy(pattern);
    cairo_surface_destroy(mask);
}

// What every cell-mask transition is told about the frame it is part of.
struct reveal {
    double   t;
    uint32_t seed;
    int      rows;
};

// Bytes: the overlay is eaten into the screen a square at a time, in no order.
// `chunk` is the side of a square.
static double level_bytes(int x, int y, void *ctx) {
    const struct reveal *r = ctx;
    return cell_noise(x, y, r->seed) < r->t ? 1 : 0;
}

static void draw_bytes(
    cairo_t *cairo, cairo_pattern_t *overlay, double t, double w, double h,
    double chunk, uint32_t seed
) {
    struct reveal r = {.t = t, .seed = seed};
    paint_cells(cairo, overlay, w, h, chunk, chunk, level_bytes, &r);
}

// Interlace: one field then the other, each sweeping down the screen the way
// the beam does. Every other line is there before its neighbour is, which is
// the thing a CRT did that nothing since does.
static double level_interlace(int x, int y, void *ctx) {
    (void)x;
    const struct reveal *r = ctx;
    // The odd field does not start until the even one is done.
    double field = (y & 1) ? 0.5 : 0;
    double down  = r->rows > 1 ? (double)y / (r->rows - 1) : 0;
    return field + down * 0.5 < r->t ? 1 : 0;
}

static void draw_interlace(
    cairo_t *cairo, cairo_pattern_t *overlay, double t, double w, double h,
    double chunk, uint32_t seed
) {
    // A line, not a square: the same setting that gives big squares in bytes
    // gives scanlines here.
    double band    = fmax(2, chunk / 8);
    struct reveal r = {
        .t = t, .seed = seed, .rows = (int)ceil(h / band)
    };
    paint_cells(cairo, overlay, w, h, w, band, level_interlace, &r);
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
        cairo_translate(cairo, snap(offset, chunk), 0);
        cairo_set_source(cairo, overlay);
        cairo_paint(cairo);
        cairo_restore(cairo);
    }
}

// Dropout: scanline stood on its end. Columns of the picture hang below or
// above where they belong and are pulled back into line -- tape dropout rather
// than sync loss, and different enough from scanline to be worth having when
// both are in the set.
static void draw_dropout(
    cairo_t *cairo, cairo_pattern_t *overlay, double t, double w, double h,
    double chunk, uint32_t seed
) {
    int    cols = (int)ceil(w / chunk);
    double left = (1 - t) * (1 - t);

    for (int x = 0; x < cols; x++) {
        if (cell_noise(x, 1, seed) * 0.5 > t) {
            continue;
        }
        double offset = (cell_noise(x, 0, seed) * 2 - 1) * h * 0.35 * left;

        cairo_save(cairo);
        cairo_rectangle(cairo, x * chunk, 0, chunk, h);
        cairo_clip(cairo);
        cairo_translate(cairo, 0, snap(offset, chunk));
        cairo_set_source(cairo, overlay);
        cairo_paint(cairo);
        cairo_restore(cairo);
    }
}

// Roll: the whole picture running up the screen and slowing to a stop, with the
// bar across the seam, which is what a monitor did when the vertical hold was
// out. Two paints of the same overlay -- one for the part that has run off the
// top and come back round the bottom -- and the gap between them is the bar.
static void draw_roll(
    cairo_t *cairo, cairo_pattern_t *overlay, double t, double w, double h,
    double chunk, uint32_t seed
) {
    double left = (1 - t) * (1 - t);
    // Two and a bit turns, slowing to none. Up or down, per overlay.
    double travel = left * h * 2.4;
    double off    = snap(fmod(travel, h), fmax(2, chunk / 4));
    if (seed & 1) {
        off = -off;
    }
    // The bar closes as the picture settles, and is gone at the end.
    double bar = snap(fmax(0, chunk * 1.5 * left), fmax(2, chunk / 4));

    for (int copy = -1; copy <= 1; copy++) {
        cairo_save(cairo);
        cairo_translate(cairo, 0, off + copy * h);
        cairo_set_source(cairo, overlay);
        cairo_paint(cairo);
        cairo_restore(cairo);
    }

    if (bar > 0) {
        // Punched out rather than drawn over: the transition is composited
        // into a group of its own, so clearing here leaves the desktop showing
        // through, which is what the blanking bar did.
        cairo_save(cairo);
        cairo_set_operator(cairo, CAIRO_OPERATOR_CLEAR);
        for (int copy = -1; copy <= 1; copy++) {
            cairo_rectangle(cairo, 0, off + copy * h - bar, w, bar);
        }
        cairo_fill(cairo);
        cairo_restore(cairo);
    }
}

// Beam: a raster scan. Everything above the beam is drawn, the line itself is
// twice as bright, and nothing below it is there yet -- the overlay is painted
// on, top to bottom, in one pass.
static void draw_beam(
    cairo_t *cairo, cairo_pattern_t *overlay, double t, double w, double h,
    double chunk, uint32_t seed
) {
    double band = fmax(2, chunk / 4);
    // A little past the bottom, so the last band is painted and the beam is
    // off the screen before the intro ends rather than parked on the edge.
    double edge = snap(t * (h + chunk * 2), band);
    // Where the beam starts is not quite the top, so two overlays in a row do
    // not scan from the same line.
    double slip = snap(cell_noise(seed, 3, seed) * chunk, band);

    cairo_save(cairo);
    cairo_rectangle(cairo, 0, 0, w, fmax(0, edge - slip));
    cairo_clip(cairo);
    cairo_set_source(cairo, overlay);
    cairo_paint(cairo);
    cairo_restore(cairo);

    if (edge < h + chunk * 2) {
        // The overlay's own colours, added to themselves: the line is the
        // overlay at twice the brightness, so it belongs to the theme without
        // this knowing anything about it.
        cairo_save(cairo);
        cairo_rectangle(cairo, 0, edge - band * 2, w, band * 2);
        cairo_clip(cairo);
        cairo_set_operator(cairo, CAIRO_OPERATOR_ADD);
        cairo_set_source(cairo, overlay);
        cairo_paint_with_alpha(cairo, 0.8);
        cairo_restore(cairo);
    }
}

// Shuffle: the picture assembled out of the wrong pieces. Most of it is in
// place; a scattering of cells show what belongs somewhere else, and the
// scattering shrinks and comes right. The loudest of them, and the only one
// that costs a paint per cell -- so the number of wrong cells is capped rather
// than being a share of however many the screen has.
struct shuffle {
    double   t;
    double   frac;
    uint32_t seed;
};

static bool shuffle_wrong(int x, int y, const struct shuffle *s) {
    return cell_noise(x, y, s->seed ^ 0x5f3759dfu) < s->frac;
}

static double level_shuffle(int x, int y, void *ctx) {
    // A wrong cell is left out of the one-paint pass; it is drawn on its own
    // below, showing somewhere else's content.
    return shuffle_wrong(x, y, ctx) ? 0 : 1;
}

static void draw_shuffle(
    cairo_t *cairo, cairo_pattern_t *overlay, double t, double w, double h,
    double chunk, uint32_t seed
) {
    int cols = (int)ceil(w / chunk);
    int rows = (int)ceil(h / chunk);
    int cells = cols * rows;

    // At most this many wrong cells, however big the screen is: past a few
    // hundred it is a frame's worth of work and no louder to look at.
    const int      cap = 400;
    double         share = cells > 0 ? fmin(0.3, (double)cap / cells) : 0;
    struct shuffle s    = {
           .t = t, .frac = share * (1 - t), .seed = seed
    };

    paint_cells(cairo, overlay, w, h, chunk, chunk, level_shuffle, &s);

    for (int y = 0; y < rows; y++) {
        for (int x = 0; x < cols; x++) {
            if (!shuffle_wrong(x, y, &s)) {
                continue;
            }
            // Where it is showing from: whole cells away, fewer as it settles.
            double reach = 1 + 5 * (1 - t);
            double dx = round((cell_noise(x, y, seed + 7) * 2 - 1) * reach);
            double dy = round((cell_noise(x, y, seed + 11) * 2 - 1) * reach);

            cairo_save(cairo);
            cairo_rectangle(cairo, x * chunk, y * chunk, chunk, chunk);
            cairo_clip(cairo);
            cairo_translate(cairo, dx * chunk, dy * chunk);
            cairo_set_source(cairo, overlay);
            cairo_paint(cairo);
            cairo_restore(cairo);
        }
    }
}

static const struct transition transitions[] = {
    {"bytes",     draw_bytes    },
    {"interlace", draw_interlace},
    {"scanline",  draw_scanline },
    {"dropout",   draw_dropout  },
    {"roll",      draw_roll     },
    {"beam",      draw_beam     },
    {"shuffle",   draw_shuffle  },
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
                "comma-separated list of: bytes, interlace, scanline, "
                "dropout, roll, beam, shuffle.",
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
