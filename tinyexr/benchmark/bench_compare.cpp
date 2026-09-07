/*
 * TinyEXR v3 vs OpenEXR - side-by-side codec throughput / size benchmark.
 *
 * Build + run via the Makefile (it links a built OpenEXR and the tinyexr v3
 * C objects):
 *
 *   make bench-compare                       # runs on asakusa.exr
 *   make bench-compare ARGS="a.exr b.exr"    # (or run build/bench_compare ...)
 *
 * Each library independently loads the SAME source .exr into its own native
 * pixel representation, then for every codec both support it measures
 * encode-to-memory and decode-from-memory throughput (megapixels/s) and the
 * compressed size. Both sides run single-threaded, fully in memory, so the
 * numbers are directly comparable. HTJ2K (HTJ2K256/HTJ2K32) is included when
 * both libraries support it.
 *
 * The tinyexr side lives in bench_tx.c (compiled as C) because tinyexr's exr.h
 * and OpenEXR's C core declare the same global enum names and cannot share a
 * translation unit.
 *
 * Copyright (c) 2014-2026 Syoyo Fujita and TinyEXR authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <ctime>

#include "bench_tx.h" /* clean tinyexr-side wrapper (no exr.h leak) */

/* OpenEXR (generic scanline path + in-memory string streams). */
#include <ImfInputFile.h>
#include <ImfOutputFile.h>
#include <ImfHeader.h>
#include <ImfChannelList.h>
#include <ImfFrameBuffer.h>
#include <ImfStdIO.h>
#include <ImfCompression.h>
#include <ImfThreading.h>
#include <ImathBox.h>

namespace IMF = OPENEXR_IMF_NAMESPACE;

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* Map a codec name (from the tinyexr canonical list) to an OpenEXR enum. */
static bool imf_for(const std::string &name, IMF::Compression *out) {
    if (name == "none")     { *out = IMF::NO_COMPRESSION;     return true; }
    if (name == "rle")      { *out = IMF::RLE_COMPRESSION;    return true; }
    if (name == "zips")     { *out = IMF::ZIPS_COMPRESSION;   return true; }
    if (name == "zip")      { *out = IMF::ZIP_COMPRESSION;    return true; }
    if (name == "piz")      { *out = IMF::PIZ_COMPRESSION;    return true; }
    if (name == "pxr24")    { *out = IMF::PXR24_COMPRESSION;  return true; }
    if (name == "b44")      { *out = IMF::B44_COMPRESSION;    return true; }
    if (name == "htj2k256") { *out = IMF::HTJ2K256_COMPRESSION; return true; }
    if (name == "htj2k32")  { *out = IMF::HTJ2K32_COMPRESSION;  return true; }
    return false;
}

/* ---- OpenEXR side --------------------------------------------------------- */

struct oexr_result {
    double enc_mpix, dec_mpix;
    size_t size;
    bool ok;
};

static size_t pixsize(IMF::PixelType t) {
    return (t == IMF::HALF) ? 2 : 4; /* HALF=2, FLOAT/UINT=4 */
}

struct exr_src {
    IMF::Header header;
    int width, height;
    std::vector<std::string> names;
    std::vector<IMF::PixelType> types;
    std::vector<int> xs, ys; /* subsampling */
    std::vector<std::vector<char> > data;
    bool ok;
    exr_src() : header(1, 1), width(0), height(0), ok(false) {}
};

static IMF::FrameBuffer make_fb(exr_src &s,
                                std::vector<std::vector<char> > &store) {
    IMF::FrameBuffer fb;
    IMATH_NAMESPACE::Box2i dw = s.header.dataWindow();
    store.resize(s.names.size());
    for (size_t c = 0; c < s.names.size(); ++c) {
        size_t w = (size_t)(s.width / s.xs[c]);
        size_t h = (size_t)(s.height / s.ys[c]);
        store[c].assign(w * h * pixsize(s.types[c]), 0);
        fb.insert(s.names[c], IMF::Slice::Make(s.types[c], store[c].data(), dw,
                                               0, 0, s.xs[c], s.ys[c]));
    }
    return fb;
}

