/*
 * TinyEXR texcomp / tir browser demo — WASM entry points.
 *
 * Ties together the four pure-C11 tools so the browser can drive the whole
 * pipeline on a real image:
 *
 *   tinyexr v3  EXR decode (HDR sources straight from openexr-images)
 *   tir         content-aware resize
 *   texcomp     block compression + decompression (BC/ETC2/EAC/ASTC/uni)
 *   texpipe     mip chains, normal/roughness coherence, KTX2/DDS containers
 *   envmap      equirect -> cubemap / octahedral projection
 *
 * The module keeps a small amount of state (source image, working image after
 * resize, last compressed payload, last decode) so the JS side can drive it as
 * a pipeline without shuttling large buffers back and forth. Everything is
 * float RGBA internally; LDR sources are just floats in [0,1].
 *
 * Copyright (c) 2014-2026 Syoyo Fujita and TinyEXR authors
 * SPDX-License-Identifier: Apache-2.0
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "exr.h"
#include "tir.h"
#include "texcomp.h"
#include "texpipe.h"
#include "envmap.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#define TCW_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define TCW_EXPORT
#endif

/* --------------------------------------------------------------- state --- */

typedef struct tcw_img {
    int w, h;
    float *rgba; /* w*h*4 */
} tcw_img;

static tcw_img g_src;     /* as loaded (EXR or 8-bit)                        */
static tcw_img g_work;    /* after resize; the thing we compress             */
static tcw_img g_decoded; /* what came back out of the codec                 */
static int g_src_hdr;     /* 1 = source carries values outside [0,1]         */

static uint8_t *g_blocks;      /* last compressed payload                    */
static size_t g_blocks_size;
static uint8_t *g_container;   /* last KTX2/DDS blob                         */
static size_t g_container_size;
static uint8_t *g_preview;     /* RGBA8 tonemapped preview                   */
static size_t g_preview_cap;

static double g_encode_ms, g_decode_ms;
static char g_msg[256];

/* Mip chain (built on demand for the LOD panel). */
static tp_mip_chain g_chain;
static int g_chain_valid;

static void tcw_img_free(tcw_img *im) {
    free(im->rgba);
    im->rgba = NULL;
    im->w = im->h = 0;
}

static int tcw_img_alloc(tcw_img *im, int w, int h) {
    tcw_img_free(im);
    if (w <= 0 || h <= 0) return 0;
    im->rgba = (float *)malloc((size_t)w * h * 4u * sizeof(float));
    if (!im->rgba) return 0;
    im->w = w;
    im->h = h;
    return 1;
}

static double tcw_now_ms(void) {
#ifdef __EMSCRIPTEN__
    return emscripten_get_now();
#else
    return 0.0;
#endif
}

TCW_EXPORT const char *tcw_message(void) { return g_msg; }
TCW_EXPORT int tcw_src_width(void) { return g_src.w; }
TCW_EXPORT int tcw_src_height(void) { return g_src.h; }
TCW_EXPORT int tcw_src_is_hdr(void) { return g_src_hdr; }
TCW_EXPORT int tcw_work_width(void) { return g_work.w; }
TCW_EXPORT int tcw_work_height(void) { return g_work.h; }
TCW_EXPORT int tcw_blocks_size(void) { return (int)g_blocks_size; }
TCW_EXPORT const uint8_t *tcw_blocks_ptr(void) { return g_blocks; }
TCW_EXPORT int tcw_container_size(void) { return (int)g_container_size; }
TCW_EXPORT const uint8_t *tcw_container_ptr(void) { return g_container; }
TCW_EXPORT double tcw_encode_ms(void) { return g_encode_ms; }
TCW_EXPORT double tcw_decode_ms(void) { return g_decode_ms; }
TCW_EXPORT const char *tcw_backend(void) { return tc_backend_name(); }

/* ---------------------------------------------------------------- load --- */

/* Read pixel `i` of channel `c` as float, whatever the stored type is. */
static float tcw_chan(const exr_part *p, int c, size_t i) {
    const void *buf;
    if (c < 0 || !p->images || !p->images[c]) return 0.0f;
    buf = p->images[c];
    switch (p->header.channels[c].pixel_type) {
    case EXR_PIXEL_HALF: {
        float f;
        exr_half_to_float((const uint16_t *)buf + i, &f, 1);
        return f;
    }
    case EXR_PIXEL_FLOAT: return ((const float *)buf)[i];
    case EXR_PIXEL_UINT: return (float)((const uint32_t *)buf)[i];
    }
    return 0.0f;
}

/* Decode an EXR into the source slot. Channels are matched by name so a
 * 3-channel scene-linear image still lands as RGBA with alpha = 1; a
 * single-channel image is replicated to grey. Part 0 only. */
