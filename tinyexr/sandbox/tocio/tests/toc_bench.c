/*
 * tocio - interpreter throughput benchmark (scalar vs SIMD per op).
 *
 * Times each op kind through toc_apply over a 1M-pixel RGBA buffer at the scalar
 * tier (toc_simd_force 0) and the best SIMD tier (level 1 = SSE2/NEON), and
 * reports megapixels/second and the speedup. The per-iteration memcpy that
 * restores the input is measured separately and subtracted so the rate reflects
 * the op alone.
 *
 *   make tocio-bench
 *
 * Copyright (c) 2014-2026 Syoyo Fujita and TinyEXR authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "tocio.h"
#include "toc_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static toc_op_list *mklist(void) {
    const toc_allocator *a = toc_default_allocator();
    toc_op_list *l = (toc_op_list *)malloc(sizeof(*l));
    memset(l, 0, sizeof(*l));
    l->alloc = *a;
    return l;
}

/* mean seconds per toc_apply over >=0.3s, with the restoring memcpy subtracted. */
static double time_apply(const toc_op_list *l, float *buf, const float *src,
                         size_t np, double memcpy_s) {
    double t0 = now_sec(), t;
    long it = 0;
    do {
        memcpy(buf, src, np * 4 * sizeof(float));
        toc_apply(l, buf, np, 4);
        ++it;
    } while (now_sec() - t0 < 0.3);
    t = (now_sec() - t0) / (double)it;
    t -= memcpy_s;
    return t > 1e-12 ? t : 1e-12;
}

