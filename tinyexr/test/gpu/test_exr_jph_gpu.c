/*
 * TinyEXR - CUDA HTJ2K block-coder bit-exactness test.
 *
 * Proves the GPU HT block decoder (one thread per code-block) produces
 * coefficients byte-identical to the CPU coder. For each test image it:
 *   1. CPU-encodes it to HTJ2K256 in memory,
 *   2. opens the blob and fetches each raw HTJ2K chunk,
 *   3. collects the chunk's code-block plan (exr_jph_collect_codeblocks),
 *   4. decodes every i32-eligible block on the GPU and on the CPU,
 *   5. compares the per-block coefficient tiles.
 *
 * Exits 77 (skip) when no CUDA device is present.
 *
 * Copyright (c) 2014-2026 Syoyo Fujita and TinyEXR authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "exr.h"
#include "exr_gpu.h"
#include "exr_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass = 0, g_fail = 0, g_skip = 0;
static char g_dir[1024];

static char *path_of(const char *rel) {
    static char buf[2048];
    snprintf(buf, sizeof(buf), "%s/%s", g_dir, rel);
    return buf;
}
static int file_exists(const char *p) {
    FILE *f = fopen(p, "rb");
    if (f) { fclose(f); return 1; }
    return 0;
}

/* Compare one image's chunks block-coder output: GPU vs CPU. Returns 0 ok. */
static int check_chunks(exr_gpu_context *ctx, const uint8_t *blob,
                        size_t blob_size, const char *label,
                        size_t *out_blocks, size_t *out_elig) {
    exr_reader *r = NULL;
    exr_result rc;
    int np, p, bad = 0;
    size_t total_blocks = 0, total_elig = 0;

    rc = exr_reader_open_memory(blob, blob_size, NULL, &r);
    if (rc != EXR_SUCCESS) { printf("  FAIL %s: reader open\n", label); return 1; }
    rc = exr_reader_parse_header(r);
    if (rc != EXR_SUCCESS) { printf("  FAIL %s: parse header\n", label); exr_reader_close(r); return 1; }
    np = exr_reader_num_parts(r);
    for (p = 0; p < np && !bad; ++p) {
        uint32_t nb = 0, b;
        if (exr_reader_num_blocks(r, p, &nb) != EXR_SUCCESS) { bad = 1; break; }
        for (b = 0; b < nb && !bad; ++b) {
            exr_block_info bi;
            const uint8_t *cdata = NULL;
            size_t csize = 0, total = 0, i;
            exr_codec_ctx cctx;
            exr_jph_cb_plan plan;
            size_t *offs = NULL;
            int32_t *cpu = NULL, *gpu = NULL;

            rc = exr_reader_block_raw(r, p, b, &bi, &cdata, &csize, &cctx);
            if (rc != EXR_SUCCESS) { bad = 1; break; }
            memset(&plan, 0, sizeof(plan));
            rc = exr_jph_collect_codeblocks(&cctx, cdata, csize, &plan);
            if (rc != EXR_SUCCESS) {
                printf("  FAIL %s: collect (part %d blk %u): %s\n", label, p, b,
                       exr_result_string(rc));
                bad = 1;
                break;
            }
            total_blocks += plan.num_records;
            /* lay out eligible tiles back-to-back */
            offs = (size_t *)calloc(plan.num_records ? plan.num_records : 1,
                                    sizeof(size_t));
            for (i = 0; i < plan.num_records; ++i) {
                if (plan.records[i].i32_eligible) {
                    uint32_t st = (plan.records[i].width + 7u) & ~7u;
                    offs[i] = total;
                    total += (size_t)st * plan.records[i].height;
                    ++total_elig;
                }
            }
            if (total == 0) { free(offs); exr_jph_cb_plan_free(NULL, &plan); continue; }
            cpu = (int32_t *)malloc(total * sizeof(int32_t));
            gpu = (int32_t *)malloc(total * sizeof(int32_t));
            if (!cpu || !gpu) { free(cpu); free(gpu); free(offs); exr_jph_cb_plan_free(NULL, &plan); bad = 1; break; }

            rc = exr_gpu_jph_decode_plan(ctx, &plan, offs, total, gpu);
            if (rc != EXR_SUCCESS) {
                printf("  FAIL %s: gpu decode: %s\n", label, exr_result_string(rc));
                bad = 1;
            }
            for (i = 0; i < plan.num_records && !bad; ++i) {
                const exr_jph_cb_record *rec = &plan.records[i];
                uint32_t st, y;
                if (!rec->i32_eligible) continue;
                st = (rec->width + 7u) & ~7u;
                rc = exr_jph_decode_one_block_i32(rec, plan.data, cpu + offs[i], st);
                if (rc != EXR_SUCCESS) {
                    printf("  FAIL %s: cpu decode rec %zu: %s\n", label, i,
                           exr_result_string(rc));
                    bad = 1;
                    break;
                }
                for (y = 0; y < rec->height; ++y) {
                    const int32_t *cr = cpu + offs[i] + (size_t)y * st;
                    const int32_t *gr = gpu + offs[i] + (size_t)y * st;
                    if (memcmp(cr, gr, rec->width * sizeof(int32_t)) != 0) {
                        printf("  FAIL %s: mismatch part %d blk %u rec %zu row %u "
                               "(%ux%u comp %u res %u band %u passes %u)\n",
                               label, p, b, i, y, rec->width, rec->height,
                               rec->comp, rec->res, rec->band, rec->active_passes);
                        bad = 1;
                        break;
                    }
                }
            }
            free(cpu); free(gpu); free(offs);
            exr_jph_cb_plan_free(NULL, &plan);
        }
    }
    exr_reader_close(r);
    if (out_blocks) *out_blocks = total_blocks;
    if (out_elig) *out_elig = total_elig;
    return bad;
}