TCW_EXPORT int tcw_load_exr(const uint8_t *bytes, int len) {
    exr_image img;
    exr_result r;
    exr_part *p;
    int i, ci[4] = {-1, -1, -1, -1};
    if (!bytes || len <= 0) return 0;
    memset(&img, 0, sizeof(img));
    r = exr_load_from_memory(bytes, (size_t)len, NULL, &img);
    if (r != EXR_SUCCESS) {
        snprintf(g_msg, sizeof(g_msg), "EXR decode failed: %s",
                 exr_result_string(r));
        return 0;
    }
    if (img.num_parts < 1) {
        exr_image_free(&img);
        snprintf(g_msg, sizeof(g_msg), "EXR has no parts");
        return 0;
    }
    p = &img.parts[0];
    if (p->is_deep || !p->images) {
        exr_image_free(&img);
        snprintf(g_msg, sizeof(g_msg), "deep EXR not supported in this demo");
        return 0;
    }
    for (i = 0; i < p->header.num_channels; ++i) {
        const char *nm = p->header.channels[i].name;
        if (!strcmp(nm, "R")) ci[0] = i;
        else if (!strcmp(nm, "G")) ci[1] = i;
        else if (!strcmp(nm, "B")) ci[2] = i;
        else if (!strcmp(nm, "A")) ci[3] = i;
    }
    /* Greyscale / oddly-named: replicate the first channel. */
    if (ci[0] < 0 && ci[1] < 0 && ci[2] < 0 && p->header.num_channels > 0)
        ci[0] = ci[1] = ci[2] = 0;

    if (!tcw_img_alloc(&g_src, p->width, p->height)) {
        exr_image_free(&img);
        snprintf(g_msg, sizeof(g_msg), "out of memory");
        return 0;
    }
    {
        size_t px = (size_t)p->width * p->height, i2;
        int c;
        g_src_hdr = 0;
        for (i2 = 0; i2 < px; ++i2)
            for (c = 0; c < 4; ++c) {
                float v = (c == 3 && ci[3] < 0) ? 1.0f : tcw_chan(p, ci[c], i2);
                if (!(v == v)) v = 0.0f; /* NaN */
                if (c < 3 && (v > 1.0f || v < 0.0f)) g_src_hdr = 1;
                g_src.rgba[i2 * 4u + (size_t)c] = v;
            }
    }
    exr_image_free(&img);
    tcw_img_free(&g_decoded);
    g_chain_valid = 0;
    snprintf(g_msg, sizeof(g_msg), "%dx%d EXR, %s", g_src.w, g_src.h,
             g_src_hdr ? "HDR (scene-linear)" : "LDR range");
    return 1;
}

/* Source from a browser-decoded PNG/JPEG (canvas gives us sRGB RGBA8). */
TCW_EXPORT int tcw_load_rgba8(const uint8_t *rgba, int w, int h) {
    size_t px = (size_t)w * h, p;
    int c;
    if (!rgba || w <= 0 || h <= 0) return 0;
    if (!tcw_img_alloc(&g_src, w, h)) return 0;
    for (p = 0; p < px; ++p)
        for (c = 0; c < 4; ++c)
            g_src.rgba[p * 4u + (size_t)c] = (float)rgba[p * 4u + (size_t)c] / 255.0f;
    g_src_hdr = 0;
    tcw_img_free(&g_decoded);
    g_chain_valid = 0;
    snprintf(g_msg, sizeof(g_msg), "%dx%d 8-bit", w, h);
    return 1;
}

/* -------------------------------------------------------------- resize --- */

static void tcw_view(tir_image_view *v, const tcw_img *im) {
    memset(v, 0, sizeof(*v));
    v->data = im->rgba;
    v->width = im->w;
    v->height = im->h;
    v->channels = 4;
    v->type = TIR_F32;
    v->row_stride_bytes = 0;
}

static float tcw_srgb_to_linear(float c) {
    if (c <= 0.04045f) return c / 12.92f;
    return powf((c + 0.055f) / 1.055f, 2.4f);
}

static float tcw_linear_to_srgb(float c) {
    if (c <= 0.0031308f) return c * 12.92f;
    return 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
}

/* Resize the source into the working image. `mode` is a tir_mode (GENERAL /
 * NORMAL_MAP / HEIGHTMAP), `filter` a tir_filter.
 *
 * `srgb_aware` decodes the RGB to linear light before filtering and re-encodes
 * afterwards. Filtering directly in sRGB darkens what it averages -- a black /
 * white checker minifies to ~0.5 sRGB (0.21 linear) instead of 0.5 linear
 * (~0.74 sRGB). Toggling it on a high-contrast image is the clearest thing in
 * the whole demo, which is why it is exposed. */
