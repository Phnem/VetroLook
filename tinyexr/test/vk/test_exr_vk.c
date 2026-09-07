/*
 * TinyEXR - Vulkan compute backend test.
 *
 * Validates the GPU decode/encode/processing/resize paths against the CPU
 * reference on real openexr-images. Exits 77 (skip) when no Vulkan device is
 * present so it is a no-op on machines / CI without a GPU.
 *
 * Usage: test_exr_vk [openexr-images-dir]   (default: ~/work/openexr-images)
 *
 * Copyright (c) 2014-2026 Syoyo Fujita and TinyEXR authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "exr.h"
#include "exr_vk.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass = 0, g_fail = 0, g_skip = 0;
static char g_dir[1024];

static void PASS(const char *what) { g_pass++; printf("  PASS  %s\n", what); }
static void FAIL(const char *what, const char *why) {
    g_fail++; printf("  FAIL  %s  (%s)\n", what, why ? why : "");
}
static void SKIP(const char *what, const char *why) {
    g_skip++; printf("  skip  %s  (%s)\n", what, why ? why : "");
}

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

/* byte-exact compare of two loaded images (per channel, native pixel size). */
static int images_equal(const exr_image *a, const exr_image *b) {
    int p, c;
    if (a->num_parts != b->num_parts) return 0;
    for (p = 0; p < a->num_parts; ++p) {
        const exr_part *pa = &a->parts[p], *pb = &b->parts[p];
        if (pa->width != pb->width || pa->height != pb->height) return 0;
        if (pa->header.num_channels != pb->header.num_channels) return 0;
        for (c = 0; c < pa->header.num_channels; ++c) {
            size_t ps = pa->header.channels[c].pixel_type == EXR_PIXEL_HALF ? 2
                       : pa->header.channels[c].pixel_type == EXR_PIXEL_UINT ? 4 : 4;
            if (memcmp(pa->images[c], pb->images[c],
                       (size_t)pa->width * pa->height * ps) != 0)
                return 0;
        }
    }
    return 1;
}

static int part_is_scanline_nosub(const exr_header *h) {
    int c;
    if (h->part_type != EXR_PART_SCANLINE) return 0;
    for (c = 0; c < h->num_channels; ++c)
        if (h->channels[c].x_sampling != 1 || h->channels[c].y_sampling != 1)
            return 0;
    return 1;
}

/* float-buffer tolerance compare: returns max abs diff. */
static double max_abs_diff(const float *a, const float *b, size_t n) {
    size_t i;
    double m = 0.0;
    for (i = 0; i < n; ++i) {
        double d = fabs((double)a[i] - (double)b[i]);
        if (d > m) m = d;
    }
    return m;
}

/* ---- decode parity ------------------------------------------------------- */
static void test_decode(exr_vk_context *ctx, const char *rel) {
    char *p = path_of(rel);
    exr_image cpu, gpu;
    exr_result rc;
    if (!file_exists(p)) { SKIP(rel, "missing"); return; }
    memset(&cpu, 0, sizeof(cpu));
    memset(&gpu, 0, sizeof(gpu));
    rc = exr_load_from_file(p, NULL, &cpu);
    if (!EXR_OK(rc)) { SKIP(rel, "cpu load failed"); return; }
    rc = exr_vk_load_from_file(ctx, p, NULL, &gpu);
    if (!EXR_OK(rc)) { FAIL(rel, exr_result_string(rc)); exr_image_free(&cpu); return; }
    if (images_equal(&cpu, &gpu)) {
        char w[256];
        snprintf(w, sizeof(w), "decode %s (%dx%d, %d ch%s)", rel,
                 cpu.parts[0].width, cpu.parts[0].height,
                 cpu.parts[0].header.num_channels,
                 part_is_scanline_nosub(&cpu.parts[0].header) ? ", gpu" : ", fallback");
        PASS(w);
    } else {
        FAIL(rel, "pixels differ from CPU decode");
    }
    exr_image_free(&cpu);
    exr_image_free(&gpu);
}