int main(void) {
    const size_t NP = 1u << 20; /* 1M pixels */
    float *buf = (float *)malloc(NP * 4 * sizeof(float));
    float *src = (float *)malloc(NP * 4 * sizeof(float));
    double mc;
    size_t i;
    int oi;
    struct { const char *name; toc_op_kind kind; } ops[] = {
        {"matrix", TOC_OP_MATRIX},   {"range", TOC_OP_RANGE},
        {"exponent", TOC_OP_EXPONENT}, {"log", TOC_OP_LOG},
        {"log_camera", TOC_OP_LOG_CAMERA}, {"exp_linear", TOC_OP_EXP_LINEAR},
        {"cdl", TOC_OP_CDL},
    };
    if (!buf || !src) { free(buf); free(src); return 1; }
    for (i = 0; i < NP * 4; ++i)
        src[i] = (float)((i * 37u) % 1000u) / 1000.0f * 1.2f; /* [0, 1.2) */

    /* memcpy-only baseline (subtracted from each timing). */
    {
        double t0 = now_sec();
        long it = 0;
        do { memcpy(buf, src, NP * 4 * sizeof(float)); ++it; } while (now_sec() - t0 < 0.2);
        mc = (now_sec() - t0) / (double)it;
    }

    printf("tocio interpreter bench  |  SIMD: %s  |  %zu px RGBA\n",
#if defined(TOC_NEON)
           "neon",
#elif defined(TOC_X86)
           "sse2/avx2",
#else
           "scalar-only",
#endif
           NP);
    printf("  %-12s %12s %12s %9s\n", "op", "scalar MP/s", "simd MP/s", "speedup");

    for (oi = 0; oi < (int)(sizeof(ops) / sizeof(ops[0])); ++oi) {
        toc_op_list *l = mklist();
        toc_op *o = toc_op_list_push(l, ops[oi].kind);
        int c;
        double ts, tv;
        /* fill realistic params per op */
        switch (ops[oi].kind) {
            case TOC_OP_MATRIX:
                for (c = 0; c < 16; ++c) o->u.matrix.m[c] = (c % 5 == 0) ? 0.9f : 0.05f;
                break;
            case TOC_OP_RANGE:
                for (c = 0; c < 4; ++c) {
                    o->u.range.scale[c] = 1.1f; o->u.range.offset[c] = -0.05f;
                    o->u.range.min[c] = 0.0f; o->u.range.max[c] = 1.0f;
                }
                o->u.range.clamp_lo = o->u.range.clamp_hi = 1;
                break;
            case TOC_OP_EXPONENT:
                for (c = 0; c < 4; ++c) o->u.exponent.e[c] = (c < 3) ? 2.2f : 1.0f;
                break;
            case TOC_OP_LOG:
                o->u.log.base = 10.0f;
                for (c = 0; c < 3; ++c) {
                    o->u.log.lin_slope[c] = 0.9f; o->u.log.lin_offset[c] = 0.1f;
                    o->u.log.log_slope[c] = 0.3f; o->u.log.log_offset[c] = 0.6f;
                }
                break;
            case TOC_OP_LOG_CAMERA:
                o->u.logcam.base = 10.0f;
                for (c = 0; c < 3; ++c) {
                    o->u.logcam.lin_slope[c] = 0.9f; o->u.logcam.lin_offset[c] = 0.1f;
                    o->u.logcam.log_slope[c] = 0.3f; o->u.logcam.log_offset[c] = 0.6f;
                    o->u.logcam.lin_break[c] = 0.15f;
                    o->u.logcam.linear_slope[c] = 1.1f; o->u.logcam.linear_offset[c] = 0.0f;
                }
                break;
            case TOC_OP_EXP_LINEAR:
                for (c = 0; c < 4; ++c) {
                    o->u.exp_linear.scale[c] = 0.95f; o->u.exp_linear.offset[c] = 0.05f;
                    o->u.exp_linear.gamma[c] = 2.4f; o->u.exp_linear.breakpoint[c] = 0.04f;
                    o->u.exp_linear.slope[c] = 0.077f;
                }
                break;
            case TOC_OP_CDL:
                o->u.cdl.saturation = 1.2f; o->u.cdl.clamp = 1;
                for (c = 0; c < 3; ++c) {
                    o->u.cdl.slope[c] = 1.1f; o->u.cdl.offset[c] = 0.02f;
                    o->u.cdl.power[c] = 0.9f; o->u.cdl.luma[c] = 0.0f;
                }
                break;
            default: break;
        }
        toc_simd_force(0);
        ts = time_apply(l, buf, src, NP, mc);
        toc_simd_force(1);
        tv = time_apply(l, buf, src, NP, mc);
        printf("  %-12s %12.1f %12.1f %8.2fx\n", ops[oi].name,
               (double)NP / ts / 1e6, (double)NP / tv / 1e6, ts / tv);
        toc_op_list_free(l);
    }

    /* JIT: native code for matrix + exponent + range, with pow inlined. Times
     * the compiled function directly (vs the NEON-batch interpreter on the same
     * pipeline). The exponent's pow/log/exp are emitted inline -- no per-pixel
     * helper call -- so the whole pixel stays in registers across the loop. */
    {
        toc_op_list *l = mklist();
        toc_op *m = toc_op_list_push(l, TOC_OP_MATRIX);
        toc_op *e = toc_op_list_push(l, TOC_OP_EXPONENT);
        toc_op *r = toc_op_list_push(l, TOC_OP_RANGE);
        toc_jit *j = NULL;
        toc_result rc;
        int c;
        for (c = 0; c < 16; ++c) m->u.matrix.m[c] = (c % 5 == 0) ? 0.9f : 0.05f;
        for (c = 0; c < 4; ++c) e->u.exponent.e[c] = (c < 3) ? 2.2f : 1.0f;
        for (c = 0; c < 4; ++c) {
            r->u.range.scale[c] = 1.0f; r->u.range.min[c] = 0.0f;
            r->u.range.max[c] = 1.0f;
        }
        r->u.range.clamp_lo = r->u.range.clamp_hi = 1;
        rc = toc_jit_compile(l, 4, NULL, &j);
        printf("\n  pipeline: matrix + exponent + range\n");
        toc_simd_force(1);
        {
            double tv = time_apply(l, buf, src, NP, mc);
            printf("  %-18s %12.1f MP/s\n", "interp (NEON)",
                   (double)NP / tv / 1e6);
        }
        if (rc == TOC_SUCCESS && j) {
            toc_jit_fn fn = toc_jit_func(j);
            double t0 = now_sec(), t;
            long it = 0;
            do {
                memcpy(buf, src, NP * 4 * sizeof(float));
                fn(buf, NP);
                ++it;
            } while (now_sec() - t0 < 0.3);
            t = (now_sec() - t0) / (double)it - mc;
            if (t < 1e-12) t = 1e-12;
            printf("  %-18s %12.1f MP/s  (pow inlined)\n", "JIT (native)",
                   (double)NP / t / 1e6);
            toc_jit_destroy(j);
        } else {
            printf("  %-18s   (unsupported)\n", "JIT (native)");
        }
        toc_op_list_free(l);
    }

    /* JIT pipeline with an inline log (camera-log colorspaces). */
    {
        toc_op_list *l = mklist();
        toc_op *o = toc_op_list_push(l, TOC_OP_LOG);
        toc_jit *j = NULL;
        toc_result rc;
        int c;
        o->u.log.base = 10.0f;
        for (c = 0; c < 3; ++c) {
            o->u.log.lin_slope[c] = 0.9f; o->u.log.lin_offset[c] = 0.1f;
            o->u.log.log_slope[c] = 0.3f; o->u.log.log_offset[c] = 0.6f;
        }
        rc = toc_jit_compile(l, 4, NULL, &j);
        printf("\n  pipeline: log\n");
        toc_simd_force(1);
        printf("  %-18s %12.1f MP/s\n", "interp (NEON)",
               (double)NP / time_apply(l, buf, src, NP, mc) / 1e6);
        if (rc == TOC_SUCCESS && j) {
            toc_jit_fn fn = toc_jit_func(j);
            double t0 = now_sec(), t;
            long it = 0;
            do {
                memcpy(buf, src, NP * 4 * sizeof(float));
                fn(buf, NP);
                ++it;
            } while (now_sec() - t0 < 0.3);
            t = (now_sec() - t0) / (double)it - mc;
            if (t < 1e-12) t = 1e-12;
            printf("  %-18s %12.1f MP/s  (log inlined)\n", "JIT (native)",
                   (double)NP / t / 1e6);
            toc_jit_destroy(j);
        }
        toc_op_list_free(l);
    }

    /* JIT pipelines with inline exp_linear and cdl. */
    {
        int which;
        for (which = 0; which < 2; ++which) {
            toc_op_list *l = mklist();
            toc_jit *j = NULL;
            int c;
            const char *nm;
            if (which == 0) {
                toc_op *o = toc_op_list_push(l, TOC_OP_EXP_LINEAR);
                nm = "exp_linear";
                for (c = 0; c < 4; ++c) {
                    o->u.exp_linear.scale[c] = 0.95f; o->u.exp_linear.offset[c] = 0.05f;
                    o->u.exp_linear.gamma[c] = 2.4f; o->u.exp_linear.breakpoint[c] = 0.04f;
                    o->u.exp_linear.slope[c] = 0.077f;
                }
            } else {
                toc_op *o = toc_op_list_push(l, TOC_OP_CDL);
                nm = "cdl";
                o->u.cdl.saturation = 1.2f; o->u.cdl.clamp = 1;
                for (c = 0; c < 3; ++c) {
                    o->u.cdl.slope[c] = 1.1f; o->u.cdl.offset[c] = 0.02f;
                    o->u.cdl.power[c] = 0.9f; o->u.cdl.luma[c] = 0.0f;
                }
            }
            printf("\n  pipeline: %s\n", nm);
            toc_simd_force(1);
            printf("  %-18s %12.1f MP/s\n", "interp (NEON)",
                   (double)NP / time_apply(l, buf, src, NP, mc) / 1e6);
            if (TOC_SUCCESS == toc_jit_compile(l, 4, NULL, &j) && j) {
                toc_jit_fn fn = toc_jit_func(j);
                double t0 = now_sec(), t;
                long it = 0;
                do {
                    memcpy(buf, src, NP * 4 * sizeof(float));
                    fn(buf, NP);
                    ++it;
                } while (now_sec() - t0 < 0.3);
                t = (now_sec() - t0) / (double)it - mc;
                if (t < 1e-12) t = 1e-12;
                printf("  %-18s %12.1f MP/s  (inlined)\n", "JIT (native)",
                       (double)NP / t / 1e6);
                toc_jit_destroy(j);
            }
            toc_op_list_free(l);
        }
    }
    free(buf);
    free(src);
    return 0;
}