TCW_EXPORT int tcw_resize(int w, int h, int filter, int mode, int srgb_aware,
                          int alpha_mode) {
    tir_image_view sv, dv;
    tir_options opt;
    tir_result r;
    tcw_img lin;
    const tcw_img *from = &g_src;
    size_t px, i;
    int c;
    if (!g_src.rgba) return 0;
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return 0;

    memset(&lin, 0, sizeof(lin));
    if (srgb_aware && !g_src_hdr) {
        if (!tcw_img_alloc(&lin, g_src.w, g_src.h)) return 0;
        px = (size_t)g_src.w * g_src.h;
        for (i = 0; i < px; ++i) {
            for (c = 0; c < 3; ++c)
                lin.rgba[i * 4u + (size_t)c] =
                    tcw_srgb_to_linear(g_src.rgba[i * 4u + (size_t)c]);
            lin.rgba[i * 4u + 3u] = g_src.rgba[i * 4u + 3u]; /* alpha stays linear */
        }
        from = &lin;
    }

    if (!tcw_img_alloc(&g_work, w, h)) {
        tcw_img_free(&lin);
        return 0;
    }
    tcw_view(&sv, from);
    tcw_view(&dv, &g_work);
    tir_options_init(&opt);
    opt.filter_x = (tir_filter)filter;
    opt.filter_y = (tir_filter)filter;
    opt.mode = (tir_mode)mode;
    opt.alpha = (tir_alpha_mode)alpha_mode;
    r = tir_resize(NULL, &sv, &dv, &opt);
    tcw_img_free(&lin);
    if (r != TIR_SUCCESS) {
        snprintf(g_msg, sizeof(g_msg), "resize failed: %s", tir_result_string(r));
        return 0;
    }
    if (srgb_aware && !g_src_hdr) {
        px = (size_t)w * h;
        for (i = 0; i < px; ++i)
            for (c = 0; c < 3; ++c)
                g_work.rgba[i * 4u + (size_t)c] =
                    tcw_linear_to_srgb(g_work.rgba[i * 4u + (size_t)c]);
    }
    tcw_img_free(&g_decoded);
    g_chain_valid = 0;
    return 1;
}

/* Copy the source through unchanged (the "no resize" path). */
TCW_EXPORT int tcw_use_source(void) {
    if (!g_src.rgba) return 0;
    if (!tcw_img_alloc(&g_work, g_src.w, g_src.h)) return 0;
    memcpy(g_work.rgba, g_src.rgba,
           (size_t)g_src.w * g_src.h * 4u * sizeof(float));
    tcw_img_free(&g_decoded);
    g_chain_valid = 0;
    return 1;
}

/* ------------------------------------------------------------ compress --- */

/* Codec ids shared with the JS side (kept stable; see app.js CODECS). */
enum {
    TCW_BC1 = 0, TCW_BC3, TCW_BC5, TCW_BC7, TCW_BC6H,
    TCW_ETC2_RGB, TCW_ETC2_RGBA, TCW_EAC_R11, TCW_EAC_RG11,
    TCW_ASTC, TCW_ASTC_HDR, TCW_UNI
};

TCW_EXPORT int tcw_codec_is_hdr(int codec) {
    return codec == TCW_BC6H || codec == TCW_ASTC_HDR;
}

static void tcw_work_to_rgba8(uint8_t *dst) {
    size_t px = (size_t)g_work.w * g_work.h, p;
    int c;
    for (p = 0; p < px; ++p)
        for (c = 0; c < 4; ++c) {
            float v = g_work.rgba[p * 4u + (size_t)c];
            int i = (int)(v * 255.0f + 0.5f);
            dst[p * 4u + (size_t)c] = (uint8_t)(i < 0 ? 0 : (i > 255 ? 255 : i));
        }
}

/* Encode the working image with `codec`. `quality` maps to each codec's own
 * quality knob; block_x/block_y only matter for ASTC. Returns the payload size
 * in bytes, or 0 on failure. Timing is recorded for the UI. */