static exr_src exr_load_src(const char *path) {
    exr_src s;
    try {
        IMF::InputFile in(path);
        s.header = in.header();
        IMATH_NAMESPACE::Box2i dw = s.header.dataWindow();
        s.width = dw.max.x - dw.min.x + 1;
        s.height = dw.max.y - dw.min.y + 1;
        const IMF::ChannelList &ch = s.header.channels();
        for (IMF::ChannelList::ConstIterator it = ch.begin(); it != ch.end();
             ++it) {
            s.names.push_back(it.name());
            s.types.push_back(it.channel().type);
            s.xs.push_back(it.channel().xSampling);
            s.ys.push_back(it.channel().ySampling);
        }
        IMF::FrameBuffer fb = make_fb(s, s.data);
        in.setFrameBuffer(fb);
        in.readPixels(dw.min.y, dw.max.y);
        s.ok = true;
    } catch (const std::exception &e) {
        fprintf(stderr, "  (OpenEXR could not load %s: %s)\n", path, e.what());
    }
    return s;
}

static IMF::FrameBuffer write_fb(exr_src &s) {
    IMF::FrameBuffer fb;
    IMATH_NAMESPACE::Box2i dw = s.header.dataWindow();
    for (size_t c = 0; c < s.names.size(); ++c)
        fb.insert(s.names[c], IMF::Slice::Make(s.types[c], s.data[c].data(), dw,
                                               0, 0, s.xs[c], s.ys[c]));
    return fb;
}

static oexr_result exr_bench(exr_src &s, IMF::Compression comp, double mpix) {
    oexr_result r;
    r.enc_mpix = r.dec_mpix = 0;
    r.size = 0;
    r.ok = false;

    std::string buf;
    try { /* reference encode: capture size + validate support */
        IMF::Header h = s.header;
        h.compression() = comp;
        IMF::StdOSStream oss;
        IMF::OutputFile out(oss, h);
        out.setFrameBuffer(write_fb(s));
        out.writePixels(s.height);
        buf = oss.str();
    } catch (const std::exception &) {
        return r;
    }
    r.size = buf.size();

    if (!std::getenv("EXR_BENCH_DECODE_ONLY")) try { /* encode timing */
        double t0 = now_sec();
        long it = 0;
        do {
            IMF::Header h = s.header;
            h.compression() = comp;
            IMF::StdOSStream oss;
            IMF::OutputFile out(oss, h);
            out.setFrameBuffer(write_fb(s));
            out.writePixels(s.height);
            (void)oss.str();
            ++it;
        } while (now_sec() - t0 < 0.3);
        r.enc_mpix = mpix / ((now_sec() - t0) / (double)it);
    } catch (const std::exception &) {
        return r;
    }

    if (!std::getenv("EXR_BENCH_ENCODE_ONLY")) try { /* decode timing */
        std::vector<std::vector<char> > store;
        IMATH_NAMESPACE::Box2i dw = s.header.dataWindow();
        double t0 = now_sec();
        long it = 0;
        do {
            IMF::StdISStream iss;
            iss.str(buf);
            IMF::InputFile in(iss);
            in.setFrameBuffer(make_fb(s, store));
            in.readPixels(dw.min.y, dw.max.y);
            ++it;
        } while (now_sec() - t0 < 0.3);
        r.dec_mpix = mpix / ((now_sec() - t0) / (double)it);
    } catch (const std::exception &) {
        return r;
    }
    r.ok = true;
    return r;
}

/* ---- driver --------------------------------------------------------------- */