/* ---- encode roundtrip (GPU save -> CPU load == original) ----------------- */
static void test_encode(exr_vk_context *ctx, const char *rel) {
    char *p = path_of(rel);
    exr_image orig, rt;
    void *blob = NULL;
    size_t blob_size = 0;
    exr_result rc;
    char w[256];
    if (!file_exists(p)) { SKIP(rel, "missing"); return; }
    memset(&orig, 0, sizeof(orig));
    memset(&rt, 0, sizeof(rt));
    rc = exr_load_from_file(p, NULL, &orig);
    if (!EXR_OK(rc)) { SKIP(rel, "cpu load failed"); return; }
    if (!part_is_scanline_nosub(&orig.parts[0].header)) {
        SKIP(rel, "not scanline/non-subsampled");
        exr_image_free(&orig);
        return;
    }
    /* GPU-encode as ZIP (lossless) then decode on CPU and compare. */
    rc = exr_vk_save_to_memory(ctx, &blob, &blob_size, NULL, &orig,
                                EXR_COMPRESSION_ZIP);
    if (!EXR_OK(rc)) { FAIL(rel, exr_result_string(rc)); exr_image_free(&orig); return; }
    rc = exr_load_from_memory(blob, blob_size, NULL, &rt);
    free(blob);
    if (!EXR_OK(rc)) { FAIL(rel, "reload failed"); exr_image_free(&orig); return; }
    snprintf(w, sizeof(w), "encode roundtrip %s", rel);
    if (images_equal(&orig, &rt)) PASS(w);
    else FAIL(rel, "roundtrip pixels differ");
    exr_image_free(&orig);
    exr_image_free(&rt);
}

/* ---- resize: GPU vs CPU -------------------------------------------------- */
static void test_resize(exr_vk_context *ctx, const char *rel) {
    char *p = path_of(rel);
    exr_image img;
    float *rgba = NULL, *cpu_out = NULL, *gpu_out = NULL;
    int w, h, ch, dw, dh;
    exr_result rc;
    static const exr_resize_filter filters[4] = {
        EXR_RESIZE_BOX, EXR_RESIZE_TRIANGLE, EXR_RESIZE_CATMULL_ROM,
        EXR_RESIZE_MITCHELL};
    static const char *fnames[4] = {"box", "triangle", "catmull-rom", "mitchell"};
    int fi;
    if (!file_exists(p)) { SKIP("resize", "missing"); return; }
    memset(&img, 0, sizeof(img));
    rc = exr_load_from_file(p, NULL, &img);
    if (!EXR_OK(rc)) { SKIP("resize", "cpu load failed"); return; }
    rc = exr_part_to_rgba_float(NULL, &img.parts[0], &rgba, &w, &h, &ch);
    exr_image_free(&img);
    if (!EXR_OK(rc)) { SKIP("resize", "to_rgba failed"); return; }
    dw = w / 2 > 0 ? w / 2 : 1;
    dh = h / 2 > 0 ? h / 2 : 1;
    cpu_out = (float *)malloc((size_t)dw * dh * ch * sizeof(float));
    gpu_out = (float *)malloc((size_t)dw * dh * ch * sizeof(float));
    if (!cpu_out || !gpu_out) { SKIP("resize", "oom"); goto done; }
    for (fi = 0; fi < 4; ++fi) {
        char wname[128];
        double md;
        rc = exr_resize_float(NULL, rgba, w, h, 0, cpu_out, dw, dh, 0, ch,
                              filters[fi], EXR_EDGE_CLAMP, -1);
        if (!EXR_OK(rc)) { FAIL("resize cpu", fnames[fi]); continue; }
        rc = exr_vk_resize_float(ctx, rgba, w, h, 0, gpu_out, dw, dh, 0, ch,
                                  filters[fi], EXR_EDGE_CLAMP, -1);
        if (!EXR_OK(rc)) { FAIL("resize gpu", exr_result_string(rc)); continue; }
        md = max_abs_diff(cpu_out, gpu_out, (size_t)dw * dh * ch);
        snprintf(wname, sizeof(wname), "resize %s %dx%d->%dx%d (maxdiff=%.3g)",
                 fnames[fi], w, h, dw, dh, md);
        if (md < 1e-3) PASS(wname);
        else FAIL(wname, "exceeds tolerance");
    }
done:
    free(rgba); free(cpu_out); free(gpu_out);
}