TCW_EXPORT int tcw_compress(int codec, int quality, int block_x, int block_y,
                            int srgb, int normal_weighted) {
    uint32_t w, h;
    size_t need = 0;
    tc_result r = TC_ERROR_UNSUPPORTED;
    double t0;
    uint8_t *rgba8 = NULL;
    if (!g_work.rgba) return 0;
    w = (uint32_t)g_work.w;
    h = (uint32_t)g_work.h;

    switch (codec) {
    case TCW_BC1: need = tc_bc1_compressed_size(w, h); break;
    case TCW_BC3: need = tc_bc3_compressed_size(w, h); break;
    case TCW_BC5: need = tc_bc5_compressed_size(w, h); break;
    case TCW_BC7: need = tc_bc7_compressed_size(w, h); break;
    case TCW_BC6H: need = tc_bc6h_compressed_size(w, h); break;
    case TCW_ETC2_RGB: need = tc_etc2_rgb_compressed_size(w, h); break;
    case TCW_ETC2_RGBA: need = tc_etc2_rgba_compressed_size(w, h); break;
    case TCW_EAC_R11: need = tc_eac_r11_compressed_size(w, h); break;
    case TCW_EAC_RG11: need = tc_eac_rg11_compressed_size(w, h); break;
    case TCW_UNI: need = tc_uni_compressed_size(w, h); break;
    case TCW_ASTC: {
        tc_astc_options ao;
        tc_astc_options_init(&ao);
        ao.block_x = (uint32_t)block_x;
        ao.block_y = (uint32_t)block_y;
        need = tc_astc_compressed_size(w, h, &ao);
        break;
    }
    case TCW_ASTC_HDR: need = tc_astc_hdr_compressed_size(w, h); break;
    default: return 0;
    }
    if (!need) return 0;

    free(g_blocks);
    g_blocks = (uint8_t *)malloc(need);
    if (!g_blocks) return 0;
    g_blocks_size = need;

    if (!tcw_codec_is_hdr(codec)) {
        rgba8 = (uint8_t *)malloc((size_t)w * h * 4u);
        if (!rgba8) return 0;
        tcw_work_to_rgba8(rgba8);
    }

    t0 = tcw_now_ms();
    switch (codec) {
    case TCW_BC1: {
        tc_bc1_options o; tc_bc1_options_init(&o); o.srgb = srgb;
        r = tc_bc1_compress_rgba8(rgba8, w, h, (size_t)w * 4u, &o, g_blocks, need);
        break;
    }
    case TCW_BC3: {
        tc_bc3_options o; tc_bc3_options_init(&o); o.srgb = srgb;
        r = tc_bc3_compress_rgba8(rgba8, w, h, (size_t)w * 4u, &o, g_blocks, need);
        break;
    }
    case TCW_BC5: {
        tc_bc5_options o; tc_bc5_options_init(&o);
        r = tc_bc5_compress_rgba8(rgba8, w, h, (size_t)w * 4u, &o, g_blocks, need);
        break;
    }
    case TCW_BC7: {
        tc_bc7_options o;
        tc_bc7_options_init(&o);
        o.quality = (tc_bc7_quality)(quality < 0 ? 0 : (quality > 2 ? 2 : quality));
        o.srgb = srgb;
        if (normal_weighted) {
            o.channel_weights[0] = 2; o.channel_weights[1] = 2;
            o.channel_weights[2] = 1; o.channel_weights[3] = 1;
        }
        r = tc_bc7_compress_rgba8(rgba8, w, h, (size_t)w * 4u, &o, g_blocks, need);
        break;
    }
    case TCW_BC6H: {
        tc_bc6h_options o;
        tc_bc6h_options_init(&o);
        /* BC6H takes packed RGB float. */
        {
            float *rgb = (float *)malloc((size_t)w * h * 3u * sizeof(float));
            size_t px = (size_t)w * h, p;
            if (!rgb) { r = TC_ERROR_OUT_OF_MEMORY; break; }
            for (p = 0; p < px; ++p) {
                rgb[p * 3u + 0] = g_work.rgba[p * 4u + 0];
                rgb[p * 3u + 1] = g_work.rgba[p * 4u + 1];
                rgb[p * 3u + 2] = g_work.rgba[p * 4u + 2];
            }
            r = tc_bc6h_compress_rgb32f(rgb, w, h, (size_t)w * 3u * sizeof(float),
                                        &o, g_blocks, need);
            free(rgb);
        }
        break;
    }
    case TCW_ETC2_RGB:
    case TCW_ETC2_RGBA: {
        tc_etc2_options o;
        tc_etc2_options_init(&o);
        o.srgb = srgb;
        o.alpha = (codec == TCW_ETC2_RGBA);
        r = tc_etc2_compress_rgba8(rgba8, w, h, (size_t)w * 4u, &o, g_blocks, need);
        break;
    }
    case TCW_EAC_R11:
    case TCW_EAC_RG11:
        r = tc_eac_compress_rgba8(rgba8, w, h, (size_t)w * 4u,
                                  codec == TCW_EAC_RG11, g_blocks, need);
        break;
    case TCW_ASTC: {
        tc_astc_options o;
        tc_astc_options_init(&o);
        o.block_x = (uint32_t)block_x;
        o.block_y = (uint32_t)block_y;
        o.quality = quality;
        o.srgb = srgb;
        r = tc_astc_compress_rgba8(rgba8, w, h, (size_t)w * 4u, &o, g_blocks, need);
        break;
    }
    case TCW_ASTC_HDR: {
        tc_astc_hdr_options o;
        float *rgb = (float *)malloc((size_t)w * h * 3u * sizeof(float));
        size_t px = (size_t)w * h, p2;
        tc_astc_hdr_options_init(&o);
        o.quality = quality;
        if (!rgb) { r = TC_ERROR_OUT_OF_MEMORY; break; }
        for (p2 = 0; p2 < px; ++p2) {
            rgb[p2 * 3u + 0] = g_work.rgba[p2 * 4u + 0];
            rgb[p2 * 3u + 1] = g_work.rgba[p2 * 4u + 1];
            rgb[p2 * 3u + 2] = g_work.rgba[p2 * 4u + 2];
        }
        /* The RGB (CEM 7/11) path. The RGBA/CEM 15 encoder currently emits
         * broken blocks on some ordinary content -- confirmed against astcenc,
         * which decodes them to the same garbage we do -- so the demo does not
         * drive it. HDR textures are RGB in practice anyway. */
        r = tc_astc_hdr_compress_rgbf(rgb, w, h, (size_t)w * 3u * sizeof(float),
                                      &o, g_blocks, need);
        free(rgb);
        break;
    }
    case TCW_UNI:
        r = tc_uni_compress_rgba8(rgba8, w, h, (size_t)w * 4u, g_blocks, need);
        break;
    }
    g_encode_ms = tcw_now_ms() - t0;
    free(rgba8);

    if (r != TC_SUCCESS) {
        snprintf(g_msg, sizeof(g_msg), "encode failed: %s", tc_result_string(r));
        g_blocks_size = 0;
        return 0;
    }
    snprintf(g_msg, sizeof(g_msg), "ok");
    return (int)g_blocks_size;
}

