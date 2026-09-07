/*
 * TinyEXR HTJ2K (JPH) encode + decode fuzz harness.
 *
 * Exercises three surfaces on every input:
 *   1. Decode:     raw fuzz bytes -> exr_jph_decompress (multiple configs/dims)
 *   2. Encode:     fuzz bytes used as pixel data -> exr_jph_compress
 *   3. Round-trip: encode pixel data, decode back, verify bit-identity
 *
 * Build (coverage-guided, libFuzzer):
 *   make fuzz-jph
 *
 * Run:
 *   ./build/fuzz_jph -max_total_time=600 test/fuzzer/corpus_jph
 *
 * Build (deterministic corpus replay):
 *   make fuzz-jph-corpus
 *   ./build/fuzz_jph_replay test/unit/regression/*.exr test/fuzzer/corpus_jph/*
 *
 * Copyright (c) 2014-2026 Syoyo Fujita and TinyEXR authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "exr_internal.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Channel configurations to probe
 * ========================================================================== */

typedef struct {
    int            nc;
    exr_pixel_type types[4];
    const char    *names[4];
} Cfg;

static const Cfg k_cfgs[] = {
    /* All-HALF RGB */
    { 3, { EXR_PIXEL_HALF,  EXR_PIXEL_HALF,  EXR_PIXEL_HALF,  EXR_PIXEL_HALF  },
         { "B", "G", "R", NULL } },
    /* RGBA HALF */
    { 4, { EXR_PIXEL_HALF,  EXR_PIXEL_HALF,  EXR_PIXEL_HALF,  EXR_PIXEL_HALF  },
         { "A", "B", "G", "R" } },
    /* Single FLOAT (widens the kmax path to 33) */
    { 1, { EXR_PIXEL_FLOAT, EXR_PIXEL_HALF,  EXR_PIXEL_HALF,  EXR_PIXEL_HALF  },
         { "Z", NULL, NULL, NULL } },
    /* Mixed HALF + FLOAT */
    { 2, { EXR_PIXEL_HALF,  EXR_PIXEL_FLOAT, EXR_PIXEL_HALF,  EXR_PIXEL_HALF  },
         { "A", "Z", NULL, NULL } },
    /* Single UINT */
    { 1, { EXR_PIXEL_UINT,  EXR_PIXEL_HALF,  EXR_PIXEL_HALF,  EXR_PIXEL_HALF  },
         { "id", NULL, NULL, NULL } },
};
#define NCFG ((int)(sizeof(k_cfgs) / sizeof(k_cfgs[0])))

/* Image dimensions: a range that stresses small, medium, and near-codeblock
 * boundaries without making per-input cost unacceptable. */
static const struct { int w, h; } k_dims[] = {
    {  1,  1 },   /* degenerate */
    {  4,  4 },   /* tiny, covers first-quad edge cases */
    { 17, 11 },   /* odd dimensions */
    { 64, 32 },   /* medium, multiple codeblocks */
    { 128, 32 },  /* max codeblock width */
};
#define NDIM ((int)(sizeof(k_dims) / sizeof(k_dims[0])))

/* ============================================================================
 * Helpers
 * ============================================================================ */

