/*
 * Small, fast SIMD smoke test - intended for cross-arch runs under an emulator
 * (e.g. qemu-aarch64) to confirm the NEON path builds and is correct, but it is
 * arch-neutral and runs natively too (exercises whatever SIMD tier is active).
 *
 * It checks two things on a deliberately small workload:
 *   1. the dispatched SIMD predictor (NEON on aarch64, SSE2 on x86) is
 *      bit-identical to the scalar reference, at several sizes incl. edges;
 *   2. small-image memory round-trips for the lossless flat codecs match exactly
 *      (this drives predictor + interleave + inflate end to end).
 *
 * Copyright (c) 2014-2026 Syoyo Fujita and TinyEXR authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "exr.h"
#include "exr_internal.h" /* scalar predictor refs + exr_simd_info (test-only) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned g_rng = 2463534242u;
static uint8_t rb(void) {
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
    return (uint8_t)(g_rng >> 7);
}

/* 1. SIMD predictor (dispatched) must equal the scalar reference. */
static int test_predictor(void) {
    const size_t sizes[] = {0, 1, 7, 15, 16, 17, 31, 64, 1000, 100003};
    size_t k;
    int ok = 1;
    for (k = 0; k < sizeof(sizes) / sizeof(sizes[0]); ++k) {
        size_t n = sizes[k], i;
        uint8_t *src = (uint8_t *)malloc(n ? n : 1);
        uint8_t *a = (uint8_t *)malloc(n ? n : 1);
        uint8_t *b = (uint8_t *)malloc(n ? n : 1);
        if (!src || !a || !b) { free(src); free(a); free(b); return 0; }
        for (i = 0; i < n; ++i) src[i] = rb();

        memcpy(a, src, n); exr_predictor_encode_scalar(a, n);
        memcpy(b, src, n); exr_predictor_encode(b, n); /* dispatched SIMD */
        if (n && memcmp(a, b, n)) { printf("  FAIL: encode n=%zu\n", n); ok = 0; }

        /* decode of the (scalar-)encoded data, scalar vs SIMD */
        memcpy(b, a, n); exr_predictor_decode_scalar(a, n);
        exr_predictor_decode(b, n); /* dispatched SIMD, in place on copy */
        if (n && memcmp(a, b, n)) { printf("  FAIL: decode n=%zu\n", n); ok = 0; }

        /* full round-trip through the SIMD path == identity */
        memcpy(b, src, n); exr_predictor_encode(b, n); exr_predictor_decode(b, n);
        if (n && memcmp(b, src, n)) { printf("  FAIL: roundtrip n=%zu\n", n); ok = 0; }

        free(src); free(a); free(b);
    }
    printf("  %s: SIMD predictor == scalar (10 sizes, incl. edges)\n",
           ok ? "ok" : "FAIL");
    return ok;
}

/* 2. small-image lossless round-trips. */
static int test_roundtrip(void) {
    enum { W = 96, H = 64, NC = 3 };
    static uint16_t r[W * H], g[W * H], bl[W * H];
    static void *planes[NC];
    exr_channel chans[NC];
    exr_part part;
    exr_image img;
    const exr_compression codecs[] = {EXR_COMPRESSION_NONE, EXR_COMPRESSION_RLE,
                                      EXR_COMPRESSION_ZIPS, EXR_COMPRESSION_ZIP,
                                      EXR_COMPRESSION_PIZ};
    const char *names[] = {"none", "rle", "zips", "zip", "piz"};
    size_t i, ci;
    int ok = 1, c;

    for (i = 0; i < (size_t)(W * H); ++i) {
        r[i] = (uint16_t)(rb() | (rb() << 8));
        g[i] = (uint16_t)(rb() | (rb() << 8));
        bl[i] = (uint16_t)(rb() | (rb() << 8));
    }
    memset(chans, 0, sizeof(chans));
    strcpy(chans[0].name, "B"); strcpy(chans[1].name, "G"); strcpy(chans[2].name, "R");
    for (c = 0; c < NC; ++c) {
        chans[c].pixel_type = EXR_PIXEL_HALF;
        chans[c].x_sampling = 1;
        chans[c].y_sampling = 1;
    }
    memset(&part, 0, sizeof(part));
    part.header.part_type = EXR_PART_SCANLINE;
    part.header.data_window.max_x = W - 1;
    part.header.data_window.max_y = H - 1;
    part.header.display_window = part.header.data_window;
    part.header.pixel_aspect_ratio = 1.0f;
    part.header.screen_window_width = 1.0f;
    part.header.num_channels = NC;
    part.header.channels = chans;
    part.width = W;
    part.height = H;
    planes[0] = bl; planes[1] = g; planes[2] = r;
    part.images = planes;
    memset(&img, 0, sizeof(img));
    img.num_parts = 1;
    img.parts = &part;

    for (ci = 0; ci < sizeof(codecs) / sizeof(codecs[0]); ++ci) {
        void *buf = NULL;
        size_t sz = 0;
        exr_image back;
        int good;
        part.header.compression = codecs[ci];
        if (!EXR_OK(exr_save_to_memory(&buf, &sz, NULL, &img, codecs[ci]))) {
            printf("  FAIL: save %s\n", names[ci]); ok = 0; continue;
        }
        memset(&back, 0, sizeof(back));
        if (!EXR_OK(exr_load_from_memory(buf, sz, NULL, &back))) {
            printf("  FAIL: load %s\n", names[ci]); ok = 0; free(buf); continue;
        }
        free(buf);
        good = back.num_parts == 1 && back.parts[0].width == W &&
               back.parts[0].height == H &&
               back.parts[0].header.num_channels == NC;
        if (good) {
            const exr_part *p = &back.parts[0];
            for (c = 0; c < NC && good; ++c) {
                const uint16_t *want = strcmp(p->header.channels[c].name, "R") == 0
                    ? r : strcmp(p->header.channels[c].name, "G") == 0 ? g : bl;
                if (memcmp(p->images[c], want, W * H * 2) != 0) good = 0;
            }
        }
        if (!good) { printf("  FAIL: roundtrip %s\n", names[ci]); ok = 0; }
        exr_image_free(&back);
    }
    printf("  %s: %dx%d lossless round-trips (none/rle/zips/zip/piz)\n",
           ok ? "ok" : "FAIL", W, H);
    return ok;
}

int main(void) {
    int ok = 1;
    exr_simd_init();
    printf("SIMD smoke  |  active tier: %s  (caps=0x%x)\n", exr_simd_info(),
           exr_simd_capabilities());
    ok &= test_predictor();
    ok &= test_roundtrip();
    printf("%s\n", ok ? "ALL OK" : "FAILED");
    return ok ? 0 : 1;
}