/* Decode the last payload back into the decoded slot, so the UI can diff it
 * against the working image. This is the same decoder set the KTX2 reader
 * uses, so what you see here is what a GPU would sample. */
TCW_EXPORT int tcw_decode(int codec, int block_x, int block_y) {
    uint32_t w, h;
    tc_result r = TC_ERROR_UNSUPPORTED;
    double t0;
    if (!g_blocks || !g_work.rgba) return 0;
    w = (uint32_t)g_work.w;
    h = (uint32_t)g_work.h;
    if (!tcw_img_alloc(&g_decoded, (int)w, (int)h)) return 0;

    t0 = tcw_now_ms();
    if (tcw_codec_is_hdr(codec)) {
        if (codec == TCW_BC6H)
            r = tc_bc6h_decompress_rgbaf(g_blocks, w, h, 0,
                                         (size_t)w * 4u * sizeof(float),
                                         g_decoded.rgba,
                                         (size_t)w * h * 4u * sizeof(float));
        else
            r = tc_astc_hdr_decompress_rgbaf(g_blocks, w, h, (uint32_t)block_x,
                                             (uint32_t)block_y,
                                             (size_t)w * 4u * sizeof(float),
                                             g_decoded.rgba,
                                             (size_t)w * h * 4u * sizeof(float));
        g_decode_ms = tcw_now_ms() - t0;
    } else {
        uint8_t *out = (uint8_t *)malloc((size_t)w * h * 4u);
        size_t px = (size_t)w * h, p;
        int c;
        if (!out) return 0;
        switch (codec) {
        case TCW_BC1: r = tc_bc1_decompress_rgba8(g_blocks, w, h, (size_t)w * 4u, out, px * 4u); break;
        case TCW_BC3: r = tc_bc3_decompress_rgba8(g_blocks, w, h, (size_t)w * 4u, out, px * 4u); break;
        case TCW_BC5: r = tc_bc5_decompress_rgba8(g_blocks, w, h, 0, (size_t)w * 4u, out, px * 4u); break;
        case TCW_BC7: r = tc_bc7_decompress_rgba8(g_blocks, w, h, (size_t)w * 4u, out, px * 4u); break;
        case TCW_ETC2_RGB:
        case TCW_ETC2_RGBA:
            r = tc_etc2_decompress_rgba8(g_blocks, w, h, codec == TCW_ETC2_RGBA,
                                         (size_t)w * 4u, out, px * 4u);
            break;
        case TCW_EAC_R11:
        case TCW_EAC_RG11:
            r = tc_eac_decompress_rgba8(g_blocks, w, h, codec == TCW_EAC_RG11,
                                        (size_t)w * 4u, out, px * 4u);
            break;
        case TCW_ASTC:
            r = tc_astc_decompress_rgba8(g_blocks, w, h, (uint32_t)block_x,
                                         (uint32_t)block_y, out, px * 4u);
            break;
        case TCW_UNI:
            r = tc_uni_decompress_rgba8(g_blocks, w, h, (size_t)w * 4u, out, px * 4u);
            break;
        }
        g_decode_ms = tcw_now_ms() - t0;
        if (r == TC_SUCCESS)
            for (p = 0; p < px; ++p)
                for (c = 0; c < 4; ++c)
                    g_decoded.rgba[p * 4u + (size_t)c] =
                        (float)out[p * 4u + (size_t)c] / 255.0f;
        free(out);
    }
    if (r != TC_SUCCESS) {
        snprintf(g_msg, sizeof(g_msg), "decode failed: %s", tc_result_string(r));
        return 0;
    }
    return 1;
}

/* PSNR of the decode against the working image. `channels` = 3 (RGB) or 4.
 * For HDR the peak is the working image's own maximum, which is the honest
 * thing to normalise by when values run past 1.0. */
