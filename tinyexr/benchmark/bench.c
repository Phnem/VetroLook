/*
 * TinyEXR v3 - standalone decode/encode + SIMD-kernel benchmark.
 *
 * Usage:
 *   make bench            # builds + runs on asakusa.exr
 *   ./build/bench [file.exr ...]
 *
 * Reports, per codec, decode and encode throughput (megapixels/s), and per
 * SIMD primitive (byte de-interleave, predictor, half<->float) the throughput at
 * each available ISA tier (scalar / SSE2 / AVX2 / F16C on x86, scalar / NEON on
 * aarch64), with speedups.
 *
 * Copyright (c) 2014-2026 Syoyo Fujita and TinyEXR authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#define _POSIX_C_SOURCE 199309L

#include "exr.h"
#include "exr_internal.h" /* kernel symbols + exr_simd_force (benchmark-only) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* Run `fn(arg)` repeatedly for >= min_sec; return seconds/iteration. */
typedef void (*bench_fn)(void *);
static double timeit(bench_fn fn, void *arg, double min_sec, long *out_iters) {
    long iters = 0;
    double t0 = now_sec(), elapsed;
    do {
        fn(arg);
        ++iters;
        elapsed = now_sec() - t0;
    } while (elapsed < min_sec);
    if (out_iters) *out_iters = iters;
    return elapsed / (double)iters;
}

/* ---- codec decode / encode ------------------------------------------------ */

struct codec_ctx {
    const exr_image *src;  /* for encode */
    const void *buf;       /* for decode */
    size_t buf_size;
    exr_compression comp;
};

static void do_encode(void *p) {
    struct codec_ctx *c = (struct codec_ctx *)p;
    void *out = NULL;
    size_t sz = 0;
    if (EXR_OK(exr_save_to_memory(&out, &sz, NULL, c->src, c->comp))) free(out);
}
static void do_decode(void *p) {
    struct codec_ctx *c = (struct codec_ctx *)p;
    exr_image img;
    memset(&img, 0, sizeof(img));
    if (EXR_OK(exr_load_from_memory(c->buf, c->buf_size, NULL, &img)))
        exr_image_free(&img);
}

