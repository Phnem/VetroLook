/* bench_decode - time TinyEXR v3 in-memory decode over a corpus.
 *
 * Reads NUL- or newline-separated file paths from stdin, decodes each once
 * (file is slurped into RAM first; only exr_load_from_memory is timed), and
 * reports per-compression and total wall-clock decode throughput.
 *
 * usage: find DIR -name '*.exr' | bench_decode
 *
 * SPDX-License-Identifier: BSD-3-Clause */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "exr.h"

static const char *compression_name(exr_compression c) {
    switch (c) {
        case EXR_COMPRESSION_NONE: return "NONE";
        case EXR_COMPRESSION_RLE: return "RLE";
        case EXR_COMPRESSION_ZIPS: return "ZIPS";
        case EXR_COMPRESSION_ZIP: return "ZIP";
        case EXR_COMPRESSION_PIZ: return "PIZ";
        case EXR_COMPRESSION_PXR24: return "PXR24";
        case EXR_COMPRESSION_B44: return "B44";
        case EXR_COMPRESSION_B44A: return "B44A";
        case EXR_COMPRESSION_DWAA: return "DWAA";
        case EXR_COMPRESSION_DWAB: return "DWAB";
        case EXR_COMPRESSION_HTJ2K256: return "HTJ2K256";
        case EXR_COMPRESSION_HTJ2K32: return "HTJ2K32";
        case EXR_COMPRESSION_ZSTD: return "ZSTD";
        default: return "UNKNOWN";
    }
}

#define NCOMP 14
struct acc {
    long files;
    long fails;
    double secs;
    double mpix;     /* megapixel-channels decoded */
    double in_mb;    /* compressed input MB */
};

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

int main(void) {
    struct acc by[NCOMP];
    memset(by, 0, sizeof(by));
    struct acc tot;
    memset(&tot, 0, sizeof(tot));

    char line[8192];
    while (fgets(line, sizeof(line), stdin)) {
        size_t n = strlen(line);
        while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0;
        if (n == 0) continue;

        FILE *fp = fopen(line, "rb");
        if (!fp) continue;
        fseek(fp, 0, SEEK_END);
        long sz = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        if (sz <= 0) { fclose(fp); continue; }
        unsigned char *buf = malloc((size_t)sz);
        if (!buf) { fclose(fp); continue; }
        if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) { free(buf); fclose(fp); continue; }
        fclose(fp);

        /* peek compression (first part) for bucketing */
        exr_compression comp = (exr_compression)0xff;
        exr_reader *r = NULL;
        if (EXR_OK(exr_reader_open_memory(buf, (size_t)sz, NULL, &r)) &&
            EXR_OK(exr_reader_parse_header(r))) {
            const exr_header *h = exr_reader_part_header(r, 0);
            if (h) comp = h->compression;
        }
        if (r) exr_reader_close(r);
        int ci = ((int)comp >= 0 && (int)comp < NCOMP) ? (int)comp : 0;

        exr_image img;
        memset(&img, 0, sizeof(img));
        double t0 = now_s();
        exr_result rc = exr_load_from_memory(buf, (size_t)sz, NULL, &img);
        double dt = now_s() - t0;

        if (!EXR_OK(rc)) {
            by[ci].fails++;
            tot.fails++;
            free(buf);
            continue;
        }

        double mpix = 0;
        for (int32_t p = 0; p < img.num_parts; p++) {
            double px = (double)img.parts[p].width * (double)img.parts[p].height;
            mpix += px * (double)img.parts[p].header.num_channels;
        }
        mpix /= 1e6;

        by[ci].files++;  by[ci].secs += dt;  by[ci].mpix += mpix;  by[ci].in_mb += sz / 1e6;
        tot.files++;     tot.secs += dt;     tot.mpix += mpix;     tot.in_mb += sz / 1e6;

        exr_image_free(&img);
        free(buf);
    }

    printf("%-10s %7s %6s %10s %10s %10s %10s\n",
           "codec", "files", "fail", "decode_s", "Mpix-ch", "Mpix/s", "in_MB/s");
    printf("------------------------------------------------------------------------\n");
    for (int i = 0; i < NCOMP; i++) {
        if (by[i].files == 0 && by[i].fails == 0) continue;
        double mps = by[i].secs > 0 ? by[i].mpix / by[i].secs : 0;
        double mbs = by[i].secs > 0 ? by[i].in_mb / by[i].secs : 0;
        printf("%-10s %7ld %6ld %10.3f %10.1f %10.1f %10.1f\n",
               compression_name((exr_compression)i), by[i].files, by[i].fails,
               by[i].secs, by[i].mpix, mps, mbs);
    }
    printf("------------------------------------------------------------------------\n");
    double mps = tot.secs > 0 ? tot.mpix / tot.secs : 0;
    double mbs = tot.secs > 0 ? tot.in_mb / tot.secs : 0;
    printf("%-10s %7ld %6ld %10.3f %10.1f %10.1f %10.1f\n",
           "TOTAL", tot.files, tot.fails, tot.secs, tot.mpix, mps, mbs);
    return 0;
}
