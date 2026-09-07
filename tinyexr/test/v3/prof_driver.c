/* Profiling driver: loop decode and/or encode of an EXR for gprof.
 * usage: prof_driver <file.exr> <iters> <dec|enc|both> [comp]
 * SPDX-License-Identifier: BSD-3-Clause */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "exr.h"

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s <file> <iters> <dec|enc|both> [comp]\n", argv[0]); return 2; }
    const char *path = argv[1];
    int iters = atoi(argv[2]);
    const char *mode = argv[3];
    exr_compression comp = EXR_COMPRESSION_HTJ2K256;
    if (argc >= 5 && strcmp(argv[4], "htj2k32") == 0) comp = EXR_COMPRESSION_HTJ2K32;

    /* read file once into memory */
    FILE *fp = fopen(path, "rb");
    if (!fp) { perror("open"); return 1; }
    fseek(fp, 0, SEEK_END); long sz = ftell(fp); fseek(fp, 0, SEEK_SET);
    unsigned char *buf = malloc((size_t)sz);
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) { return 1; }
    fclose(fp);

    int do_dec = strcmp(mode, "enc") != 0;
    int do_enc = strcmp(mode, "dec") != 0;

    /* For encode, load once to an image we can re-save. */
    exr_image base; memset(&base, 0, sizeof(base));
    if (do_enc) {
        if (!EXR_OK(exr_load_from_memory(buf, (size_t)sz, NULL, &base))) {
            fprintf(stderr, "enc: base load failed\n"); return 1;
        }
    }

    for (int i = 0; i < iters; ++i) {
        if (do_dec) {
            exr_image img; memset(&img, 0, sizeof(img));
            exr_result rc = exr_load_from_memory(buf, (size_t)sz, NULL, &img);
            if (!EXR_OK(rc)) { fprintf(stderr, "dec fail %s\n", exr_result_string(rc)); return 1; }
            exr_image_free(&img);
        }
        if (do_enc) {
            void *out = NULL; size_t osz = 0;
            exr_result rc = exr_save_to_memory(&out, &osz, NULL, &base, comp);
            if (!EXR_OK(rc)) { fprintf(stderr, "enc fail %s\n", exr_result_string(rc)); return 1; }
            free(out);
        }
    }
    if (do_enc) exr_image_free(&base);
    free(buf);
    printf("done %s x%d %s\n", path, iters, mode);
    return 0;
}
