/*
 * TinyEXR v3 - minimal WebAssembly binding (pure C, no embind).
 *
 * Exposes memory-based decode/encode of the first part as RGBA float, built on
 * the v3 C API (include/exr.h). No filesystem, no stdio: buffers cross the JS
 * boundary via emscripten's _malloc/_free.
 *
 * Exported:
 *   float*   exrw_decode_rgba(const uint8_t* data, int size, int* w, int* h);
 *   uint8_t* exrw_encode_rgba(const float* rgba, int w, int h, int comp,
 *                             int* out_size);
 *   void     exrw_free(void* p);
 *
 * decode returns a malloc'd w*h*4 float RGBA buffer (NULL on error); encode
 * returns a malloc'd EXR byte buffer (NULL on error). Free either with
 * exrw_free (or JS Module._free).
 *
 * Copyright (c) 2014-2026 Syoyo Fujita and TinyEXR authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "exr.h"

#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#define EXRW_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define EXRW_EXPORT
#endif

/* Find a channel by exact name in a part; returns its index or -1. */
static int find_channel(const exr_part *p, const char *name) {
    int c;
    for (c = 0; c < p->header.num_channels; ++c)
        if (strcmp(p->header.channels[c].name, name) == 0) return c;
    return -1;
}

/* Read one channel's pixel at linear index i as float (HALF/FLOAT/UINT). */
static float chan_to_float(const exr_part *p, int c, size_t i) {
    const void *base = p->images[c];
    switch (p->header.channels[c].pixel_type) {
    case EXR_PIXEL_HALF: {
        float f;
        exr_half_to_float(((const uint16_t *)base) + i, &f, 1);
        return f;
    }
    case EXR_PIXEL_FLOAT:
        return ((const float *)base)[i];
    case EXR_PIXEL_UINT:
        return (float)((const uint32_t *)base)[i];
    }
    return 0.0f;
}

EXRW_EXPORT float *exrw_decode_rgba(const uint8_t *data, int size, int *w,
                                    int *h) {
    exr_image img;
    const exr_part *p;
    float *rgba;
    size_t n, i;
    int ci[4], k;
    static const char *names[4] = {"R", "G", "B", "A"};

    if (!data || size <= 0) return NULL;
    memset(&img, 0, sizeof(img));
    if (!EXR_OK(exr_load_from_memory(data, (size_t)size, NULL, &img)))
        return NULL;
    if (img.num_parts < 1) { exr_image_free(&img); return NULL; }
    p = &img.parts[0];
    if (p->is_deep || !p->images) { exr_image_free(&img); return NULL; }

    /* Luminance-chroma (Y + subsampled RY/BY): reconstruct true color instead
     * of leaving it as grayscale Y. The buffer is malloc'd (default allocator),
     * so the JS side frees it with exrw_free just like the generic path. */
    if (exr_part_is_luminance_chroma(p)) {
        float *yc = NULL;
        int yw = 0, yh = 0;
        if (EXR_OK(exr_part_yc_to_rgba_float(NULL, p, &yc, &yw, &yh))) {
            if (w) *w = yw;
            if (h) *h = yh;
            exr_image_free(&img);
            return yc;
        }
        /* fall through to the generic R/G/B/A path on failure */
    }

    for (k = 0; k < 4; ++k) ci[k] = find_channel(p, names[k]);
    n = (size_t)p->width * (size_t)p->height;
    rgba = (float *)malloc((n ? n : 1) * 4 * sizeof(float));
    if (!rgba) { exr_image_free(&img); return NULL; }

    for (i = 0; i < n; ++i) {
        for (k = 0; k < 4; ++k) {
            float v = (k == 3) ? 1.0f : 0.0f; /* default RGB=0, A=1 */
            if (ci[k] >= 0) v = chan_to_float(p, ci[k], i);
            rgba[i * 4 + k] = v;
        }
    }
    if (w) *w = p->width;
    if (h) *h = p->height;
    exr_image_free(&img);
    return rgba;
}

EXRW_EXPORT uint8_t *exrw_encode_rgba(const float *rgba, int w, int h, int comp,
                                      int *out_size) {
    exr_image img;
    exr_part part;
    exr_channel chans[4];
    float *plane[4];
    void *out = NULL;
    size_t np, i, sz = 0;
    int k;
    static const char *names[4] = {"R", "G", "B", "A"};

    if (!rgba || w <= 0 || h <= 0) return NULL;
    np = (size_t)w * (size_t)h;

    memset(&img, 0, sizeof(img));
    memset(&part, 0, sizeof(part));
    memset(chans, 0, sizeof(chans));
    for (k = 0; k < 4; ++k) {
        strcpy(chans[k].name, names[k]);
        chans[k].pixel_type = EXR_PIXEL_FLOAT;
        chans[k].x_sampling = 1;
        chans[k].y_sampling = 1;
        plane[k] = (float *)malloc((np ? np : 1) * sizeof(float));
        if (!plane[k]) { while (k-- > 0) free(plane[k]); return NULL; }
    }
    for (i = 0; i < np; ++i)
        for (k = 0; k < 4; ++k) plane[k][i] = rgba[i * 4 + k];

    part.header.part_type = EXR_PART_SCANLINE;
    part.header.compression = (exr_compression)comp;
    part.header.data_window.min_x = 0;
    part.header.data_window.min_y = 0;
    part.header.data_window.max_x = w - 1;
    part.header.data_window.max_y = h - 1;
    part.header.display_window = part.header.data_window;
    part.header.pixel_aspect_ratio = 1.0f;
    part.header.screen_window_width = 1.0f;
    part.header.num_channels = 4;
    part.header.channels = chans;
    part.width = w;
    part.height = h;
    part.images = (void **)plane;

    img.num_parts = 1;
    img.parts = &part;

    if (!EXR_OK(exr_save_to_memory(&out, &sz, NULL, &img, (exr_compression)comp)))
        out = NULL;

    for (k = 0; k < 4; ++k) free(plane[k]);
    if (out && out_size) *out_size = (int)sz;
    return (uint8_t *)out;
}

EXRW_EXPORT void exrw_free(void *p) { free(p); }
