/*
 * compare_exr - load two EXR files with the v3 API and compare pixel data.
 *
 * Usage: compare_exr <a.exr> <b.exr>
 *
 * Used to validate lossless codecs (e.g. HTJ2K) against an uncompressed/ZIP
 * original: a reversible recompression must decode byte-identically. Matches
 * parts by index and channels by name; reports per-channel max abs diff and a
 * byte-identical verdict. Exit 0 iff every shared channel is byte-identical.
 *
 * Copyright (c) 2014-2026 Syoyo Fujita and TinyEXR authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "exr.h"

static size_t pixel_size(exr_pixel_type t) {
    return t == EXR_PIXEL_HALF ? 2u : 4u;
}

/* floor(a / b) for b > 0 (a may be negative), matching exr_core's num_samples. */
static long floordivc(long a, long b) {
    long q = a / b, r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) --q;
    return q;
}

/* Interpret one sample as a double for diff reporting. */
static double sample_to_double(const void *base, size_t i, exr_pixel_type t) {
    if (t == EXR_PIXEL_HALF) {
        uint16_t h = ((const uint16_t *)base)[i];
        float f;
        exr_half_to_float(&h, &f, 1);
        return (double)f;
    } else if (t == EXR_PIXEL_FLOAT) {
        return (double)((const float *)base)[i];
    } else {
        return (double)((const uint32_t *)base)[i];
    }
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <a.exr> <b.exr>\n", argv[0]);
        return 2;
    }
    exr_image a, b;
    exr_result ra = exr_load_from_file(argv[1], NULL, &a);
    exr_result rb = exr_load_from_file(argv[2], NULL, &b);
    if (!EXR_OK(ra)) { fprintf(stderr, "load %s: %s\n", argv[1], exr_result_string(ra)); return 2; }
    if (!EXR_OK(rb)) { fprintf(stderr, "load %s: %s\n", argv[2], exr_result_string(rb)); exr_image_free(&a); return 2; }

    int mismatch = 0;
    if (a.num_parts != b.num_parts) {
        printf("PART COUNT DIFFERS: %d vs %d\n", a.num_parts, b.num_parts);
        mismatch = 1;
    }
    int nparts = a.num_parts < b.num_parts ? a.num_parts : b.num_parts;
    for (int p = 0; p < nparts; ++p) {
        exr_part *pa = &a.parts[p], *pb = &b.parts[p];
        if (pa->width != pb->width || pa->height != pb->height) {
            printf("part %d: size differs %dx%d vs %dx%d\n", p,
                   pa->width, pa->height, pb->width, pb->height);
            mismatch = 1;
            continue;
        }
        for (int ca = 0; ca < pa->header.num_channels; ++ca) {
            const exr_channel *cha = &pa->header.channels[ca];
            /* per-channel sample count honours x/y subsampling (e.g. RY/BY in
             * luminance-chroma images are sampling 2 2 -> a quarter-size plane).
             * num_samples(lo,hi,s) = floor(hi/s) - floor((lo-1)/s). */
            int xs = cha->x_sampling <= 0 ? 1 : cha->x_sampling;
            int ys = cha->y_sampling <= 0 ? 1 : cha->y_sampling;
            int lo_x = pa->header.data_window.min_x, hi_x = pa->header.data_window.max_x;
            int lo_y = pa->header.data_window.min_y, hi_y = pa->header.data_window.max_y;
            long cw = (long)floordivc(hi_x, xs) - floordivc((long)lo_x - 1, xs);
            long chh = (long)floordivc(hi_y, ys) - floordivc((long)lo_y - 1, ys);
            size_t npix = (size_t)(cw > 0 ? cw : 0) * (size_t)(chh > 0 ? chh : 0);
            /* find same-named channel in b */
            int cb = -1;
            for (int j = 0; j < pb->header.num_channels; ++j)
                if (strcmp(cha->name, pb->header.channels[j].name) == 0) { cb = j; break; }
            if (cb < 0) { printf("part %d: channel '%s' missing in b\n", p, cha->name); mismatch = 1; continue; }
            const exr_channel *chb = &pb->header.channels[cb];
            const char *tn = cha->pixel_type == EXR_PIXEL_HALF ? "half" :
                             cha->pixel_type == EXR_PIXEL_FLOAT ? "float" : "uint";
            if (cha->pixel_type != chb->pixel_type) {
                printf("part %d ch '%s': type differs\n", p, cha->name);
                mismatch = 1; continue;
            }
            size_t ps = pixel_size(cha->pixel_type);
            const void *da = pa->images ? pa->images[ca] : NULL;
            const void *db = pb->images ? pb->images[cb] : NULL;
            if (!da || !db) { printf("part %d ch '%s': missing pixel data\n", p, cha->name); mismatch = 1; continue; }
            int identical = (memcmp(da, db, npix * ps) == 0);
            double maxabs = 0.0; size_t nbad = 0;
            if (!identical) {
                for (size_t i = 0; i < npix; ++i) {
                    double va = sample_to_double(da, i, cha->pixel_type);
                    double vb = sample_to_double(db, i, cha->pixel_type);
                    double d = fabs(va - vb);
                    if (d > maxabs) maxabs = d;
                    if (d != 0.0) ++nbad;
                }
                mismatch = 1;
            }
            printf("  part %d ch %-18s %-5s : %s", p, cha->name, tn,
                   identical ? "IDENTICAL\n" : "");
            if (!identical)
                printf("DIFF (%zu/%zu samples, max|d|=%g)\n", nbad, npix, maxabs);
        }
    }

    exr_image_free(&a);
    exr_image_free(&b);
    printf(mismatch ? "RESULT: MISMATCH\n" : "RESULT: byte-identical\n");
    return mismatch ? 1 : 0;
}
