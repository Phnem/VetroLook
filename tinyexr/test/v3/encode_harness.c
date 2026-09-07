/*
 * encode_harness - load an EXR with the v3 API and re-save it with a chosen
 * compression. Used to validate the HTJ2K encoder (incl. mixed half/float).
 *
 * Usage: encode_harness <in.exr> <out.exr> <compression>
 *   compression: none rle zips zip piz pxr24 b44 b44a htj2k256 htj2k32 zstd
 *
 * Copyright (c) 2014-2026 Syoyo Fujita and TinyEXR authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdio.h>
#include <string.h>

#include "exr.h"

static int parse_comp(const char *s, exr_compression *out) {
    struct { const char *n; exr_compression c; } t[] = {
        {"none", EXR_COMPRESSION_NONE}, {"rle", EXR_COMPRESSION_RLE},
        {"zips", EXR_COMPRESSION_ZIPS}, {"zip", EXR_COMPRESSION_ZIP},
        {"piz", EXR_COMPRESSION_PIZ}, {"pxr24", EXR_COMPRESSION_PXR24},
        {"b44", EXR_COMPRESSION_B44}, {"b44a", EXR_COMPRESSION_B44A},
        {"htj2k256", EXR_COMPRESSION_HTJ2K256},
        {"htj2k32", EXR_COMPRESSION_HTJ2K32}, {"zstd", EXR_COMPRESSION_ZSTD},
    };
    size_t i;
    for (i = 0; i < sizeof(t) / sizeof(t[0]); ++i)
        if (strcmp(s, t[i].n) == 0) { *out = t[i].c; return 1; }
    return 0;
}

int main(int argc, char **argv) {
    exr_image img;
    exr_compression comp;
    exr_result rc;
    if (argc != 4) {
        fprintf(stderr, "usage: %s <in.exr> <out.exr> <compression>\n", argv[0]);
        return 2;
    }
    if (!parse_comp(argv[3], &comp)) {
        fprintf(stderr, "unknown compression '%s'\n", argv[3]);
        return 2;
    }
    rc = exr_load_from_file(argv[1], NULL, &img);
    if (!EXR_OK(rc)) {
        fprintf(stderr, "load %s: %s\n", argv[1], exr_result_string(rc));
        return 1;
    }
    rc = exr_save_to_file(argv[2], &img, comp);
    if (!EXR_OK(rc)) {
        fprintf(stderr, "save %s (%s): %s\n", argv[2], argv[3],
                exr_result_string(rc));
        exr_image_free(&img);
        return 1;
    }
    exr_image_free(&img);
    printf("encoded %s -> %s (%s)\n", argv[1], argv[2], argv[3]);
    return 0;
}
