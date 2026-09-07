/*
 * Functional smoke test for the freestanding-compiled core.
 *
 * This harness is built HOSTED (it may use stdio/malloc for reporting and to
 * back a custom allocator), but it links against the core objects compiled with
 * -DEXR_FREESTANDING -DEXR_NO_ZSTD. It proves the freestanding build is callable
 * and correct: a memory round-trip with a caller-supplied allocator (the
 * freestanding default allocator is intentionally NULL).
 *
 * Copyright (c) 2014-2026 Syoyo Fujita and TinyEXR authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "exr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef EXR_FREESTANDING_ZSTD
#include "fs_zstd_blob.inc" /* fs_zstd_exr[] : 4x4 BGR FLOAT, ZSTD, R[i]==i */
#endif

/* exported by the core (declared in the internal header); the freestanding
 * build returns NULL here. */
const exr_allocator *exr_default_allocator(void);

static void *a_alloc(void *u, size_t n) { (void)u; return malloc(n ? n : 1); }
static void a_free(void *u, void *p) { (void)u; free(p); }

int main(void) {
    exr_allocator al = {NULL, a_alloc, a_free};
    enum { W = 4, H = 4 };
    float r[W * H], g[W * H], b[W * H];
    exr_channel chans[3];
    exr_part part;
    exr_image img, back;
    void *buf = NULL;
    size_t sz = 0, i;
    int ok = 1, c;

    for (i = 0; i < (size_t)(W * H); ++i) {
        r[i] = (float)i;
        g[i] = (float)i * 2.0f;
        b[i] = (float)i * 3.0f;
    }
    memset(chans, 0, sizeof(chans));
    strcpy(chans[0].name, "B"); strcpy(chans[1].name, "G"); strcpy(chans[2].name, "R");
    for (c = 0; c < 3; ++c) {
        chans[c].pixel_type = EXR_PIXEL_FLOAT;
        chans[c].x_sampling = 1;
        chans[c].y_sampling = 1;
    }
    memset(&part, 0, sizeof(part));
    part.header.part_type = EXR_PART_SCANLINE;
    part.header.compression = EXR_COMPRESSION_ZIP;
    part.header.data_window.max_x = W - 1;
    part.header.data_window.max_y = H - 1;
    part.header.display_window = part.header.data_window;
    part.header.pixel_aspect_ratio = 1.0f;
    part.header.screen_window_width = 1.0f;
    part.header.num_channels = 3;
    part.header.channels = chans;
    part.width = W;
    part.height = H;
    {
        static void *planes[3];
        planes[0] = b; planes[1] = g; planes[2] = r; /* B,G,R order */
        part.images = planes;
    }
    memset(&img, 0, sizeof(img));
    img.num_parts = 1;
    img.parts = &part;

    /* default allocator must be NULL in freestanding builds */
    if (exr_default_allocator() != NULL) {
        printf("FAIL: freestanding default allocator is non-NULL\n");
        return 1;
    }

    if (!EXR_OK(exr_save_to_memory(&buf, &sz, &al, &img, EXR_COMPRESSION_ZIP))) {
        printf("FAIL: save\n");
        return 1;
    }
    memset(&back, 0, sizeof(back));
    if (!EXR_OK(exr_load_from_memory(buf, sz, &al, &back))) {
        printf("FAIL: load\n");
        al.free(NULL, buf);
        return 1;
    }
    al.free(NULL, buf);

    ok = back.num_parts == 1 && back.parts[0].width == W &&
         back.parts[0].height == H && back.parts[0].header.num_channels == 3;
    /* spot-check a channel round-trips (find R) */
    if (ok) {
        const exr_part *p = &back.parts[0];
        int rc = -1;
        for (c = 0; c < p->header.num_channels; ++c)
            if (strcmp(p->header.channels[c].name, "R") == 0) rc = c;
        if (rc < 0) ok = 0;
        else {
            const float *rr = (const float *)p->images[rc];
            for (i = 0; i < (size_t)(W * H); ++i)
                if (rr[i] != (float)i) { ok = 0; break; }
        }
    }
    exr_image_free(&back);

    printf("%s: freestanding memory round-trip (%dx%d, zip, %zu bytes)\n",
           ok ? "ok" : "FAIL", W, H, sz);

#ifdef EXR_FREESTANDING_ZSTD
    /* Decode-only zstd: load a canned ZSTD-compressed EXR via the no-malloc
     * static-DCtx path (the freestanding build cannot encode zstd). */
    {
        exr_image z;
        int zok;
        memset(&z, 0, sizeof(z));
        zok = EXR_OK(exr_load_from_memory(fs_zstd_exr, fs_zstd_exr_len, &al, &z));
        if (zok) {
            const exr_part *p = &z.parts[0];
            int rc = -1;
            zok = z.num_parts == 1 && p->width == W && p->height == H;
            for (c = 0; c < p->header.num_channels; ++c)
                if (strcmp(p->header.channels[c].name, "R") == 0) rc = c;
            if (rc < 0) zok = 0;
            else {
                const float *rr = (const float *)p->images[rc];
                for (i = 0; i < (size_t)(W * H); ++i)
                    if (rr[i] != (float)i) { zok = 0; break; }
            }
            exr_image_free(&z);
        }
        printf("%s: freestanding zstd decode (%dx%d, %u bytes)\n",
               zok ? "ok" : "FAIL", W, H, fs_zstd_exr_len);
        ok = ok && zok;
    }
#endif

    return ok ? 0 : 1;
}
