/* HTJ2K GPU vs CPU throughput benchmark (CUDA backend).
 *
 * Encodes asakusa.exr to HTJ2K256/HTJ2K32 in memory, then times whole-image
 * decode and encode on the CPU path (exr_load/exr_save) vs the GPU path
 * (exr_gpu_load/exr_gpu_save). The GPU path is warmed up first so the one-time
 * NVRTC compile and lazy device-buffer inits are excluded from the timing.
 *
 * Throughput is megapixels/second (higher is better). Exit 77 = skip (no GPU).
 *
 *   make bench-gpu-jph      # builds + runs against ./asakusa.exr
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "exr.h"
#include "exr_gpu.h"

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* compressed payload to decode, shared by the CPU/GPU decode loops */
static const void *g_enc;
static size_t      g_enc_size;
static exr_gpu_context *g_ctx;
static const exr_image *g_src;       /* source image for the encode loops */
static exr_compression  g_comp;

static double mpix(const exr_image *im) {
    if (im->num_parts < 1) return 0.0;
    return (double)im->parts[0].width * (double)im->parts[0].height / 1e6;
}

/* Run an op for >= min_sec; return seconds/iteration. Returns -1 on op error. */
typedef int (*op_fn)(void);
static double timeit(op_fn fn, double min_sec, long *out_iters) {
    long iters = 0;
    double t0 = now_sec(), elapsed = 0.0;
    do {
        if (fn() != 0) return -1.0;
        ++iters;
        elapsed = now_sec() - t0;
    } while (elapsed < min_sec);
    if (out_iters) *out_iters = iters;
    return elapsed / (double)iters;
}

static int op_cpu_decode(void) {
    exr_image im; memset(&im, 0, sizeof im);
    if (exr_load_from_memory(g_enc, g_enc_size, NULL, &im) != EXR_SUCCESS) return 1;
    exr_image_free(&im);
    return 0;
}
static int op_gpu_decode(void) {
    exr_image im; memset(&im, 0, sizeof im);
    if (exr_gpu_load_from_memory(g_ctx, g_enc, g_enc_size, NULL, &im) != EXR_SUCCESS) return 1;
    exr_image_free(&im);
    return 0;
}
static int op_cpu_encode(void) {
    void *out = NULL; size_t out_size = 0;
    if (exr_save_to_memory(&out, &out_size, NULL, g_src, g_comp) != EXR_SUCCESS) return 1;
    free(out);
    return 0;
}
static int op_gpu_encode(void) {
    void *out = NULL; size_t out_size = 0;
    if (exr_gpu_save_to_memory(g_ctx, &out, &out_size, NULL, g_src, g_comp) != EXR_SUCCESS) return 1;
    free(out);
    return 0;
}

static void bench_codec(const char *name, exr_compression comp, const exr_image *src) {
    /* Encode once on the CPU to get a payload to decode. */
    void *enc = NULL; size_t enc_size = 0;
    if (exr_save_to_memory(&enc, &enc_size, NULL, src, comp) != EXR_SUCCESS) {
        printf("  %-9s ENCODE FAILED (skipped)\n", name);
        return;
    }
    g_enc = enc; g_enc_size = enc_size; g_src = src; g_comp = comp;
    double mp = mpix(src);

    /* Warm up the GPU paths (NVRTC compile + lazy buffer init excluded). */
    (void)op_gpu_decode();
    (void)op_gpu_encode();

    long it;
    double cdec = timeit(op_cpu_decode, 2.0, &it);
    double gdec = timeit(op_gpu_decode, 2.0, &it);
    double cenc = timeit(op_cpu_encode, 2.0, &it);
    double genc = timeit(op_gpu_encode, 2.0, &it);

    printf("  %-9s | decode CPU %6.1f  GPU %6.1f (%.2fx) | encode CPU %6.1f  GPU %6.1f (%.2fx)\n",
           name,
           cdec > 0 ? mp / cdec : 0.0, gdec > 0 ? mp / gdec : 0.0,
           (cdec > 0 && gdec > 0) ? cdec / gdec : 0.0,
           cenc > 0 ? mp / cenc : 0.0, genc > 0 ? mp / genc : 0.0,
           (cenc > 0 && genc > 0) ? cenc / genc : 0.0);
    free(enc);
}