/* ---- extract channel: GPU convert HALF->FLOAT vs exr_half_to_float ------- */
static void test_extract_channel(exr_vk_context *ctx, const char *rel) {
    char *p = path_of(rel);
    exr_image img;
    const exr_part *pt;
    int c = -1, i;
    size_t n;
    float *ref = NULL, *got = NULL;
    exr_result rc;
    if (!file_exists(p)) { SKIP("extract-channel", "missing"); return; }
    memset(&img, 0, sizeof(img));
    rc = exr_load_from_file(p, NULL, &img);
    if (!EXR_OK(rc)) { SKIP("extract-channel", "cpu load failed"); return; }
    pt = &img.parts[0];
    for (i = 0; i < pt->header.num_channels; ++i)
        if (pt->header.channels[i].pixel_type == EXR_PIXEL_HALF) { c = i; break; }
    if (c < 0) { SKIP("extract-channel", "no HALF channel"); exr_image_free(&img); return; }
    n = (size_t)pt->width * pt->height;
    ref = (float *)malloc(n * sizeof(float));
    got = (float *)malloc(n * sizeof(float));
    if (!ref || !got) { SKIP("extract-channel", "oom"); goto done; }
    exr_half_to_float((const uint16_t *)pt->images[c], ref, n);
    rc = exr_vk_convert_pixels(ctx, got, EXR_PIXEL_FLOAT, pt->images[c],
                                EXR_PIXEL_HALF, n, EXR_CONVERT_RAW);
    if (!EXR_OK(rc)) { FAIL("extract-channel", exr_result_string(rc)); goto done; }
    if (memcmp(ref, got, n * sizeof(float)) == 0)
        PASS("extract-channel (HALF->FLOAT bit-exact)");
    else
        FAIL("extract-channel", "differs from exr_half_to_float");
done:
    free(ref); free(got);
    exr_image_free(&img);
}

/* ---- combine channels: GPU gather vs CPU header-order widen ------------- */
static void test_combine_channels(exr_vk_context *ctx, const char *rel) {
    char *p = path_of(rel);
    exr_image img;
    const exr_part *pt;
    float *gpu = NULL, *ref = NULL;
    int ow = 0, oh = 0, oc = 0, outc, cc;
    size_t n;
    exr_result rc;
    if (!file_exists(p)) { SKIP("combine-channels", "missing"); return; }
    memset(&img, 0, sizeof(img));
    rc = exr_load_from_file(p, NULL, &img);
    if (!EXR_OK(rc)) { SKIP("combine-channels", "cpu load failed"); return; }
    pt = &img.parts[0];
    if (!part_is_scanline_nosub(&pt->header)) {
        SKIP("combine-channels", "not scanline/non-subsampled");
        exr_image_free(&img); return;
    }
    rc = exr_vk_part_to_rgba_float(ctx, NULL, pt, &gpu, &ow, &oh, &oc);
    if (!EXR_OK(rc)) { FAIL("combine-channels", exr_result_string(rc)); exr_image_free(&img); return; }
    /* CPU reference: gather header-order channels widened to float, interleaved. */
    outc = oc;
    n = (size_t)ow * oh;
    ref = (float *)malloc(n * outc * sizeof(float));
    if (!ref) { SKIP("combine-channels", "oom"); free(gpu); exr_image_free(&img); return; }
    for (cc = 0; cc < outc; ++cc) {
        size_t i;
        exr_pixel_type t = pt->header.channels[cc].pixel_type;
        float *tmp = (float *)malloc(n * sizeof(float));
        if (!tmp) { SKIP("combine-channels", "oom"); free(ref); free(gpu); exr_image_free(&img); return; }
        if (t == EXR_PIXEL_HALF)
            exr_half_to_float((const uint16_t *)pt->images[cc], tmp, n);
        else if (t == EXR_PIXEL_FLOAT)
            memcpy(tmp, pt->images[cc], n * sizeof(float));
        else
            for (i = 0; i < n; ++i) tmp[i] = (float)((const uint32_t *)pt->images[cc])[i];
        for (i = 0; i < n; ++i) ref[i * outc + cc] = tmp[i];
        free(tmp);
    }
    if (memcmp(ref, gpu, n * outc * sizeof(float)) == 0)
        PASS("combine-channels (planar->interleaved gather)");
    else
        FAIL("combine-channels", "differs from CPU gather");
    free(ref); free(gpu);
    exr_image_free(&img);
}