static void bench_codecs(const char *path) {
    static const exr_compression codecs[] = {
        EXR_COMPRESSION_NONE,  EXR_COMPRESSION_RLE,  EXR_COMPRESSION_ZIPS,
        EXR_COMPRESSION_ZIP,   EXR_COMPRESSION_PIZ,  EXR_COMPRESSION_PXR24,
        EXR_COMPRESSION_B44,   EXR_COMPRESSION_B44A,
        EXR_COMPRESSION_HTJ2K32, EXR_COMPRESSION_HTJ2K256};
    static const char *names[] = {"none", "rle", "zips", "zip", "piz",
                                  "pxr24", "b44", "b44a", "htj2k32",
                                  "htj2k256"};
    exr_image src;
    double mpix;
    size_t i;

    memset(&src, 0, sizeof(src));
    if (!EXR_OK(exr_load_from_file(path, NULL, &src))) {
        printf("  (could not load %s)\n", path);
        return;
    }
    mpix = (double)src.parts[0].width * src.parts[0].height / 1e6;
    printf("\n== codec throughput: %s (%dx%d, %d ch) ==\n", path,
           src.parts[0].width, src.parts[0].height,
           src.parts[0].header.num_channels);
    printf("  %-6s %12s %12s %10s\n", "codec", "encode MP/s", "decode MP/s",
           "size");

    for (i = 0; i < sizeof(codecs) / sizeof(codecs[0]); ++i) {
        struct codec_ctx ec, dc;
        void *buf = NULL;
        size_t sz = 0;
        double te, td;
        if (!EXR_OK(exr_save_to_memory(&buf, &sz, NULL, &src, codecs[i]))) {
            printf("  %-6s   (encode unsupported)\n", names[i]);
            continue;
        }
        ec.src = &src;
        ec.comp = codecs[i];
        te = timeit(do_encode, &ec, 0.3, NULL);
        dc.buf = buf;
        dc.buf_size = sz;
        dc.comp = codecs[i];
        td = timeit(do_decode, &dc, 0.3, NULL);
        printf("  %-6s %12.1f %12.1f %9.0fK\n", names[i], mpix / te, mpix / td,
               sz / 1024.0);
        free(buf);
    }

    /* end-to-end SIMD tier comparison on the ZIP decode path */
    {
        void *buf = NULL;
        size_t sz = 0;
        if (EXR_OK(exr_save_to_memory(&buf, &sz, NULL, &src, EXR_COMPRESSION_ZIP))) {
            struct codec_ctx dc;
            int lvl;
#if defined(EXR_NEON)
            /* On aarch64, exr_simd_force(>=1) selects the NEON kernels; there is
             * no separate sse2/avx2 tier, so sweep scalar vs neon only. */
            const char *lv[] = {"scalar", "neon"};
            const int ntier = 2;
#else
            const char *lv[] = {"scalar", "sse2", "avx2"};
            const int ntier = 3;
#endif
            dc.buf = buf;
            dc.buf_size = sz;
            printf("  ZIP decode by forced SIMD tier (predictor + de-interleave):\n");
            for (lvl = 0; lvl < ntier; ++lvl) {
                double td;
                exr_simd_force(lvl);
                td = timeit(do_decode, &dc, 0.3, NULL);
                printf("    %-7s %10.1f MP/s\n", lv[lvl], mpix / td);
            }
            exr_simd_init(); /* restore best */
            free(buf);
        }
    }

    /* end-to-end SIMD tier comparison on the HTJ2K (JPH) path. The JPH codec
     * dispatches its SIMD kernels (NLT type-3, half pack, 5/3 inverse) off
     * exr_cpu_caps() directly, so forcing the cap tier exercises the whole
     * HTJ2K encode/decode pipeline, not just the de-interleave. */
    {
        void *buf = NULL;
        size_t sz = 0;
        if (EXR_OK(exr_save_to_memory(&buf, &sz, NULL, &src,
                                      EXR_COMPRESSION_HTJ2K256))) {
            struct codec_ctx dc, ec;
            int lvl;
#if defined(EXR_NEON)
            /* NEON JPH kernels are compile-time selected on aarch64; report a
             * single representative row rather than synthetic x86 tiers. */
            const char *lv[] = {"neon"};
            const int ntier = 1;
#else
            const char *lv[] = {"scalar", "sse4.1", "avx2"};
            const int ntier = 3;
#endif
            dc.buf = buf;
            dc.buf_size = sz;
            dc.comp = EXR_COMPRESSION_HTJ2K256;
            ec.src = &src;
            ec.comp = EXR_COMPRESSION_HTJ2K256;
            printf("  HTJ2K256 by forced SIMD tier (JPH dispatch):\n");
            printf("    %-7s %12s %12s\n", "tier", "encode MP/s", "decode MP/s");
            for (lvl = 0; lvl < ntier; ++lvl) {
                double te, td;
#if defined(EXR_NEON)
                exr_simd_force(1);       /* NEON interleave/util; JPH stays scalar */
#else
                exr_cpu_caps_force(lvl); /* JPH path */
                exr_simd_force(lvl);     /* interleave/half table, for parity */
#endif
                te = timeit(do_encode, &ec, 0.3, NULL);
                td = timeit(do_decode, &dc, 0.3, NULL);
                printf("    %-7s %12.1f %12.1f\n", lv[lvl], mpix / te, mpix / td);
            }
            exr_cpu_caps_force(-1); /* restore real detection */
            exr_simd_init();        /* restore best */
            free(buf);
        }
    }
    exr_image_free(&src);
}

/* ---- SIMD micro-kernels --------------------------------------------------- */

