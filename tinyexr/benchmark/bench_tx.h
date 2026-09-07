/*
 * Clean tinyexr-side wrapper for the OpenEXR comparison benchmark.
 *
 * tinyexr's exr.h and OpenEXR's C core (openexr_attr.h, pulled in by the Imf
 * C++ headers) declare the SAME global enum names (EXR_PIXEL_UINT,
 * EXR_TILE_ROUND_DOWN, exr_compression, ...), so they cannot be included in one
 * translation unit. This header exposes the tinyexr codec timing without
 * leaking exr.h, letting bench_compare.cpp include the OpenEXR headers freely.
 *
 * Copyright (c) 2014-2026 Syoyo Fujita and TinyEXR authors
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef BENCH_TX_H
#define BENCH_TX_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    double enc_mpix;
    double dec_mpix;
    size_t size; /* compressed bytes */
    size_t peak_bytes; /* peak live TinyEXR allocation during the run */
    int ok;
} bench_tx_result;

/* Load the source image. Returns 1 on success and fills *w,*h. */
int bench_tx_load(const char *path, int *w, int *h);

/* Canonical codec list (also drives the OpenEXR side ordering by name). */
int bench_tx_codec_count(void);
const char *bench_tx_codec_name(int i);

/* Encode+decode timing for codec i on the loaded source. */
bench_tx_result bench_tx_run(int i, double mpix);

void bench_tx_unload(void);
const char *bench_tx_simd_info(void);

/* Set tinyexr's worker-thread count (no-op unless built with THREADS=1). */
void bench_tx_set_threads(int n);

#ifdef __cplusplus
}
#endif

#endif /* BENCH_TX_H */