TCW_EXPORT double tcw_psnr(int channels) {
    size_t px, p;
    int c, nc = (channels == 4) ? 4 : 3;
    double sse = 0.0, peak = 1.0;
    if (!g_work.rgba || !g_decoded.rgba) return 0.0;
    px = (size_t)g_work.w * g_work.h;
    if (g_src_hdr) {
        for (p = 0; p < px * 4u; ++p)
            if (g_work.rgba[p] > peak) peak = g_work.rgba[p];
    }
    for (p = 0; p < px; ++p)
        for (c = 0; c < nc; ++c) {
            double d = (double)g_work.rgba[p * 4u + (size_t)c] -
                       (double)g_decoded.rgba[p * 4u + (size_t)c];
            sse += d * d;
        }
    if (sse <= 0.0) return 99.0;
    return 10.0 * log10(peak * peak / (sse / (double)(px * (size_t)nc)));
}

/* Mean angular error, in degrees, between the working image and the decode
 * read as tangent-space normals. This is the metric that actually matters for
 * a normal map — PSNR on the raw channels does not tell you whether the
 * lighting will be wrong. */
TCW_EXPORT double tcw_normal_angular_error(int reconstruct_z) {
    size_t px, p;
    double sum = 0.0;
    int n = 0;
    if (!g_work.rgba || !g_decoded.rgba) return 0.0;
    px = (size_t)g_work.w * g_work.h;
    for (p = 0; p < px; ++p) {
        float a[3], b[3];
        double la, lb, dot;
        int c;
        for (c = 0; c < 3; ++c) {
            a[c] = g_work.rgba[p * 4u + (size_t)c] * 2.0f - 1.0f;
            b[c] = g_decoded.rgba[p * 4u + (size_t)c] * 2.0f - 1.0f;
        }
        if (reconstruct_z) { /* BC5/EAC keep only X,Y: rebuild Z as a GPU would */
            double zz = 1.0 - (double)a[0] * a[0] - (double)a[1] * a[1];
            a[2] = (float)sqrt(zz > 0.0 ? zz : 0.0);
            zz = 1.0 - (double)b[0] * b[0] - (double)b[1] * b[1];
            b[2] = (float)sqrt(zz > 0.0 ? zz : 0.0);
        }
        la = sqrt((double)a[0] * a[0] + (double)a[1] * a[1] + (double)a[2] * a[2]);
        lb = sqrt((double)b[0] * b[0] + (double)b[1] * b[1] + (double)b[2] * b[2]);
        if (la < 1e-6 || lb < 1e-6) continue;
        dot = ((double)a[0] * b[0] + (double)a[1] * b[1] + (double)a[2] * b[2]) /
              (la * lb);
        if (dot > 1.0) dot = 1.0;
        if (dot < -1.0) dot = -1.0;
        sum += acos(dot) * 180.0 / 3.14159265358979323846;
        ++n;
    }
    return n ? sum / (double)n : 0.0;
}

/* ------------------------------------------------------------- preview --- */

/* Tonemap one of the images to RGBA8 for a canvas. `which`: 0 = work,
 * 1 = decoded, 2 = |work - decoded| amplified (the error view — the most
 * informative thing you can look at when comparing codecs). */
TCW_EXPORT const uint8_t *tcw_preview(int which, float exposure, float gamma,
                                      float err_scale) {
    const tcw_img *im = (which == 1) ? &g_decoded : &g_work;
    size_t px, p;
    int c;
    if (!im->rgba) return NULL;
    px = (size_t)im->w * im->h;
    if (g_preview_cap < px * 4u) {
        free(g_preview);
        g_preview = (uint8_t *)malloc(px * 4u);
        if (!g_preview) { g_preview_cap = 0; return NULL; }
        g_preview_cap = px * 4u;
    }
    for (p = 0; p < px; ++p) {
        for (c = 0; c < 3; ++c) {
            float v;
            if (which == 2) {
                if (!g_decoded.rgba) return NULL;
                v = fabsf(g_work.rgba[p * 4u + (size_t)c] -
                          g_decoded.rgba[p * 4u + (size_t)c]) * err_scale;
            } else {
                v = im->rgba[p * 4u + (size_t)c] * exposure;
            }
            if (v < 0.0f) v = 0.0f;
            v = powf(v, 1.0f / (gamma > 0.01f ? gamma : 1.0f));
            if (v > 1.0f) v = 1.0f;
            g_preview[p * 4u + (size_t)c] = (uint8_t)(v * 255.0f + 0.5f);
        }
        g_preview[p * 4u + 3u] =
            (which == 2) ? 255u
                         : (uint8_t)(im->rgba[p * 4u + 3u] * 255.0f + 0.5f);
    }
    return g_preview;
}

/* ----------------------------------------------------------- mip chain --- */

/* Build a content-aware mip chain from the working image. `content` is a
 * tp_content (COLOR / ALPHA_TESTED / NORMAL / HEIGHT), so the LOD panel can
 * show what alpha-coverage preservation and normal renormalisation actually
 * do to the pyramid. */