static void compare(const char *path) {
    int txw = 0, txh = 0;
    const bool tx_only = std::getenv("EXR_BENCH_TINY_ONLY") != nullptr;
    const bool exr_only = std::getenv("EXR_BENCH_OPENEXR_ONLY") != nullptr;
    bool tx_ok = !exr_only && bench_tx_load(path, &txw, &txh) != 0;
    if (!tx_ok) printf("  (tinyexr could not load %s)\n", path);

    exr_src es;
    if (tx_only) es.ok = false;
    else es = exr_load_src(path);

    int w = tx_ok ? txw : es.width;
    int h = tx_ok ? txh : es.height;
    double mpix = (double)w * (double)h / 1e6;

    printf("\n== tinyexr vs OpenEXR: %s (%dx%d) ==\n", path, w, h);
    printf("  %-9s | %9s %9s | %9s %9s | %9s %9s | %9s\n", "codec", "tx enc",
           "exr enc", "tx dec", "exr dec", "tx KB", "exr KB", "tx peak");
    printf("  %-9s | %9s %9s | %9s %9s | %9s %9s | %9s\n", "", "MP/s", "MP/s", "MP/s",
           "MP/s", "", "", "KB");

    int n = bench_tx_codec_count();
    const bool htj2k_only = std::getenv("EXR_BENCH_HTJ2K_ONLY") != nullptr;
    const char *codec_only = std::getenv("EXR_BENCH_CODEC");
    for (int i = 0; i < n; ++i) {
        const char *name = bench_tx_codec_name(i);
        if (htj2k_only && std::strncmp(name, "htj2k", 5) != 0) continue;
        if (codec_only && std::strcmp(name, codec_only) != 0) continue;
        bench_tx_result tr;
        tr.ok = 0;
        oexr_result er;
        er.ok = false;

        if (tx_ok) tr = bench_tx_run(i, mpix);
        IMF::Compression comp;
        if (es.ok && imf_for(name, &comp)) er = exr_bench(es, comp, mpix);

        printf("  %-9s | ", name);
        if (tr.ok) printf("%9.1f ", tr.enc_mpix); else printf("%9s ", "-");
        if (er.ok) printf("%9.1f ", er.enc_mpix); else printf("%9s ", "-");
        printf("| ");
        if (tr.ok) printf("%9.1f ", tr.dec_mpix); else printf("%9s ", "-");
        if (er.ok) printf("%9.1f ", er.dec_mpix); else printf("%9s ", "-");
        printf("| ");
        if (tr.ok) printf("%9.0f ", tr.size / 1024.0); else printf("%9s ", "-");
        if (er.ok) printf("%9.0f ", er.size / 1024.0); else printf("%9s ", "-");
        if (tr.ok) printf("| %9.0f ", tr.peak_bytes / 1024.0);
        else printf("| %9s ", "-");
        printf("\n");
    }

    if (tx_ok) bench_tx_unload();
}

int main(int argc, char **argv) {
    /* OpenEXR thread count: 0 = single-threaded (calling thread only), which is
     * the default for parity with tinyexr (tinyexr is single-threaded). Set
     * EXR_THREADS=N to let OpenEXR use a worker pool for contrast. */
    int threads = 0;
    {
        const char *e = getenv("EXR_THREADS");
        if (e) threads = atoi(e);
        if (threads < 0) threads = 0;
    }
    IMF::setGlobalThreadCount(threads);
    /* Match tinyexr's worker count to OpenEXR's (no-op unless built THREADS=1).
     * 0 -> single-threaded on both sides. */
    bench_tx_set_threads(threads);

    printf("TinyEXR v3 vs OpenEXR benchmark  |  tinyexr SIMD: %s\n",
           bench_tx_simd_info());
    printf("worker threads: %d (both libraries; EXR_THREADS=N to change; "
           "tinyexr needs a THREADS=1 build to use them)\n",
           threads);
    printf("(in-memory; each library loads the same source independently)\n");

    if (argc > 1) {
        for (int i = 1; i < argc; ++i) compare(argv[i]);
    } else {
        compare("asakusa.exr");
    }
    return 0;
}