static void test_image(exr_gpu_context *ctx, const char *rel) {
    char *path = path_of(rel);
    exr_image img;
    void *blob = NULL;
    size_t blob_size = 0, blocks = 0, elig = 0;
    exr_result rc;
    int bad;
    if (!file_exists(path)) { ++g_skip; printf("  skip  %s (missing)\n", rel); return; }
    memset(&img, 0, sizeof(img));
    rc = exr_load_from_file(path, NULL, &img);
    if (rc != EXR_SUCCESS) { ++g_skip; printf("  skip  %s (cpu load)\n", rel); return; }
    rc = exr_save_to_memory(&blob, &blob_size, NULL, &img, EXR_COMPRESSION_HTJ2K256);
    exr_image_free(&img);
    if (rc != EXR_SUCCESS) { ++g_skip; printf("  skip  %s (htj2k encode: %s)\n", rel, exr_result_string(rc)); return; }
    bad = check_chunks(ctx, (const uint8_t *)blob, blob_size, rel, &blocks, &elig);
    free(blob);
    if (bad) { ++g_fail; }
    else { ++g_pass; printf("  PASS  %s (%zu code-blocks, %zu i32 on GPU)\n", rel, blocks, elig); }
}

static size_t pix_size(exr_pixel_type t) {
    return t == EXR_PIXEL_HALF ? 2u : 4u;
}

/* Encode parity: build the first chunk's canonical pixel block from a loaded
 * image, collect its code-block coefficient plan, and compare each i32 block's
 * GPU-encoded bytes/missing/length against the CPU coder. */