TCW_EXPORT int tcw_build_mips(int content, int filter, int srgb_aware) {
    tir_image_view v;
    tp_options opt;
    if (!g_work.rgba) return 0;
    if (g_chain_valid) {
        tp_mip_chain_free(NULL, &g_chain);
        g_chain_valid = 0;
    }
    tcw_view(&v, &g_work);
    tp_options_init(&opt, (tp_content)content, TP_CODEC_BC7);
    opt.filter = (tir_filter)filter;
    opt.srgb_aware = srgb_aware ? 1 : 0;
    memset(&g_chain, 0, sizeof(g_chain));
    if (!TP_OK(tp_build_mips(NULL, &v, 1, &opt, &g_chain))) {
        snprintf(g_msg, sizeof(g_msg), "mip build failed");
        return 0;
    }
    g_chain_valid = 1;
    return g_chain.num_levels;
}

TCW_EXPORT int tcw_mip_width(int level) {
    if (!g_chain_valid || level < 0 || level >= g_chain.num_levels) return 0;
    return g_chain.level[level].width;
}
TCW_EXPORT int tcw_mip_height(int level) {
    if (!g_chain_valid || level < 0 || level >= g_chain.num_levels) return 0;
    return g_chain.level[level].height;
}

/* Copy one mip level into the working slot so the usual compress/preview path
 * applies to it — lets the UI compress a chosen LOD with no extra plumbing. */
TCW_EXPORT int tcw_select_mip(int level) {
    const tp_surface *s;
    int y, x, c;
    if (!g_chain_valid || level < 0 || level >= g_chain.num_levels) return 0;
    s = &g_chain.level[level];
    if (!tcw_img_alloc(&g_work, s->width, s->height)) return 0;
    for (y = 0; y < s->height; ++y) {
        const float *row = (const float *)((const uint8_t *)s->data +
                                           (size_t)y * s->stride);
        for (x = 0; x < s->width; ++x)
            for (c = 0; c < 4; ++c)
                g_work.rgba[((size_t)y * s->width + x) * 4u + (size_t)c] =
                    (s->channels > c) ? row[(size_t)x * s->channels + c]
                                      : (c == 3 ? 1.0f : 0.0f);
    }
    tcw_img_free(&g_decoded);
    return 1;
}

/* --------------------------------------------------------- environment --- */

/* Reproject the source (an equirect latlong EXR) to a cubemap or octahedral
 * map and put the result in the working slot. For CUBE the faces are stacked
 * vertically (face-major), which is exactly how texpipe wants them and makes a
 * readable strip in the UI. `proj` is an em_proj. */
TCW_EXPORT int tcw_project_env(int proj, int size) {
    em_image src, dst;
    em_result r;
    int face, y, x, nf;
    if (!g_src.rgba || size <= 0 || size > 2048) return 0;
    memset(&src, 0, sizeof(src));
    memset(&dst, 0, sizeof(dst));
    if (!EM_OK(em_image_alloc(NULL, &src, EM_PROJ_EQUIRECT, g_src.w, g_src.h, 3)))
        return 0;
    {
        size_t px = (size_t)g_src.w * g_src.h, p;
        for (p = 0; p < px; ++p) {
            src.data[p * 3u + 0] = g_src.rgba[p * 4u + 0];
            src.data[p * 3u + 1] = g_src.rgba[p * 4u + 1];
            src.data[p * 3u + 2] = g_src.rgba[p * 4u + 2];
        }
    }
    r = em_convert(NULL, &src, (em_proj)proj, size, &dst);
    em_image_free(NULL, &src);
    if (!EM_OK(r)) {
        snprintf(g_msg, sizeof(g_msg), "projection failed: %s",
                 em_result_string(r));
        return 0;
    }
    nf = dst.faces;
    if (!tcw_img_alloc(&g_work, dst.width, dst.height * nf)) {
        em_image_free(NULL, &dst);
        return 0;
    }
    for (face = 0; face < nf; ++face)
        for (y = 0; y < dst.height; ++y)
            for (x = 0; x < dst.width; ++x) {
                const float *s = em_image_texel(&dst, face, x, y);
                size_t o = ((size_t)(face * dst.height + y) * dst.width + x) * 4u;
                g_work.rgba[o + 0] = s[0];
                g_work.rgba[o + 1] = s[1];
                g_work.rgba[o + 2] = s[2];
                g_work.rgba[o + 3] = 1.0f;
            }
    em_image_free(NULL, &dst);
    tcw_img_free(&g_decoded);
    g_chain_valid = 0;
    snprintf(g_msg, sizeof(g_msg), "%s %dx%d%s",
             proj == EM_PROJ_CUBE ? "cubemap" : "octahedral", size, size,
             proj == EM_PROJ_CUBE ? " x6 faces" : "");
    return 1;
}

/* ------------------------------------------------------------ normal map -- */

/* Derive a tangent-space normal map from the working image's luminance, used
 * as a height field. Gives the normal-map panel real content from any source
 * image (including a photo), rather than requiring the user to find one. */