static void bench_kernels(void) {
    const size_t N = 64u * 1024u * 1024u; /* 64 MB */
    uint8_t *src = (uint8_t *)malloc(N), *dst = (uint8_t *)malloc(N);
    size_t i;
    double t;
    long it;
    uint32_t caps = exr_simd_capabilities();
    (void)caps; /* only consulted by the x86 tier tables below */

    if (!src || !dst) { free(src); free(dst); return; }
    for (i = 0; i < N; ++i) src[i] = (uint8_t)i;

    printf("\n== SIMD kernels (%.0f MB buffers) ==\n", N / 1048576.0);

    printf("  byte de-interleave:\n");
    {
        struct { const char *n; void (*f)(const uint8_t *, uint8_t *, size_t); int ok; } v[] = {
            {"scalar", exr_interleave_scalar, 1},
#if defined(EXR_X86)
            {"sse2", exr_interleave_sse2, (caps & EXR_SIMD_SSE2) != 0},
            {"avx2", exr_interleave_avx2, (caps & EXR_SIMD_AVX2) != 0},
#elif defined(EXR_NEON)
            {"neon", exr_interleave_neon, 1},
#endif
        };
        double base = 0;
        size_t k;
        for (k = 0; k < sizeof(v) / sizeof(v[0]); ++k) {
            double gbs;
            if (!v[k].ok) { printf("    %-7s   (unavailable)\n", v[k].n); continue; }
            { /* inline timing (kernel sig differs from bench_fn) */
                double t0 = now_sec();
                long n = 0;
                do { v[k].f(src, dst, N); ++n; } while (now_sec() - t0 < 0.3);
                t = (now_sec() - t0) / (double)n;
                it = n;
            }
            gbs = (double)N / t / 1e9;
            if (k == 0) base = gbs;
            printf("    %-7s %8.2f GB/s  (%.2fx)  [%ld it]\n", v[k].n, gbs,
                   base > 0 ? gbs / base : 1.0, it);
        }
    }

    printf("  predictor decode (delta prefix-sum):\n");
    {
        struct { const char *n; void (*f)(uint8_t *, size_t); int ok; } v[] = {
            {"scalar", exr_predictor_decode_scalar, 1},
#if defined(EXR_X86)
            {"sse2", exr_predictor_decode_sse2, (caps & EXR_SIMD_SSE2) != 0},
#elif defined(EXR_NEON)
            {"neon", exr_predictor_decode_neon, 1},
#endif
        };
        double base = 0;
        size_t k;
        memcpy(dst, src, N);
        for (k = 0; k < sizeof(v) / sizeof(v[0]); ++k) {
            double gbs;
            if (!v[k].ok) { printf("    %-7s   (unavailable)\n", v[k].n); continue; }
            { /* in-place prefix-sum; data-independent, so repeat on dst directly
               * (values wrap mod 256 — harmless, the per-byte work is identical) */
                double t0 = now_sec();
                long n = 0;
                do { v[k].f(dst, N); ++n; } while (now_sec() - t0 < 0.3);
                t = (now_sec() - t0) / (double)n;
                it = n;
            }
            gbs = (double)N / t / 1e9;
            if (k == 0) base = gbs;
            printf("    %-7s %8.2f GB/s  (%.2fx)  [%ld it]\n", v[k].n, gbs,
                   base > 0 ? gbs / base : 1.0, it);
        }
    }

    printf("  half->float:\n");
    {
        size_t M = N / 2; /* halves */
        uint16_t *h = (uint16_t *)src;
        float *f = (float *)malloc(M * sizeof(float));
        if (f) {
            double base = 0, t0, gbs;
            long n;
            t0 = now_sec(); n = 0;
            do { exr_half_to_float_scalar(h, f, M); ++n; } while (now_sec() - t0 < 0.3);
            t = (now_sec() - t0) / n;
            base = (double)M * 6 / t / 1e9; /* 2B in + 4B out */
            printf("    %-7s %8.2f GB/s  (1.00x)\n", "scalar", base);
#if defined(EXR_X86)
            if (caps & EXR_SIMD_AVX2) {
                t0 = now_sec(); n = 0;
                do { exr_half_to_float_f16c(h, f, M); ++n; } while (now_sec() - t0 < 0.3);
                t = (now_sec() - t0) / n;
                gbs = (double)M * 6 / t / 1e9;
                printf("    %-7s %8.2f GB/s  (%.2fx)\n", "f16c", gbs, gbs / base);
            }
#elif defined(EXR_NEON)
            {
                t0 = now_sec(); n = 0;
                do { exr_half_to_float_neon(h, f, M); ++n; } while (now_sec() - t0 < 0.3);
                t = (now_sec() - t0) / n;
                gbs = (double)M * 6 / t / 1e9;
                printf("    %-7s %8.2f GB/s  (%.2fx)\n", "neon", gbs, gbs / base);
            }
#endif
            (void)gbs;
            free(f);
        }
    }

#if defined(EXR_X86)
    printf("  fpnge Huffman lookup (PSHUFB):\n");
    {
        const exr_allocator *al = exr_default_allocator();
        uint64_t freqs[286];
        exr_fpnge_table tbl;
        uint8_t *nbo = (uint8_t *)malloc(N), *blo = (uint8_t *)malloc(N),
                *bho = (uint8_t *)malloc(N);
        size_t k;
        double base = 0;
        struct {
            const char *n;
            void (*f)(const exr_fpnge_table *, const uint8_t *, size_t, uint8_t *,
                      uint8_t *, uint8_t *);
            int ok;
        } v[] = {
            {"scalar", exr_fpnge_lookup_scalar, 1},
            {"sse4.1", exr_fpnge_lookup_sse41, (caps & EXR_SIMD_SSE41) != 0},
            {"avx2", exr_fpnge_lookup_avx2, (caps & EXR_SIMD_AVX2) != 0},
        };
        memset(freqs, 0, sizeof(freqs));
        for (i = 0; i < N; ++i) freqs[src[i]]++;
        if (nbo && blo && bho && exr_fpnge_build_table(al, freqs, &tbl)) {
            for (k = 0; k < 3; ++k) {
                double t0, gbs;
                long n2 = 0;
                if (!v[k].ok) { printf("    %-7s   (unavailable)\n", v[k].n); continue; }
                t0 = now_sec();
                do { v[k].f(&tbl, src, N, nbo, blo, bho); ++n2; } while (now_sec() - t0 < 0.3);
                gbs = (double)N * n2 / (now_sec() - t0) / 1e9;
                if (k == 0) base = gbs;
                printf("    %-7s %8.2f GB/s  (%.2fx)\n", v[k].n, gbs,
                       base > 0 ? gbs / base : 1.0);
            }
        }
        free(nbo);
        free(blo);
        free(bho);
    }
#endif
    free(src);
    free(dst);
}