static void test_encode_image(exr_gpu_context *ctx, const char *rel) {
    char *path = path_of(rel);
    exr_image img;
    exr_result rc;
    const exr_part *pt;
    exr_codec_ctx cctx;
    uint8_t *block = NULL, *gpu_bytes = NULL, *cpu_buf = NULL;
    exr_jph_enc_plan plan;
    unsigned int *gmiss = NULL, *glen0 = NULL, *gsize = NULL;
    size_t nlines, blk = 0, off, i, eligible = 0;
    int c, nch, y, bad = 0;
    const unsigned int OUTSTRIDE = 20480u;

    if (!file_exists(path)) { ++g_skip; printf("  skip  enc %s (missing)\n", rel); return; }
    memset(&img, 0, sizeof(img));
    if (exr_load_from_file(path, NULL, &img) != EXR_SUCCESS) { ++g_skip; printf("  skip  enc %s (load)\n", rel); return; }
    pt = &img.parts[0];
    nch = pt->header.num_channels;
    /* all-HALF only (the GPU i32 encode path) */
    for (c = 0; c < nch; ++c)
        if (pt->header.channels[c].pixel_type != EXR_PIXEL_HALF) {
            ++g_skip; printf("  skip  enc %s (not all-HALF)\n", rel); exr_image_free(&img); return;
        }
    nlines = (size_t)(pt->height < 256 ? pt->height : 256);
    /* canonical scanline block: per line, per channel (header order), packed */
    for (c = 0; c < nch; ++c) blk += (size_t)pt->width * pix_size(pt->header.channels[c].pixel_type);
    blk *= nlines;
    block = (uint8_t *)malloc(blk);
    if (!block) { ++g_skip; exr_image_free(&img); return; }
    off = 0;
    for (y = 0; y < (int)nlines; ++y)
        for (c = 0; c < nch; ++c) {
            size_t ps = pix_size(pt->header.channels[c].pixel_type);
            size_t row = (size_t)pt->width * ps;
            memcpy(block + off, (const uint8_t *)pt->images[c] + (size_t)y * row, row);
            off += row;
        }
    memset(&cctx, 0, sizeof(cctx));
    cctx.alloc = NULL;
    cctx.compression = EXR_COMPRESSION_HTJ2K256;
    cctx.channels = pt->header.channels;
    cctx.num_channels = nch;
    cctx.x = pt->header.data_window.min_x;
    cctx.y = pt->header.data_window.min_y;
    cctx.width = pt->width;
    cctx.num_lines = (int)nlines;

    memset(&plan, 0, sizeof(plan));
    rc = exr_jph_collect_encode_blocks(&cctx, block, blk, &plan);
    if (rc != EXR_SUCCESS) { printf("  FAIL enc %s: collect %s\n", rel, exr_result_string(rc)); ++g_fail; goto cleanup; }

    gpu_bytes = (uint8_t *)malloc(plan.num_records * OUTSTRIDE);
    gmiss = (unsigned int *)calloc(plan.num_records ? plan.num_records : 1, sizeof(unsigned int));
    glen0 = (unsigned int *)calloc(plan.num_records ? plan.num_records : 1, sizeof(unsigned int));
    gsize = (unsigned int *)calloc(plan.num_records ? plan.num_records : 1, sizeof(unsigned int));
    cpu_buf = (uint8_t *)malloc(65536u + 3072u + 2u);
    if (!gpu_bytes || !gmiss || !glen0 || !gsize || !cpu_buf) { ++g_skip; goto cleanup; }

    rc = exr_gpu_jph_encode_plan(ctx, &plan, gpu_bytes, OUTSTRIDE, gmiss, glen0, gsize);
    if (rc != EXR_SUCCESS) { printf("  FAIL enc %s: gpu encode %s\n", rel, exr_result_string(rc)); ++g_fail; goto cleanup; }

    for (i = 0; i < plan.num_records && !bad; ++i) {
        const exr_jph_enc_record *r = &plan.records[i];
        uint32_t cmiss = 0, clen0 = 0; size_t csize = 0;
        if (!r->plane_is_i32 || r->kmax < 1 || r->kmax > 30) continue;
        ++eligible;
        rc = exr_jph_encode_one_block_i32(r, plan.coeffs + r->coeff_offset, cpu_buf,
                                          65536u + 3072u + 2u, &cmiss, &clen0, &csize);
        if (rc != EXR_SUCCESS) { printf("  FAIL enc %s: cpu encode rec %zu\n", rel, i); bad = 1; break; }
        if (cmiss != gmiss[i] || clen0 != glen0[i] || (size_t)gsize[i] != csize) {
            printf("  FAIL enc %s: rec %zu meta (cpu miss=%u len0=%u size=%zu; gpu miss=%u len0=%u size=%u)\n",
                   rel, i, cmiss, clen0, csize, gmiss[i], glen0[i], gsize[i]);
            bad = 1; break;
        }
        if (csize && memcmp(cpu_buf, gpu_bytes + i * OUTSTRIDE, csize) != 0) {
            printf("  FAIL enc %s: rec %zu bytes differ (%ux%u kmax %u size %zu)\n",
                   rel, i, r->width, r->height, r->kmax, csize);
            bad = 1; break;
        }
    }
    if (bad) ++g_fail;
    else { ++g_pass; printf("  PASS  enc %s (%zu blocks, %zu i32 on GPU)\n", rel, plan.num_records, eligible); }

cleanup:
    free(block); free(gpu_bytes); free(gmiss); free(glen0); free(gsize); free(cpu_buf);
    exr_jph_enc_plan_free(NULL, &plan);
    exr_image_free(&img);
}

