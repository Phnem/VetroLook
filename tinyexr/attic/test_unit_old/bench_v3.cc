/*
 * TinyEXR V3 Decompression Benchmark
 *
 * Loads various EXR files with different compression types and measures
 * decompression time. Designed for profiling with samply/perf.
 *
 * Build: make bench-v3
 * Profile: samply record ./bench-v3
 * Or: perf record -g ./bench-v3 && perf report
 */

#define TINYEXR_V3_ENABLE_PIZ 1
#define TINYEXR_V3_ENABLE_PXR24 1
#define TINYEXR_V3_ENABLE_B44 1

#include "../../tinyexr_v3.hh"
#include "../../tinyexr_c.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <chrono>

/* ============================================================================
 * File I/O
 * ============================================================================ */

static std::vector<uint8_t> read_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return {};
    fseek(f, 0, SEEK_END);
    size_t size = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> data(size);
    size_t nread = fread(data.data(), 1, size, f);
    fclose(f);
    if (nread != size) return {};
    return data;
}

/* ============================================================================
 * V3 Decode Benchmark
 * ============================================================================ */

static const char* compression_name(uint32_t c) {
    switch (c) {
        case 0: return "NONE";
        case 1: return "RLE";
        case 2: return "ZIPS";
        case 3: return "ZIP";
        case 4: return "PIZ";
        case 5: return "PXR24";
        case 6: return "B44";
        case 7: return "B44A";
        default: return "???";
    }
}

struct BenchResult {
    const char* file;
    const char* comp;
    const char* layout;  /* scanline / tiled */
    int width, height, channels;
    double avg_ms;
    int iterations;
    bool ok;
};

/* Decode a file once through V3 API. Returns true on success. */
static bool decode_v3_once(const std::vector<uint8_t>& fileData,
                           ExrPartInfo* out_info = nullptr) {
    ExrContextCreateInfo ctx_info = {};
    ctx_info.api_version = TINYEXR_C_API_VERSION;
    ExrContext ctx = nullptr;
    if (exr_context_create(&ctx_info, &ctx) != EXR_SUCCESS) return false;

    ExrDataSource source = {};
    if (exr_data_source_from_memory(fileData.data(), fileData.size(), &source) != EXR_SUCCESS) {
        exr_context_destroy(ctx);
        return false;
    }

    ExrDecoderCreateInfo dec_info = {};
    dec_info.source = source;
    ExrDecoder decoder = nullptr;
    if (exr_decoder_create(ctx, &dec_info, &decoder) != EXR_SUCCESS) {
        exr_context_destroy(ctx);
        return false;
    }

    ExrImage image = nullptr;
    if (exr_decoder_parse_header(decoder, &image) != EXR_SUCCESS) {
        exr_decoder_destroy(decoder);
        exr_context_destroy(ctx);
        return false;
    }

    ExrPart part = nullptr;
    if (exr_image_get_part(image, 0, &part) != EXR_SUCCESS) {
        exr_decoder_destroy(decoder);
        exr_context_destroy(ctx);
        return false;
    }

    ExrPartInfo pinfo = {};
    exr_part_get_info(part, &pinfo);
    if (out_info) *out_info = pinfo;

    ExrChannelInfo ch = {};
    exr_part_get_channel(part, 0, &ch);
    int bpp = (ch.pixel_type == EXR_PIXEL_HALF) ? 2 : 4;

    size_t buf_size = (size_t)pinfo.width * pinfo.height * pinfo.num_channels * bpp;
    std::vector<uint8_t> buffer(buf_size, 0);

    ExrCommandBufferCreateInfo cmd_info = {};
    cmd_info.decoder = decoder;
    ExrCommandBuffer cmd = nullptr;
    if (exr_command_buffer_create(ctx, &cmd_info, &cmd) != EXR_SUCCESS) {
        exr_part_destroy(part);
        exr_decoder_destroy(decoder);
        exr_context_destroy(ctx);
        return false;
    }

    exr_command_buffer_begin(cmd);

    ExrFullImageRequest req = {};
    req.part = part;
    req.output.data = buffer.data();
    req.output.size = buf_size;
    req.output_pixel_type = ch.pixel_type;

    if (exr_cmd_request_full_image(cmd, &req) != EXR_SUCCESS) {
        exr_command_buffer_destroy(cmd);
        exr_part_destroy(part);
        exr_decoder_destroy(decoder);
        exr_context_destroy(ctx);
        return false;
    }

    exr_command_buffer_end(cmd);

    ExrSubmitInfo submit = {};
    ExrCommandBuffer cmds[] = { cmd };
    submit.command_buffer_count = 1;
    submit.command_buffers = cmds;

    ExrResult result = exr_submit(decoder, &submit);

    exr_command_buffer_destroy(cmd);
    exr_part_destroy(part);
    exr_decoder_destroy(decoder);
    exr_context_destroy(ctx);

    return result == EXR_SUCCESS;
}