/* ---- processing: color matrix / transfer / tonemap vs CPU twins --------- */
static void test_processing(exr_vk_context *ctx) {
    size_t n = 4096, i;
    int ch = 4;
    float *src = (float *)malloc(n * ch * sizeof(float));
    float *cpu = (float *)malloc(n * ch * sizeof(float));
    float *gpu = (float *)malloc(n * ch * sizeof(float));
    float m[9];
    exr_result rc;
    double md;
    if (!src || !cpu || !gpu) { SKIP("processing", "oom"); goto done; }
    for (i = 0; i < n * ch; ++i) src[i] = (float)((i * 1103515245u + 12345u) % 1000) / 999.0f;

    /* color matrix: Rec709 -> Rec2020 */
    rc = exr_color_matrix(EXR_CS_REC709, EXR_CS_REC2020, m);
    if (EXR_OK(rc)) {
        exr_color_apply_matrix(cpu, src, n, ch, m);
        rc = exr_vk_color_apply_matrix(ctx, gpu, src, n, ch, m);
        if (!EXR_OK(rc)) FAIL("color-matrix gpu", exr_result_string(rc));
        else { md = max_abs_diff(cpu, gpu, n * ch);
               if (md < 1e-4) PASS("color-matrix (Rec709->Rec2020)");
               else FAIL("color-matrix", "exceeds tolerance"); }
    }
    /* transfer: sRGB encode */
    exr_encode_transfer(cpu, src, n * ch, EXR_TF_SRGB);
    rc = exr_vk_encode_transfer(ctx, gpu, src, n * ch, EXR_TF_SRGB);
    if (!EXR_OK(rc)) FAIL("transfer gpu", exr_result_string(rc));
    else { md = max_abs_diff(cpu, gpu, n * ch);
           if (md < 1e-4) PASS("transfer (sRGB encode)");
           else FAIL("transfer sRGB", "exceeds tolerance"); }

    /* tonemap: ACES */
    exr_tonemap_float(cpu, src, n, ch, EXR_TONEMAP_ACES, NULL);
    rc = exr_vk_tonemap_float(ctx, gpu, src, n, ch, EXR_TONEMAP_ACES, NULL);
    if (!EXR_OK(rc)) FAIL("tonemap gpu", exr_result_string(rc));
    else { md = max_abs_diff(cpu, gpu, n * ch);
           if (md < 1e-4) PASS("tonemap (ACES)");
           else FAIL("tonemap ACES", "exceeds tolerance"); }
done:
    free(src); free(cpu); free(gpu);
}

int main(int argc, char **argv) {
    exr_vk_context *ctx = NULL;
    exr_result rc;
    char name[256];
    const char *home;

    if (argc > 1) snprintf(g_dir, sizeof(g_dir), "%s", argv[1]);
    else {
        home = getenv("HOME");
        snprintf(g_dir, sizeof(g_dir), "%s/work/openexr-images", home ? home : ".");
    }

    if (!exr_vk_available()) {
        printf("SKIP: no Vulkan device available\n");
        return 77;
    }
    if (exr_vk_device_name(0, name, sizeof(name)) == EXR_SUCCESS)
        printf("Vulkan device 0: %s\n", name);

    rc = exr_vk_context_create(NULL, NULL, &ctx);
    if (!EXR_OK(rc)) {
        printf("FAIL: exr_vk_context_create: %s\n", exr_result_string(rc));
        return 1;
    }
    printf("images dir: %s\n", g_dir);

    printf("\n[decode parity]\n");
    test_decode(ctx, "ScanLines/Desk.exr");
    test_decode(ctx, "ScanLines/Carrots.exr");
    test_decode(ctx, "ScanLines/Tree.exr");
    test_decode(ctx, "TestImages/AllHalfValues.exr");
    test_decode(ctx, "Tiles/Spirals.exr");           /* tiled -> CPU fallback */

    printf("\n[encode roundtrip]\n");
    test_encode(ctx, "ScanLines/Desk.exr");
    test_encode(ctx, "ScanLines/Carrots.exr");

    printf("\n[resize]\n");
    test_resize(ctx, "ScanLines/Desk.exr");

    printf("\n[extract / combine channels]\n");
    test_extract_channel(ctx, "ScanLines/Desk.exr");
    test_combine_channels(ctx, "ScanLines/Desk.exr");

    printf("\n[processing]\n");
    test_processing(ctx);

    exr_vk_context_destroy(ctx);

    printf("\n=== %d passed, %d failed, %d skipped ===\n", g_pass, g_fail, g_skip);
    return g_fail ? 1 : 0;
}