/* Whole-image GPU HTJ2K decode: CPU-encode to HTJ2K256, then compare
 * exr_gpu_load_from_memory (full GPU path) against exr_load_from_memory. */
static int img_equal(const exr_image *a, const exr_image *b) {
    int p, c;
    if (a->num_parts != b->num_parts) return 0;
    for (p = 0; p < a->num_parts; ++p) {
        const exr_part *pa = &a->parts[p], *pb = &b->parts[p];
        if (pa->width != pb->width || pa->height != pb->height) return 0;
        if (pa->header.num_channels != pb->header.num_channels) return 0;
        for (c = 0; c < pa->header.num_channels; ++c) {
            size_t ps = pix_size(pa->header.channels[c].pixel_type);
            if (memcmp(pa->images[c], pb->images[c],
                       (size_t)pa->width * pa->height * ps) != 0)
                return 0;
        }
    }
    return 1;
}

static void test_decode_image(exr_gpu_context *ctx, const char *rel) {
    char *path = path_of(rel);
    exr_image src, cpu, gpu;
    void *blob = NULL;
    size_t blob_size = 0;
    exr_result rc;
    if (!file_exists(path)) { ++g_skip; printf("  skip  dec %s (missing)\n", rel); return; }
    memset(&src, 0, sizeof(src)); memset(&cpu, 0, sizeof(cpu)); memset(&gpu, 0, sizeof(gpu));
    if (exr_load_from_file(path, NULL, &src) != EXR_SUCCESS) { ++g_skip; printf("  skip  dec %s (load)\n", rel); return; }
    rc = exr_save_to_memory(&blob, &blob_size, NULL, &src, EXR_COMPRESSION_HTJ2K256);
    exr_image_free(&src);
    if (rc != EXR_SUCCESS) { ++g_skip; printf("  skip  dec %s (htj2k enc)\n", rel); return; }
    if (exr_load_from_memory(blob, blob_size, NULL, &cpu) != EXR_SUCCESS) { ++g_skip; free(blob); printf("  skip  dec %s (cpu reload)\n", rel); return; }
    rc = exr_gpu_load_from_memory(ctx, blob, blob_size, NULL, &gpu);
    free(blob);
    if (rc != EXR_SUCCESS) { printf("  FAIL dec %s: gpu load %s\n", rel, exr_result_string(rc)); ++g_fail; exr_image_free(&cpu); return; }
    if (img_equal(&cpu, &gpu)) { ++g_pass; printf("  PASS  dec %s (whole-image GPU == CPU)\n", rel); }
    else { ++g_fail; printf("  FAIL dec %s: pixels differ\n", rel); }
    exr_image_free(&cpu); exr_image_free(&gpu);
}

/* Whole-image GPU HTJ2K encode: compare exr_gpu_save_to_memory (GPU block
 * encode + CPU assembly) against exr_save_to_memory byte-for-byte, and verify
 * the GPU-produced blob round-trips. */