static exr_result make_ctx(exr_codec_ctx *ctx, exr_channel chans[4],
                           const Cfg *c, int w, int h) {
    int i;
    memset(chans, 0, sizeof(exr_channel) * 4);
    for (i = 0; i < c->nc; ++i) {
        strncpy(chans[i].name, c->names[i], EXR_MAX_NAME - 1);
        chans[i].pixel_type = c->types[i];
        chans[i].x_sampling = 1;
        chans[i].y_sampling = 1;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->compression  = EXR_COMPRESSION_HTJ2K256;
    ctx->channels     = chans;
    ctx->num_channels = c->nc;
    ctx->x            = 0;
    ctx->y            = 0;
    ctx->width        = w;
    ctx->num_lines    = h;
    return EXR_SUCCESS;
}

static size_t block_size(const Cfg *c, int w, int h) {
    exr_channel chans[4];
    exr_codec_ctx ctx;
    size_t sz = 0;
    make_ctx(&ctx, chans, c, w, h);
    if (exr_block_uncompressed_size(chans, c->nc, 0, 0, w, h, &sz) != EXR_SUCCESS)
        return 0;
    return sz;
}

/* ============================================================================
 * 1. Decode: treat raw fuzz bytes as compressed JPH data
 * ============================================================================ */

static void fuzz_decode(const uint8_t *data, size_t size,
                        const Cfg *c, int w, int h) {
    exr_channel chans[4];
    exr_codec_ctx ctx;
    size_t out_sz;
    uint8_t *out;

    make_ctx(&ctx, chans, c, w, h);
    out_sz = block_size(c, w, h);
    if (out_sz == 0 || out_sz > (32u << 20)) return;

    out = (uint8_t *)malloc(out_sz);
    if (!out) return;

    /* Ignore return value — we let sanitizers catch UB, not error codes. */
    exr_jph_decompress(&ctx, data, size, out, out_sz);

    free(out);
}

/* ============================================================================
 * 2. Encode: fuzz bytes as pixel data
 * ============================================================================ */

static void fuzz_encode(const uint8_t *data, size_t size,
                        const Cfg *c, int w, int h) {
    exr_channel chans[4];
    exr_codec_ctx ctx;
    size_t in_sz;
    uint8_t *block;
    uint8_t *comp = NULL;
    size_t   comp_sz = 0;

    make_ctx(&ctx, chans, c, w, h);
    in_sz = block_size(c, w, h);
    if (in_sz == 0 || in_sz > (32u << 20)) return;

    block = (uint8_t *)malloc(in_sz);
    if (!block) return;

    /* Fill pixel block by cycling through fuzz bytes. */
    if (size > 0) {
        size_t i;
        for (i = 0; i < in_sz; ++i)
            block[i] = data[i % size];
    } else {
        memset(block, 0, in_sz);
    }

    if (exr_jph_compress(&ctx, block, in_sz, &comp, &comp_sz) == EXR_SUCCESS)
        free(comp);

    free(block);
}

/* ============================================================================
 * 3. Round-trip: encode fuzz-derived pixels, decode back, verify identity
 * ============================================================================ */

static void fuzz_roundtrip(const uint8_t *data, size_t size,
                           const Cfg *c, int w, int h) {
    exr_channel chans[4];
    exr_codec_ctx ctx;
    size_t in_sz;
    uint8_t *orig = NULL, *decoded = NULL;
    uint8_t *comp = NULL;
    size_t   comp_sz = 0;
    size_t i;

    make_ctx(&ctx, chans, c, w, h);
    in_sz = block_size(c, w, h);
    if (in_sz == 0 || in_sz > (32u << 20)) return;

    orig    = (uint8_t *)malloc(in_sz);
    decoded = (uint8_t *)malloc(in_sz);
    if (!orig || !decoded) goto done;

    /* Use fuzz bytes as pixel values (cycle). */
    if (size > 0) {
        for (i = 0; i < in_sz; ++i) orig[i] = data[i % size];
    } else {
        memset(orig, 0, in_sz);
    }

    if (exr_jph_compress(&ctx, orig, in_sz, &comp, &comp_sz) != EXR_SUCCESS)
        goto done;

    memset(decoded, 0xAA, in_sz);
    if (exr_jph_decompress(&ctx, comp, comp_sz, decoded, in_sz) != EXR_SUCCESS)
        goto done;

    /* HTJ2K with the 5/3 reversible wavelet is lossless for all pixel types.
     * A mismatch here is a real encoder/decoder bug. */
    if (memcmp(orig, decoded, in_sz) != 0)
        __builtin_trap(); /* Deliberately crash so libFuzzer records the input. */

done:
    free(comp);
    free(orig);
    free(decoded);
}

/* ============================================================================
 * LibFuzzer entry point
 * ============================================================================ */

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    int ci, di;

    /* ---- 1. Decode: feed raw bytes, all configs × all dims ---- */
    for (ci = 0; ci < NCFG; ++ci)
        for (di = 0; di < NDIM; ++di)
            fuzz_decode(data, size, &k_cfgs[ci], k_dims[di].w, k_dims[di].h);

    /* ---- 2. Encode: pixel data from fuzz bytes, subset of dims ---- */
    for (ci = 0; ci < NCFG; ++ci)
        for (di = 0; di < 3; ++di)  /* dims[0..2]: 1x1, 4x4, 17x11 */
            fuzz_encode(data, size, &k_cfgs[ci], k_dims[di].w, k_dims[di].h);

    /* ---- 3. Round-trip: smallest and medium dims ---- */
    for (ci = 0; ci < NCFG; ++ci) {
        fuzz_roundtrip(data, size, &k_cfgs[ci], k_dims[1].w, k_dims[1].h); /* 4x4 */
        fuzz_roundtrip(data, size, &k_cfgs[ci], k_dims[2].w, k_dims[2].h); /* 17x11 */
    }

    return 0;
}

/* ============================================================================
 * Standalone corpus replay (no libFuzzer dependency)
 * ============================================================================ */

#ifdef EXR_JPH_FUZZ_STANDALONE
#include <stdio.h>

int main(int argc, char **argv) {
    int i;
    for (i = 1; i < argc; ++i) {
        FILE *fp = fopen(argv[i], "rb");
        long n;
        uint8_t *buf;
        if (!fp) {
            fprintf(stderr, "  skip (open failed): %s\n", argv[i]);
            continue;
        }
        fseek(fp, 0, SEEK_END);
        n = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        if (n < 0) { fclose(fp); continue; }
        buf = (uint8_t *)malloc(n ? (size_t)n : 1);
        if (!buf) { fclose(fp); continue; }
        if (n > 0 && fread(buf, 1, (size_t)n, fp) != (size_t)n) { /* short read, ignore */ }
        fclose(fp);
        LLVMFuzzerTestOneInput(buf, (size_t)n);
        free(buf);
        printf("  ok (no crash): %s\n", argv[i]);
    }
    printf("fuzz_jph corpus replay: %d file(s), no crash / no sanitizer error\n",
           argc - 1);
    return 0;
}
#endif /* EXR_JPH_FUZZ_STANDALONE */