TCW_EXPORT int tcw_normal_from_height(float strength) {
    tcw_img out;
    int x, y, w = g_work.w, h = g_work.h;
    if (!g_work.rgba) return 0;
    memset(&out, 0, sizeof(out));
    if (!tcw_img_alloc(&out, w, h)) return 0;
    for (y = 0; y < h; ++y)
        for (x = 0; x < w; ++x) {
            int xm = x > 0 ? x - 1 : 0, xp = x < w - 1 ? x + 1 : w - 1;
            int ym = y > 0 ? y - 1 : 0, yp = y < h - 1 ? y + 1 : h - 1;
            const float *p = g_work.rgba;
#define TCW_LUM(ix, iy)                                                        \
    (0.2126f * p[((size_t)(iy) * w + (ix)) * 4u + 0] +                         \
     0.7152f * p[((size_t)(iy) * w + (ix)) * 4u + 1] +                         \
     0.0722f * p[((size_t)(iy) * w + (ix)) * 4u + 2])
            float dx = (TCW_LUM(xp, y) - TCW_LUM(xm, y)) * strength;
            float dy = (TCW_LUM(x, yp) - TCW_LUM(x, ym)) * strength;
#undef TCW_LUM
            float nx = -dx, ny = -dy, nz = 1.0f;
            float len = sqrtf(nx * nx + ny * ny + nz * nz);
            size_t o = ((size_t)y * w + x) * 4u;
            out.rgba[o + 0] = (nx / len) * 0.5f + 0.5f;
            out.rgba[o + 1] = (ny / len) * 0.5f + 0.5f;
            out.rgba[o + 2] = (nz / len) * 0.5f + 0.5f;
            out.rgba[o + 3] = 1.0f;
        }
    tcw_img_free(&g_work);
    g_work = out;
    tcw_img_free(&g_decoded);
    g_chain_valid = 0;
    return 1;
}

/* ----------------------------------------------------------- container --- */

/* Run the full texpipe pipeline on the working image (mips + codec + KTX2 or
 * DDS) and keep the blob so the browser can download a file a GPU tool will
 * actually open. This is the payoff of the demo: the same bytes the CLI writes. */
TCW_EXPORT int tcw_write_container(int codec, int container, int content,
                                   int quality, int block_x, int block_y,
                                   int srgb, int num_faces) {
    tir_image_view faces[6];
    tp_options opt;
    uint8_t *blob = NULL;
    size_t n = 0;
    int f, fh;
    if (!g_work.rgba) return 0;
    if (num_faces != 1 && num_faces != 6) num_faces = 1;
    fh = g_work.h / num_faces;
    if (fh <= 0) return 0;
    for (f = 0; f < num_faces; ++f) {
        memset(&faces[f], 0, sizeof(faces[f]));
        faces[f].data = g_work.rgba + (size_t)f * fh * g_work.w * 4u;
        faces[f].width = g_work.w;
        faces[f].height = fh;
        faces[f].channels = 4;
        faces[f].type = TIR_F32;
        faces[f].row_stride_bytes = 0;
    }
    tp_options_init(&opt, (tp_content)content, (tp_codec)codec);
    opt.container = (tp_container)container;
    opt.srgb = srgb;
    opt.is_cube = (num_faces == 6);
    opt.astc.block_x = (uint32_t)block_x;
    opt.astc.block_y = (uint32_t)block_y;
    opt.bc7.quality = (tc_bc7_quality)(quality < 0 ? 0 : (quality > 2 ? 2 : quality));
    opt.astc.quality = quality;
    if (!TP_OK(tp_process(NULL, faces, num_faces, &opt, &blob, &n))) {
        snprintf(g_msg, sizeof(g_msg), "container write failed");
        return 0;
    }
    free(g_container);
    g_container = blob;
    g_container_size = n;
    return (int)n;
}

/* Parse a KTX2 we just wrote (or one the user dropped in) and report what the
 * reader sees. Round-tripping our own output through our own parser in the
 * browser is a nice end-to-end sanity check. */
TCW_EXPORT const char *tcw_ktx2_info(const uint8_t *bytes, int len) {
    tp_ktx2_image img;
    tp_result r;
    if (!bytes || len <= 0) {
        snprintf(g_msg, sizeof(g_msg), "no data");
        return g_msg;
    }
    r = tp_ktx2_read(bytes, (size_t)len, &img);
    if (!TP_OK(r)) {
        snprintf(g_msg, sizeof(g_msg), "KTX2 parse failed: %s",
                 tp_result_string(r));
        return g_msg;
    }
    snprintf(g_msg, sizeof(g_msg),
             "KTX2 %dx%d, vkFormat %u, %d level%s, %d face%s%s%s",
             (int)img.width, (int)img.height, (unsigned)img.vk_format,
             img.num_levels, img.num_levels == 1 ? "" : "s", img.num_faces,
             img.num_faces == 1 ? "" : "s", img.is_uni ? ", uni (transcodable)" : "",
             img.is_hdr ? ", HDR" : "");
    return g_msg;
}
