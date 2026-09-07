/*
 * exrinfo - tiny native example for the pure-C11 v3 API.
 *
 * Demonstrates the stdio convenience layer (exr_load_from_file, from the
 * optional src/exr_stdio.c) and walking the decoded image. Native programs are
 * free to use stdio; the core library itself is freestanding.
 *
 *     cc -Iinclude exrinfo.c build/libtinyexr3.a -o exrinfo
 *     ./exrinfo asakusa.exr [out.exr]
 *
 * Copyright (c) 2014-2026 Syoyo Fujita and TinyEXR authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "exr.h"

#include <stdio.h>

static const char *comp_name(exr_compression c) {
    switch (c) {
    case EXR_COMPRESSION_NONE: return "none";
    case EXR_COMPRESSION_RLE: return "rle";
    case EXR_COMPRESSION_ZIPS: return "zips";
    case EXR_COMPRESSION_ZIP: return "zip";
    case EXR_COMPRESSION_PIZ: return "piz";
    case EXR_COMPRESSION_PXR24: return "pxr24";
    case EXR_COMPRESSION_B44: return "b44";
    case EXR_COMPRESSION_B44A: return "b44a";
    case EXR_COMPRESSION_DWAA: return "dwaa";
    case EXR_COMPRESSION_DWAB: return "dwab";
    case EXR_COMPRESSION_HTJ2K256: return "htj2k256";
    case EXR_COMPRESSION_HTJ2K32: return "htj2k32";
    case EXR_COMPRESSION_ZSTD: return "zstd";
    }
    return "?";
}

int main(int argc, char **argv) {
    exr_image img;
    exr_result rc;
    int p, c;

    if (argc < 2) {
        fprintf(stderr, "usage: %s <input.exr> [output.exr]\n", argv[0]);
        return 2;
    }
    rc = exr_load_from_file(argv[1], NULL, &img);
    if (!EXR_OK(rc)) {
        fprintf(stderr, "load failed: %s\n", exr_result_string(rc));
        return 1;
    }

    printf("%s: %d part(s)\n", argv[1], img.num_parts);
    for (p = 0; p < img.num_parts; ++p) {
        const exr_part *pt = &img.parts[p];
        printf("  part %d: %dx%d  %s%s  compression=%s  channels=%d\n", p,
               pt->width, pt->height, pt->header.tiled ? "tiled" : "scanline",
               pt->is_deep ? " deep" : "", comp_name(pt->header.compression),
               pt->header.num_channels);
        for (c = 0; c < pt->header.num_channels; ++c) {
            const exr_channel *ch = &pt->header.channels[c];
            const char *t = ch->pixel_type == EXR_PIXEL_HALF    ? "half"
                            : ch->pixel_type == EXR_PIXEL_FLOAT ? "float"
                                                                : "uint";
            printf("    [%d] %-8s %s (sampling %dx%d)\n", c, ch->name, t,
                   ch->x_sampling, ch->y_sampling);
        }
    }

    if (argc >= 3) {
        rc = exr_save_to_file(argv[2], &img, EXR_COMPRESSION_ZIP);
        if (!EXR_OK(rc)) {
            fprintf(stderr, "save failed: %s\n", exr_result_string(rc));
            exr_image_free(&img);
            return 1;
        }
        printf("wrote %s (zip)\n", argv[2]);
    }

    exr_image_free(&img);
    return 0;
}
