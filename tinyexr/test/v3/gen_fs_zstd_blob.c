/*
 * Regenerate test/v3/fs_zstd_blob.inc: the ZSTD-compressed EXR fixture decoded
 * by the freestanding smoke test. Builds the same 4x4 BGR FLOAT image as
 * freestanding_smoke.c (B=3i, G=2i, R=i), saves it as EXR_COMPRESSION_ZSTD, and
 * dumps the bytes as a C array. Run via `make gen-fs-zstd-blob`.
 *
 * Copyright (c) 2014-2026 Syoyo Fujita and TinyEXR authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdio.h>
#include <string.h>

#include "exr.h"

int main(void) {
    enum { W = 4, H = 4 };
    float r[W * H], g[W * H], b[W * H];
    exr_channel ch[3];
    exr_part part;
    exr_image img;
    void *planes[3];
    void *buf = NULL;
    size_t sz = 0, i;

    for (i = 0; i < (size_t)(W * H); ++i) {
        r[i] = (float)i;
        g[i] = (float)i * 2.0f;
        b[i] = (float)i * 3.0f;
    }
    memset(ch, 0, sizeof(ch));
    strcpy(ch[0].name, "B");
    strcpy(ch[1].name, "G");
    strcpy(ch[2].name, "R");
    for (i = 0; i < 3; ++i) {
        ch[i].pixel_type = EXR_PIXEL_FLOAT;
        ch[i].x_sampling = 1;
        ch[i].y_sampling = 1;
    }
    memset(&part, 0, sizeof(part));
    part.header.part_type = EXR_PART_SCANLINE;
    part.header.compression = EXR_COMPRESSION_ZSTD;
    part.header.data_window.max_x = W - 1;
    part.header.data_window.max_y = H - 1;
    part.header.display_window = part.header.data_window;
    part.header.pixel_aspect_ratio = 1.0f;
    part.header.screen_window_width = 1.0f;
    part.header.num_channels = 3;
    part.header.channels = ch;
    part.width = W;
    part.height = H;
    planes[0] = b;
    planes[1] = g;
    planes[2] = r;
    part.images = planes;
    memset(&img, 0, sizeof(img));
    img.num_parts = 1;
    img.parts = &part;

    if (!EXR_OK(exr_save_to_memory(&buf, &sz, NULL, &img, EXR_COMPRESSION_ZSTD))) {
        fprintf(stderr, "gen-fs-zstd-blob: ZSTD encode failed\n");
        return 1;
    }
    printf("/* Fixture for freestanding_smoke.c: the same 4x4 BGR FLOAT image it "
           "builds\n");
    printf(" * (B=3i, G=2i, R=i), saved with EXR_COMPRESSION_ZSTD. Regenerate by "
           "building\n");
    printf(" * that image hosted and calling exr_save_to_memory(..., "
           "EXR_COMPRESSION_ZSTD),\n");
    printf(" * then dumping the bytes (see `make gen-fs-zstd-blob`). */\n");
    printf("static const unsigned char fs_zstd_exr[] = {\n");
    for (i = 0; i < sz; ++i) {
        printf("0x%02x,", ((const unsigned char *)buf)[i]);
        if ((i & 15) == 15) printf("\n");
    }
    printf("\n};\nstatic const unsigned fs_zstd_exr_len = %zuu;\n", sz);
    return 0;
}