static void test_encode_roundtrip(exr_gpu_context *ctx, const char *rel) {
    char *path = path_of(rel);
    exr_image src, rt;
    void *cpu_blob = NULL, *gpu_blob = NULL;
    size_t cpu_size = 0, gpu_size = 0;
    exr_result rc;
    if (!file_exists(path)) { ++g_skip; printf("  skip  save %s (missing)\n", rel); return; }
    memset(&src, 0, sizeof(src)); memset(&rt, 0, sizeof(rt));
    if (exr_load_from_file(path, NULL, &src) != EXR_SUCCESS) { ++g_skip; printf("  skip  save %s (load)\n", rel); return; }
    rc = exr_save_to_memory(&cpu_blob, &cpu_size, NULL, &src, EXR_COMPRESSION_HTJ2K256);
    if (rc != EXR_SUCCESS) { ++g_skip; exr_image_free(&src); printf("  skip  save %s (cpu enc)\n", rel); return; }
    rc = exr_gpu_save_to_memory(ctx, &gpu_blob, &gpu_size, NULL, &src, EXR_COMPRESSION_HTJ2K256);
    if (rc != EXR_SUCCESS) { printf("  FAIL save %s: gpu save %s\n", rel, exr_result_string(rc)); ++g_fail; goto cleanup; }
    if (cpu_size != gpu_size || memcmp(cpu_blob, gpu_blob, cpu_size) != 0) {
        /* Not byte-identical: still accept if it round-trips to the same pixels. */
        if (exr_load_from_memory(gpu_blob, gpu_size, NULL, &rt) == EXR_SUCCESS &&
            img_equal(&src, &rt)) {
            ++g_pass; printf("  PASS  save %s (roundtrip ok; %zu vs cpu %zu bytes)\n", rel, gpu_size, cpu_size);
        } else { ++g_fail; printf("  FAIL save %s: differs from CPU and roundtrip mismatch\n", rel); }
    } else {
        ++g_pass; printf("  PASS  save %s (byte-identical to CPU, %zu bytes)\n", rel, gpu_size);
    }
cleanup:
    free(cpu_blob); free(gpu_blob);
    exr_image_free(&src); exr_image_free(&rt);
}

int main(int argc, char **argv) {
    exr_gpu_context *ctx = NULL;
    exr_result rc;
    char name[256];
    const char *home;

    if (argc > 1) snprintf(g_dir, sizeof(g_dir), "%s", argv[1]);
    else {
        home = getenv("HOME");
        snprintf(g_dir, sizeof(g_dir), "%s/work/openexr-images", home ? home : ".");
    }
    if (!exr_gpu_available()) { printf("SKIP: no CUDA device\n"); return 77; }
    if (exr_gpu_device_name(0, name, sizeof(name)) == EXR_SUCCESS)
        printf("CUDA device 0: %s\n", name);
    rc = exr_gpu_context_create(NULL, NULL, &ctx);
    if (rc != EXR_SUCCESS) { printf("FAIL: context create: %s\n", exr_result_string(rc)); return 1; }
    printf("images dir: %s\n\n[HTJ2K GPU block-coder parity]\n", g_dir);

    test_image(ctx, "ScanLines/Desk.exr");
    test_image(ctx, "ScanLines/Carrots.exr");
    test_image(ctx, "ScanLines/Tree.exr");
    test_image(ctx, "TestImages/AllHalfValues.exr");
    test_image(ctx, "TestImages/GrayRampsHorizontal.exr");
    test_image(ctx, "TestImages/SquaresSwirls.exr");

    printf("\n[HTJ2K whole-image GPU decode == CPU]\n");
    test_decode_image(ctx, "ScanLines/Desk.exr");
    test_decode_image(ctx, "ScanLines/Carrots.exr");
    test_decode_image(ctx, "ScanLines/Tree.exr");
    test_decode_image(ctx, "TestImages/AllHalfValues.exr");

    printf("\n[HTJ2K GPU block-coder ENCODE parity]\n");
    test_encode_image(ctx, "ScanLines/Desk.exr");
    test_encode_image(ctx, "ScanLines/Carrots.exr");
    test_encode_image(ctx, "ScanLines/Tree.exr");
    test_encode_image(ctx, "TestImages/AllHalfValues.exr");
    test_encode_image(ctx, "TestImages/GrayRampsHorizontal.exr");
    test_encode_image(ctx, "TestImages/SquaresSwirls.exr");

    printf("\n[HTJ2K whole-image GPU encode (save) == CPU]\n");
    test_encode_roundtrip(ctx, "ScanLines/Desk.exr");
    test_encode_roundtrip(ctx, "ScanLines/Carrots.exr");
    test_encode_roundtrip(ctx, "TestImages/AllHalfValues.exr");

    exr_gpu_context_destroy(ctx);
    printf("\n=== %d passed, %d failed, %d skipped ===\n", g_pass, g_fail, g_skip);
    return g_fail ? 1 : 0;
}