/* ---- DEFLATE encoders on real (predictor+split) EXR bytes ----------------- */

static void bench_deflate(const char *path) {
    exr_image img;
    exr_part *p;
    size_t n = 0, c;
    uint8_t *raw, *tmp, *cur;
    const exr_allocator *a = exr_default_allocator();

    memset(&img, 0, sizeof(img));
    if (!EXR_OK(exr_load_from_file(path, NULL, &img))) return;
    p = &img.parts[0];
    for (c = 0; c < (size_t)p->header.num_channels; ++c) {
        size_t ps = p->header.channels[c].pixel_type == EXR_PIXEL_HALF ? 2 : 4;
        n += (size_t)p->width * p->height * ps;
    }
    raw = (uint8_t *)malloc(n);
    tmp = (uint8_t *)malloc(n);
    if (!raw || !tmp) { free(raw); free(tmp); exr_image_free(&img); return; }
    cur = raw;
    for (c = 0; c < (size_t)p->header.num_channels; ++c) {
        size_t ps = p->header.channels[c].pixel_type == EXR_PIXEL_HALF ? 2 : 4;
        size_t cb = (size_t)p->width * p->height * ps;
        memcpy(cur, p->images[c], cb);
        cur += cb;
    }
    /* EXR ZIP intermediate transform */
    exr_interleave_encode(raw, tmp, n);
    exr_predictor_encode(tmp, n);

    printf("\n== DEFLATE encode on EXR bytes: %s (%.2f MB) ==\n", path,
           n / 1048576.0);
    printf("  %-18s %10s %12s\n", "encoder", "MB/s", "out (ratio)");
    {
        struct { const char *name; int kind; } v[] = {
            {"fpng LZ77 (scalar)", 0},
            {"fpnge PSHUFB-huff scalar", 1},
#if defined(EXR_X86)
            {"fpnge PSHUFB-huff sse4.1", 2}, /* PSHUFB lookup is x86-only */
#endif
        };
        size_t k;
        for (k = 0; k < sizeof(v) / sizeof(v[0]); ++k) {
            double t0 = now_sec(), t;
            long it = 0;
            uint8_t *out = NULL;
            size_t os = 0;
            do {
                if (out) free(out);
                out = NULL;
                if (v[k].kind == 0) exr_deflate_zlib(a, tmp, n, &out, &os);
                else exr_fpnge_deflate(a, tmp, n, &out, &os, v[k].kind == 2);
                ++it;
            } while (now_sec() - t0 < 0.3);
            t = (now_sec() - t0) / it;
            printf("  %-18s %10.1f %9zuK (%.2fx)\n", v[k].name,
                   n / t / 1e6, os / 1024, (double)n / (double)os);
            free(out);
        }
    }
    free(raw);
    free(tmp);
    exr_image_free(&img);
}