static BenchResult bench_file(const char* path, int iterations) {
    BenchResult r = {};
    r.file = path;
    r.iterations = iterations;

    std::vector<uint8_t> fileData = read_file(path);
    if (fileData.empty()) {
        r.ok = false;
        r.comp = "???";
        r.layout = "???";
        return r;
    }

    /* First decode to get info */
    ExrPartInfo pinfo = {};
    if (!decode_v3_once(fileData, &pinfo)) {
        r.ok = false;
        r.comp = compression_name(pinfo.compression);
        r.layout = (pinfo.part_type == EXR_PART_TILED) ? "tiled" : "scanline";
        r.width = pinfo.width;
        r.height = pinfo.height;
        r.channels = pinfo.num_channels;
        return r;
    }

    r.comp = compression_name(pinfo.compression);
    r.layout = (pinfo.part_type == EXR_PART_TILED) ? "tiled" : "scanline";
    r.width = pinfo.width;
    r.height = pinfo.height;
    r.channels = pinfo.num_channels;

    /* Warm up */
    decode_v3_once(fileData);

    /* Timed iterations */
    auto t0 = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < iterations; i++) {
        decode_v3_once(fileData);
    }
    auto t1 = std::chrono::high_resolution_clock::now();

    double total_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    r.avg_ms = total_ms / iterations;
    r.ok = true;
    return r;
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(int argc, char** argv) {
    int iterations = 10;  /* default */
    bool profile_mode = false;

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            iterations = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--profile") == 0) {
            profile_mode = true;
            iterations = 50;  /* More iterations for profiling */
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [-n iterations] [--profile]\n", argv[0]);
            printf("  -n N        Number of iterations per file (default: 10)\n");
            printf("  --profile   Profile mode: 50 iterations, no output during run\n");
            return 0;
        }
    }

    struct TestFile {
        const char* path;
        const char* description;
    };

    TestFile files[] = {
        /* ZIP scanline */
        { "../../../openexr-images/ScanLines/Blobbies.exr",        "ZIP scanline (5.9MB)" },
        { "../../../openexr-images/ScanLines/Carrots.exr",         "ZIP scanline (894K)" },
        { "../../../openexr-images/TestImages/BrightRings.exr",    "ZIP scanline BrightRings" },

        /* ZIP tiled */
        { "../../../openexr-images/Tiles/Ocean.exr",               "ZIP tiled (4.5MB)" },
        { "../../../openexr-images/MultiResolution/Kapaa.exr",     "ZIP tiled mipmap (6.5MB)" },

        /* PIZ scanline */
        { "../../../openexr-images/ScanLines/Desk.exr",            "PIZ scanline (2.4MB)" },
        { "../../../openexr-images/ScanLines/StillLife.exr",       "PIZ scanline (3.7MB)" },
        { "../../../openexr-images/ScanLines/MtTamWest.exr",       "PIZ scanline (3.2MB)" },

        /* PIZ tiled */
        { "../../../openexr-images/Tiles/GoldenGate.exr",          "PIZ tiled (3.6MB)" },

        /* PXR24 scanline */
        { "../../../openexr-images/TestImages/SquaresSwirls.exr",  "PXR24 scanline (428K)" },

        /* PXR24 tiled */
        { "../../../openexr-images/Tiles/Spirals.exr",             "PXR24 tiled (5.2MB)" },

        /* B44 scanline */
        { "../../../openexr-images/ScanLines/Cannon.exr",          "B44 scanline (1.2MB)" },
        { "../../../openexr-images/LuminanceChroma/CrissyField.exr", "B44 scanline (1.3MB)" },

        /* Also the standard test file */
        { "../../asakusa.exr",                                     "ZIP scanline asakusa" },
    };
    int nfiles = sizeof(files) / sizeof(files[0]);

    if (!profile_mode) {
        printf("TinyEXR V3 Decompression Benchmark\n");
        printf("===================================\n");
        printf("Iterations per file: %d\n\n", iterations);
        printf("%-8s %-9s %6s %5s %3s  %9s  %s\n",
               "Comp", "Layout", "W", "H", "Ch", "Avg(ms)", "File");
        printf("-------- --------- ------ ----- ---  ---------  ----\n");
    }

    for (int i = 0; i < nfiles; i++) {
        BenchResult r = bench_file(files[i].path, iterations);
        if (!r.ok) {
            if (!profile_mode) {
                printf("%-8s %-9s %6d %5d %3d  %9s  %s\n",
                       r.comp, r.layout, r.width, r.height, r.channels,
                       "FAILED", files[i].description);
            }
        } else {
            if (!profile_mode) {
                printf("%-8s %-9s %6d %5d %3d  %8.2fms  %s\n",
                       r.comp, r.layout, r.width, r.height, r.channels,
                       r.avg_ms, files[i].description);
            }
        }
    }

    if (!profile_mode) {
        printf("\nDone.\n");
    }

    return 0;
}
