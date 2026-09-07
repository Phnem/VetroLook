/*
 * gen_scene_linear.c - generate a synthetic scene-linear (ACEScg) test EXR for
 * the web/tocio demo: neutral exposure steps (incl. super-white highlights to
 * exercise the ACES tonescale), a saturated color-swatch grid, and per-channel
 * gradients. Float RGBA, ZIP-compressed.
 *
 * Build/run (from repo root):
 *   cc -Iinclude -Isrc -Ideps/zstd -std=c11 -O2 web/tocio/gen_scene_linear.c \
 *      $(ls src/exr_*.c | grep -v 'exr_stdio\|exr_freestanding\|exr_spectral') \
 *      deps/zstd/tinyexr_zstd.c -lm -o build/gen_scene_linear
 *   ./build/gen_scene_linear web/tocio/scene_linear.exr
 *
 * Copyright (c) 2014-2026 Syoyo Fujita and TinyEXR authors
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "exr.h"

#include <stdlib.h>
#include <string.h>

#define W 640
#define H 480

static void set_px(float **p, int x, int y, float r, float g, float b, float a) {
    size_t i = (size_t)y * W + (size_t)x;
    p[0][i] = r; p[1][i] = g; p[2][i] = b; p[3][i] = a;
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "scene_linear.exr";
    exr_image img;
    exr_part part;
    exr_channel chans[4];
    float *plane[4];
    static const char *names[4] = {"R", "G", "B", "A"};
    int k, x, y;
    /* a small set of saturated-ish ACEScg swatches (scene-linear, ~0.18 key) */
    static const float SW[12][3] = {
        {0.45f, 0.10f, 0.08f}, {0.50f, 0.30f, 0.16f}, {0.12f, 0.20f, 0.40f},
        {0.10f, 0.35f, 0.10f}, {0.40f, 0.40f, 0.08f}, {0.45f, 0.16f, 0.30f},
        {0.55f, 0.55f, 0.55f}, {0.18f, 0.18f, 0.18f}, {0.04f, 0.04f, 0.04f},
        {0.90f, 0.20f, 0.05f}, {0.05f, 0.45f, 0.55f}, {0.80f, 0.75f, 0.20f},
    };
    size_t np = (size_t)W * H;

    memset(&img, 0, sizeof(img));
    memset(&part, 0, sizeof(part));
    memset(chans, 0, sizeof(chans));
    for (k = 0; k < 4; ++k) {
        strcpy(chans[k].name, names[k]);
        chans[k].pixel_type = EXR_PIXEL_FLOAT; /* buffers below are float (must match) */
        chans[k].x_sampling = chans[k].y_sampling = 1;
        plane[k] = (float *)malloc(np * sizeof(float));
        if (!plane[k]) return 1;
    }

    for (y = 0; y < H; ++y) {
        for (x = 0; x < W; ++x) {
            float u = (float)x / (W - 1), v = (float)y / (H - 1);
            if (v < 0.18f) {
                /* neutral exposure steps: 12 patches, 0 .. ~8 (super-white) */
                int step = (int)(u * 12.0f);
                float e = (float)step / 11.0f;       /* 0..1 */
                float g = e * e * 8.0f;              /* 0 .. 8 scene-linear */
                set_px(plane, x, y, g, g, g, 1.0f);
            } else if (v < 0.72f) {
                /* 4x3 swatch grid */
                int col = (int)(u * 4.0f), row = (int)(((v - 0.18f) / 0.54f) * 3.0f);
                int idx = row * 4 + col;
                if (idx < 0) idx = 0;
                if (idx > 11) idx = 11;
                set_px(plane, x, y, SW[idx][0], SW[idx][1], SW[idx][2], 1.0f);
            } else {
                /* per-channel horizontal gradients, scaled to a small HDR range */
                float band = (v - 0.72f) / 0.28f; /* 0..1 across the 3 sub-bands */
                float val = u * 2.0f;             /* 0..2 */
                if (band < 0.3333f) set_px(plane, x, y, val, 0.0f, 0.0f, 1.0f);
                else if (band < 0.6666f) set_px(plane, x, y, 0.0f, val, 0.0f, 1.0f);
                else set_px(plane, x, y, 0.0f, 0.0f, val, 1.0f);
            }
        }
    }
    /* a bright specular highlight blob (top-right) to drive the tonescale roll-off */
    for (y = 0; y < H / 5; ++y)
        for (x = W - W / 5; x < W; ++x) {
            float cx = (float)(x - (W - W / 10)) / (W / 10.0f);
            float cy = (float)(y - (H / 10)) / (H / 10.0f);
            float d = cx * cx + cy * cy;
            float s = 16.0f * (1.0f - d);
            if (s > 0.0f) set_px(plane, x, y, s, s, s * 0.95f, 1.0f);
        }

    part.header.part_type = EXR_PART_SCANLINE;
    part.header.compression = EXR_COMPRESSION_ZIP;
    part.header.data_window.min_x = 0;
    part.header.data_window.min_y = 0;
    part.header.data_window.max_x = W - 1;
    part.header.data_window.max_y = H - 1;
    part.header.display_window = part.header.data_window;
    part.header.pixel_aspect_ratio = 1.0f;
    part.header.screen_window_width = 1.0f;
    part.header.num_channels = 4;
    part.header.channels = chans;
    part.width = W;
    part.height = H;
    part.images = (void **)plane;
    img.num_parts = 1;
    img.parts = &part;

    if (!EXR_OK(exr_save_to_file(path, &img, EXR_COMPRESSION_ZIP))) {
        for (k = 0; k < 4; ++k) free(plane[k]);
        return 2;
    }
    for (k = 0; k < 4; ++k) free(plane[k]);
    return 0;
}