/* Build a synthetic W x H, 4x HALF scanline image (a smooth gradient so the
 * codec does real work). Used to probe the large-image regime that the small
 * test corpus can't reach. */
static int make_synth(int w, int h, exr_image *out) {
    memset(out, 0, sizeof *out);
    exr_part *p = (exr_part *)calloc(1, sizeof *p);
    if (!p) return 1;
    out->num_parts = 1; out->parts = p;
    p->width = w; p->height = h;
    p->header.part_type = EXR_PART_SCANLINE;
    p->header.compression = EXR_COMPRESSION_HTJ2K256;
    p->header.data_window.max_x = w - 1;
    p->header.data_window.max_y = h - 1;
    p->header.display_window.max_x = w - 1;
    p->header.display_window.max_y = h - 1;
    p->header.pixel_aspect_ratio = 1.0f;
    p->header.screen_window_width = 1.0f;
    const int nch = 4;
    const char *names[4] = {"A", "B", "G", "R"};
    p->header.num_channels = nch;
    p->header.channels = (exr_channel *)calloc(nch, sizeof(exr_channel));
    p->images = (void **)calloc(nch, sizeof(void *));
    if (!p->header.channels || !p->images) return 1;
    for (int c = 0; c < nch; ++c) {
        strcpy(p->header.channels[c].name, names[c]);
        p->header.channels[c].pixel_type = EXR_PIXEL_HALF;
        p->header.channels[c].x_sampling = 1;
        p->header.channels[c].y_sampling = 1;
        uint16_t *px = (uint16_t *)malloc((size_t)w * h * sizeof(uint16_t));
        if (!px) return 1;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                /* HALF bits of a slowly varying value; deterministic. */
                px[(size_t)y * w + x] = (uint16_t)(((x * 7 + y * 13 + c * 101) & 0x3ff) | 0x3800);
        p->images[c] = px;
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "asakusa.exr";

    if (!exr_gpu_available()) { puts("SKIP: no CUDA device"); return 77; }

    exr_image src; memset(&src, 0, sizeof src);
    int synth_w = 0, synth_h = 0;
    if (sscanf(path, "synth:%dx%d", &synth_w, &synth_h) == 2) {
        if (make_synth(synth_w, synth_h, &src) != 0) {
            fprintf(stderr, "failed to build synthetic %dx%d\n", synth_w, synth_h);
            return 1;
        }
    } else if (exr_load_from_file(path, NULL, &src) != EXR_SUCCESS) {
        fprintf(stderr, "failed to load %s\n", path);
        return 1;
    }
    if (exr_gpu_context_create(NULL, NULL, &g_ctx) != EXR_SUCCESS) {
        fprintf(stderr, "failed to create GPU context\n");
        exr_image_free(&src);
        return 1;
    }

    char gname[128] = "?";
    exr_gpu_device_name(0, gname, sizeof gname);
    printf("image: %s  %dx%d  %d ch  (%.2f MP)\n",
           path, src.parts[0].width, src.parts[0].height,
           src.parts[0].header.num_channels, mpix(&src));
    printf("GPU: %s   CPU path: tinyexr in-tree HTJ2K (MP/s)\n\n", gname);

    static const struct { const char *name; exr_compression c; } codecs[] = {
        {"none",     EXR_COMPRESSION_NONE},
        {"rle",      EXR_COMPRESSION_RLE},
        {"zips",     EXR_COMPRESSION_ZIPS},
        {"zip",      EXR_COMPRESSION_ZIP},
        {"piz",      EXR_COMPRESSION_PIZ},
        {"pxr24",    EXR_COMPRESSION_PXR24},
        {"b44",      EXR_COMPRESSION_B44},
        {"zstd",     EXR_COMPRESSION_ZSTD},
        {"htj2k256", EXR_COMPRESSION_HTJ2K256},
        {"htj2k32",  EXR_COMPRESSION_HTJ2K32},
    };
    for (size_t i = 0; i < sizeof codecs / sizeof codecs[0]; ++i)
        bench_codec(codecs[i].name, codecs[i].c, &src);

    exr_gpu_context_destroy(g_ctx);
    exr_image_free(&src);
    return 0;
}