/* ---- JPH (HTJ2K) SIMD micro-kernels: per-tier throughput vs scalar -------- */
static void bench_jph_kernels(void) {
    uint32_t caps = exr_simd_capabilities();
    size_t n = (size_t)8u << 20; /* 8M int64 coefficients */
    int64_t *buf = (int64_t *)malloc(n * sizeof(int64_t));
    int64_t bias = ((int64_t)1 << 31) + 1;
    size_t i, k;
    uint32_t rng = 1u;
    (void)caps;
    if (!buf) return;
    for (i = 0; i < n; ++i) {
        rng = rng * 1664525u + 1013904223u;
        buf[i] = (int64_t)(int32_t)rng;
    }
    printf("\n== JPH SIMD kernels (%.0f M int64) ==\n", n / 1e6);
    {
        struct { const char *nm; void (*f)(int64_t *, size_t, int64_t); int ok; }
        v[] = {
            {"nlt3 scalar", jph_nlt_type3_i64_scalar, 1},
#if defined(EXR_X86)
            {"nlt3 sse2", jph_nlt_type3_i64_sse2, (caps & EXR_SIMD_SSE2) != 0},
            {"nlt3 avx2", jph_nlt_type3_i64_avx2, (caps & EXR_SIMD_AVX2) != 0},
#elif defined(EXR_NEON)
            {"nlt3 neon", jph_nlt_type3_i64_neon, 1},
#endif
        };
        double base = 0;
        for (k = 0; k < sizeof(v) / sizeof(v[0]); ++k) {
            double t0, t;
            long it = 0;
            if (!v[k].ok) { printf("  %-12s (unavailable)\n", v[k].nm); continue; }
            t0 = now_sec();
            do { v[k].f(buf, n, bias); ++it; } while (now_sec() - t0 < 0.3);
            t = (now_sec() - t0) / it;
            if (k == 0) base = t;
            printf("  %-12s %8.1f M/s (%.2fx)\n", v[k].nm, n / t / 1e6,
                   base > 0 ? base / t : 1.0);
        }
    }
    /* int32 -> uint16 pixel pack (all-HALF store). */
    {
        int32_t *src = (int32_t *)buf; /* reuse */
        uint8_t *dst = (uint8_t *)malloc(n * 2);
        struct { const char *nm; void (*f)(uint8_t *, const int32_t *, size_t);
                 int ok; } v[] = {
            {"pack scalar", jph_pack_i32_to_half_scalar, 1},
#if defined(EXR_X86)
            {"pack sse4.1", jph_pack_i32_to_half_sse41, (caps & EXR_SIMD_SSE41) != 0},
            {"pack avx2", jph_pack_i32_to_half_avx2, (caps & EXR_SIMD_AVX2) != 0},
#elif defined(EXR_NEON)
            {"pack neon", jph_pack_i32_to_half_neon, 1},
#endif
        };
        double base = 0;
        if (dst) for (k = 0; k < sizeof(v) / sizeof(v[0]); ++k) {
            double t0, t; long it = 0;
            if (!v[k].ok) { printf("  %-12s (unavailable)\n", v[k].nm); continue; }
            t0 = now_sec();
            do { v[k].f(dst, src, n); ++it; } while (now_sec() - t0 < 0.3);
            t = (now_sec() - t0) / it;
            if (k == 0) base = t;
            printf("  %-12s %8.1f M/s (%.2fx)\n", v[k].nm, n / t / 1e6,
                   base > 0 ? base / t : 1.0);
        }
        free(dst);
    }
    free(buf);
}

int main(int argc, char **argv) {
    printf("TinyEXR v3 benchmark  |  SIMD: %s (caps=0x%x)\n", exr_simd_info(),
           exr_simd_capabilities());

    if (argc > 1) {
        int i;
        for (i = 1; i < argc; ++i) bench_codecs(argv[i]);
    } else {
        bench_codecs("asakusa.exr");
    }
    bench_kernels();
    bench_jph_kernels();
    bench_deflate(argc > 1 ? argv[1] : "asakusa.exr");

    printf("\nNote: 'fpnge PSHUFB-huff' is a literal-only DEFLATE encoder using\n"
           "the fpnge constrained Huffman table with a PSHUFB per-byte code\n"
           "lookup (sse4.1 path). It trades ratio for a simpler, vectorizable\n"
           "emit; the default ZIP codec still uses the fpng LZ77 encoder.\n");
    return 0;
}
