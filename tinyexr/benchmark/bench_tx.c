/*
 * tinyexr-side implementation for the OpenEXR comparison benchmark.
 * Compiled as C (includes exr.h); see bench_tx.h for the rationale.
 *
 * Copyright (c) 2014-2026 Syoyo Fujita and TinyEXR authors
 * SPDX-License-Identifier: BSD-3-Clause
 */
#define _POSIX_C_SOURCE 199309L

#include "bench_tx.h"
#include "exr.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct {
    size_t size;
    max_align_t alignment;
} TxAllocHeader;

static size_t g_alloc_live = 0;
static size_t g_alloc_peak = 0;

static void *bench_alloc(void *user, size_t size) {
    TxAllocHeader *h;
    (void)user;
    h = (TxAllocHeader *)malloc(sizeof(*h) + (size ? size : 1u));
    if (!h) return NULL;
    h->size = size;
    g_alloc_live += size;
    if (g_alloc_live > g_alloc_peak) g_alloc_peak = g_alloc_live;
    return (void *)(h + 1);
}

static void bench_free(void *user, void *ptr) {
    TxAllocHeader *h;
    (void)user;
    if (!ptr) return;
    h = ((TxAllocHeader *)ptr) - 1;
    g_alloc_live -= h->size;
    free(h);
}

static const exr_allocator g_allocator = {NULL, bench_alloc, bench_free};

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* Canonical codec list shared with the OpenEXR side (matched by name). */
static const struct {
    const char *name;
    exr_compression comp;
} g_codecs[] = {
    {"none", EXR_COMPRESSION_NONE},   {"rle", EXR_COMPRESSION_RLE},
    {"zips", EXR_COMPRESSION_ZIPS},   {"zip", EXR_COMPRESSION_ZIP},
    {"piz", EXR_COMPRESSION_PIZ},     {"pxr24", EXR_COMPRESSION_PXR24},
    {"b44", EXR_COMPRESSION_B44},     {"htj2k256", EXR_COMPRESSION_HTJ2K256},
    {"htj2k32", EXR_COMPRESSION_HTJ2K32},
};

static exr_image g_src;
static int g_loaded = 0;
static exr_context *g_ctx = NULL;

int bench_tx_load(const char *path, int *w, int *h) {
    memset(&g_src, 0, sizeof(g_src));
    if (!EXR_OK(exr_context_create(&g_allocator, &g_ctx)) ||
        !EXR_OK(exr_load_from_file_ctx(g_ctx, path, &g_allocator, &g_src))) {
        exr_context_destroy(g_ctx);
        g_ctx = NULL;
        return 0;
    }
    g_loaded = 1;
    if (w) *w = g_src.parts[0].width;
    if (h) *h = g_src.parts[0].height;
    return 1;
}

int bench_tx_codec_count(void) {
    return (int)(sizeof(g_codecs) / sizeof(g_codecs[0]));
}

const char *bench_tx_codec_name(int i) {
    if (i < 0 || i >= bench_tx_codec_count()) return "?";
    return g_codecs[i].name;
}

bench_tx_result bench_tx_run(int i, double mpix) {
    bench_tx_result r;
    exr_compression comp;
    void *buf = NULL;
    size_t sz = 0;

    r.enc_mpix = r.dec_mpix = 0;
    r.size = 0;
    r.peak_bytes = 0;
    r.ok = 0;
    if (!g_loaded || i < 0 || i >= bench_tx_codec_count()) return r;
    comp = g_codecs[i].comp;

    {
        size_t run_base = g_alloc_live;
        g_alloc_peak = g_alloc_live;
        if (!EXR_OK(exr_save_to_memory_ctx(g_ctx, &buf, &sz, &g_allocator,
                                           &g_src, comp)))
            return r;
        r.peak_bytes = g_alloc_peak > run_base ? g_alloc_peak - run_base : 0;
    }
    r.size = sz;

    if (!getenv("EXR_BENCH_DECODE_ONLY")) { /* encode */
        double t0 = now_sec();
        long it = 0;
        do {
            void *o = NULL;
            size_t os = 0;
            if (EXR_OK(exr_save_to_memory_ctx(g_ctx, &o, &os, &g_allocator,
                                              &g_src, comp)))
                bench_free(NULL, o);
            ++it;
        } while (now_sec() - t0 < 0.3);
        r.enc_mpix = mpix / ((now_sec() - t0) / (double)it);
    }
    if (!getenv("EXR_BENCH_ENCODE_ONLY")) { /* decode */
        double t0 = now_sec();
        long it = 0;
        long failures = 0;
        exr_result last_rc = EXR_SUCCESS;
        do {
            exr_image img;
            memset(&img, 0, sizeof(img));
            last_rc = exr_load_from_memory_ctx(g_ctx, buf, sz, &g_allocator,
                                               &img);
            if (EXR_OK(last_rc))
                exr_image_free(&img);
            else
                ++failures;
            ++it;
        } while (now_sec() - t0 < 0.3);
        if (failures)
            fprintf(stderr, "tinyexr: %ld HTJ2K decode failures (%s)\n",
                    failures, exr_result_string(last_rc));
        r.dec_mpix = mpix / ((now_sec() - t0) / (double)it);
    }
    bench_free(NULL, buf);
    r.ok = 1;
    return r;
}

void bench_tx_unload(void) {
    if (g_loaded) {
        exr_image_free(&g_src);
        exr_context_destroy(g_ctx);
        g_ctx = NULL;
        g_loaded = 0;
    }
}

const char *bench_tx_simd_info(void) { return exr_simd_info(); }

void bench_tx_set_threads(int n) { exr_set_num_threads(n); }
