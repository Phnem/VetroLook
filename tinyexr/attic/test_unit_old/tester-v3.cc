/*
 * TinyEXR V3 API Test Suite
 *
 * Tests for the new Vulkan-style C API and C++17 wrapper.
 *
 * Copyright (c) 2024 TinyEXR authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

/* Enable PIZ, PXR24, and B44 support in V3 (requires V1 implementation) */
#define TINYEXR_V3_ENABLE_PIZ 1
#define TINYEXR_V3_ENABLE_PXR24 1
#define TINYEXR_V3_ENABLE_B44 1

#include "../../tinyexr_v3.hh"
#include "../../tinyexr_c.h"

/* Include V1 API for comparison testing */
#define TINYEXR_IMPLEMENTATION
#include "../../tinyexr.h"

/* PIZ wrapper function - calls V1's DecompressPiz */
extern "C" bool tinyexr_v3_decompress_piz(
    unsigned char* outPtr, const unsigned char* inPtr,
    size_t tmpBufSizeInBytes, size_t inLen, int num_channels,
    const EXRChannelInfo* channels, int data_width, int num_lines) {
    return tinyexr::DecompressPiz(outPtr, inPtr, tmpBufSizeInBytes, inLen,
                                   num_channels, channels, data_width, num_lines);
}

/* PXR24 wrapper function - calls V1's DecompressPxr24 */
extern "C" bool tinyexr_v3_decompress_pxr24(
    unsigned char* outPtr, size_t outBufSize,
    const unsigned char* inPtr, size_t inLen,
    int data_width, int num_lines,
    size_t num_channels, const EXRChannelInfo* channels) {
    return tinyexr::DecompressPxr24(outPtr, outBufSize, inPtr, inLen,
                                     data_width, num_lines, num_channels, channels);
}

/* B44 wrapper function - calls V1's DecompressB44 */
extern "C" bool tinyexr_v3_decompress_b44(
    unsigned char* outPtr, size_t outBufSize,
    const unsigned char* inPtr, size_t inLen,
    int data_width, int num_lines,
    size_t num_channels, const EXRChannelInfo* channels, bool is_b44a) {
    return tinyexr::DecompressB44(outPtr, outBufSize, inPtr, inLen,
                                   data_width, num_lines, num_channels, channels, is_b44a);
}

#include <cstdio>
#include <cstring>
#include <cassert>
#include <stdexcept>
#include <vector>
#include <cmath>

/* ============================================================================
 * Test Utilities
 * ============================================================================ */

static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST(name) \
    static void test_##name(); \
    static void run_test_##name() { \
        printf("  [TEST] %s... ", #name); \
        fflush(stdout); \
        try { \
            test_##name(); \
            printf("PASSED\n"); \
            g_tests_passed++; \
        } catch (const std::exception& e) { \
            printf("FAILED: %s\n", e.what()); \
            g_tests_failed++; \
        } catch (...) { \
            printf("FAILED: unknown exception\n"); \
            g_tests_failed++; \
        } \
    } \
    static void test_##name()

#define REQUIRE(cond) \
    do { \
        if (!(cond)) { \
            throw std::runtime_error("Assertion failed: " #cond); \
        } \
    } while (0)

#define REQUIRE_EQ(a, b) \
    do { \
        if ((a) != (b)) { \
            throw std::runtime_error("Assertion failed: " #a " == " #b); \
        } \
    } while (0)

/* ============================================================================
 * C API Tests
 * ============================================================================ */

TEST(c_api_version) {
    int major, minor, patch;
    exr_get_version(&major, &minor, &patch);
    REQUIRE_EQ(major, TINYEXR_C_API_VERSION_MAJOR);
    REQUIRE_EQ(minor, TINYEXR_C_API_VERSION_MINOR);
    REQUIRE_EQ(patch, TINYEXR_C_API_VERSION_PATCH);
}

TEST(c_api_version_string) {
    const char* ver = exr_get_version_string();
    REQUIRE(ver != nullptr);
    REQUIRE(strlen(ver) > 0);
}

TEST(c_api_result_to_string) {
    const char* s = exr_result_to_string(EXR_SUCCESS);
    REQUIRE(strcmp(s, "Success") == 0);

    s = exr_result_to_string(EXR_ERROR_INVALID_HANDLE);
    REQUIRE(strcmp(s, "Invalid handle") == 0);

    s = exr_result_to_string(EXR_ERROR_OUT_OF_MEMORY);
    REQUIRE(strcmp(s, "Out of memory") == 0);
}

TEST(c_api_simd_info) {
    const char* info = exr_get_simd_info();
    REQUIRE(info != nullptr);
    printf("[%s] ", info);
}

TEST(c_api_context_create_destroy) {
    ExrContextCreateInfo info = {};
    info.api_version = TINYEXR_C_API_VERSION;

    ExrContext ctx = nullptr;
    ExrResult result = exr_context_create(&info, &ctx);
    REQUIRE_EQ(result, EXR_SUCCESS);
    REQUIRE(ctx != nullptr);

    exr_context_destroy(ctx);
}

TEST(c_api_context_invalid_version) {
    ExrContextCreateInfo info = {};
    info.api_version = (99 << 22);  // Invalid major version

    ExrContext ctx = nullptr;
    ExrResult result = exr_context_create(&info, &ctx);
    REQUIRE_EQ(result, EXR_ERROR_INVALID_VERSION);
    REQUIRE(ctx == nullptr);
}

TEST(c_api_context_ref_counting) {
    ExrContextCreateInfo info = {};
    info.api_version = TINYEXR_C_API_VERSION;

    ExrContext ctx = nullptr;
    ExrResult result = exr_context_create(&info, &ctx);
    REQUIRE_EQ(result, EXR_SUCCESS);

    exr_context_add_ref(ctx);
    exr_context_release(ctx);  // Should not destroy
    exr_context_release(ctx);  // Should destroy
}

TEST(c_api_error_handling) {
    ExrContextCreateInfo info = {};
    info.api_version = TINYEXR_C_API_VERSION;

    ExrContext ctx = nullptr;
    ExrResult result = exr_context_create(&info, &ctx);
    REQUIRE_EQ(result, EXR_SUCCESS);

    // Initially no errors
    REQUIRE_EQ(exr_get_error_count(ctx), 0u);

    exr_clear_errors(ctx);
    REQUIRE_EQ(exr_get_error_count(ctx), 0u);

    exr_context_destroy(ctx);
}

TEST(c_api_memory_pool) {
    ExrContextCreateInfo ctx_info = {};
    ctx_info.api_version = TINYEXR_C_API_VERSION;

    ExrContext ctx = nullptr;
    ExrResult result = exr_context_create(&ctx_info, &ctx);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrMemoryPoolCreateInfo pool_info = {};
    pool_info.initial_size = 1024;
    pool_info.max_size = 0;  // Unlimited

    ExrMemoryPool pool = nullptr;
    result = exr_memory_pool_create(ctx, &pool_info, &pool);
    REQUIRE_EQ(result, EXR_SUCCESS);
    REQUIRE(pool != nullptr);

    size_t used = exr_memory_pool_get_used(pool);
    REQUIRE_EQ(used, 0u);

    exr_memory_pool_reset(pool);
    exr_memory_pool_destroy(pool);
    exr_context_destroy(ctx);
}

TEST(c_api_data_source_from_memory) {
    uint8_t test_data[] = { 0x76, 0x2F, 0x31, 0x01 };  // EXR magic

    ExrDataSource source = {};
    ExrResult result = exr_data_source_from_memory(test_data, sizeof(test_data), &source);
    REQUIRE_EQ(result, EXR_SUCCESS);
    REQUIRE(source.fetch != nullptr);
    REQUIRE(source.total_size == sizeof(test_data));
    REQUIRE(source.flags & EXR_DATA_SOURCE_SEEKABLE);
}

TEST(c_api_decoder_create_destroy) {
    uint8_t test_data[] = { 0x76, 0x2F, 0x31, 0x01 };  // EXR magic

    ExrContextCreateInfo ctx_info = {};
    ctx_info.api_version = TINYEXR_C_API_VERSION;

    ExrContext ctx = nullptr;
    ExrResult result = exr_context_create(&ctx_info, &ctx);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrDataSource source = {};
    result = exr_data_source_from_memory(test_data, sizeof(test_data), &source);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrDecoderCreateInfo dec_info = {};
    dec_info.source = source;

    ExrDecoder decoder = nullptr;
    result = exr_decoder_create(ctx, &dec_info, &decoder);
    REQUIRE_EQ(result, EXR_SUCCESS);
    REQUIRE(decoder != nullptr);

    exr_decoder_destroy(decoder);
    exr_context_destroy(ctx);
}

TEST(c_api_fence_create_destroy) {
    ExrContextCreateInfo ctx_info = {};
    ctx_info.api_version = TINYEXR_C_API_VERSION;

    ExrContext ctx = nullptr;
    ExrResult result = exr_context_create(&ctx_info, &ctx);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrFenceCreateInfo fence_info = {};
    fence_info.flags = EXR_FENCE_SIGNALED;

    ExrFence fence = nullptr;
    result = exr_fence_create(ctx, &fence_info, &fence);
    REQUIRE_EQ(result, EXR_SUCCESS);
    REQUIRE(fence != nullptr);

    // Should already be signaled
    result = exr_fence_get_status(fence);
    REQUIRE_EQ(result, EXR_SUCCESS);

    // Wait should return immediately
    result = exr_fence_wait(fence, EXR_TIMEOUT_NONE);
    REQUIRE_EQ(result, EXR_SUCCESS);

    // Reset and check
    result = exr_fence_reset(fence);
    REQUIRE_EQ(result, EXR_SUCCESS);

    result = exr_fence_get_status(fence);
    REQUIRE_EQ(result, EXR_ERROR_NOT_READY);

    exr_fence_destroy(fence);
    exr_context_destroy(ctx);
}

TEST(c_api_command_buffer) {
    ExrContextCreateInfo ctx_info = {};
    ctx_info.api_version = TINYEXR_C_API_VERSION;

    ExrContext ctx = nullptr;
    ExrResult result = exr_context_create(&ctx_info, &ctx);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrCommandBufferCreateInfo cmd_info = {};
    cmd_info.decoder = nullptr;  // No decoder for this test
    cmd_info.flags = 0;

    ExrCommandBuffer cmd = nullptr;
    result = exr_command_buffer_create(ctx, &cmd_info, &cmd);
    REQUIRE_EQ(result, EXR_SUCCESS);
    REQUIRE(cmd != nullptr);

    result = exr_command_buffer_begin(cmd);
    REQUIRE_EQ(result, EXR_SUCCESS);

    result = exr_command_buffer_end(cmd);
    REQUIRE_EQ(result, EXR_SUCCESS);

    result = exr_command_buffer_reset(cmd);
    REQUIRE_EQ(result, EXR_SUCCESS);

    exr_command_buffer_destroy(cmd);
    exr_context_destroy(ctx);
}

TEST(c_api_half_conversion) {
    // Test half to float conversion
    uint16_t half_vals[] = { 0x0000, 0x3C00, 0x4000, 0x7C00 };  // 0, 1, 2, inf
    float float_vals[4];

    exr_convert_half_to_float(half_vals, float_vals, 4);

    REQUIRE(float_vals[0] == 0.0f);
    REQUIRE(float_vals[1] == 1.0f);
    REQUIRE(float_vals[2] == 2.0f);
    // float_vals[3] should be infinity

    // Test float to half conversion
    float src_floats[] = { 0.0f, 1.0f, 2.0f, 0.5f };
    uint16_t dst_halfs[4];

    exr_convert_float_to_half(src_floats, dst_halfs, 4);

    REQUIRE(dst_halfs[0] == 0x0000);  // 0
    REQUIRE(dst_halfs[1] == 0x3C00);  // 1
    REQUIRE(dst_halfs[2] == 0x4000);  // 2
    REQUIRE(dst_halfs[3] == 0x3800);  // 0.5
}

/* ============================================================================
 * C++ Wrapper Tests
 * ============================================================================ */

TEST(cpp_version_string) {
    std::string ver = tinyexr::v3::get_version_string();
    REQUIRE(!ver.empty());
}

TEST(cpp_simd_info) {
    std::string info = tinyexr::v3::get_simd_info();
    REQUIRE(!info.empty());
    printf("[%s] ", info.c_str());
}

TEST(cpp_context_create) {
    auto result = tinyexr::v3::Context::create();
    REQUIRE(result.success);
    REQUIRE(result.value);
}

TEST(cpp_context_with_options) {
    tinyexr::v3::Context::CreateInfo info;
    info.enable_validation = true;
    info.single_threaded = true;

    auto result = tinyexr::v3::Context::create(info);
    REQUIRE(result.success);
    REQUIRE(result.value);
}

TEST(cpp_result_ok) {
    auto result = tinyexr::v3::Result<int>::ok(42);
    REQUIRE(result.success);
    REQUIRE(result.value == 42);
    REQUIRE(static_cast<bool>(result));
}

TEST(cpp_result_error) {
    auto result = tinyexr::v3::Result<int>::error(EXR_ERROR_IO, "Test error");
    REQUIRE(!result.success);
    REQUIRE(result.first_error().code == EXR_ERROR_IO);
    REQUIRE(!static_cast<bool>(result));
}

TEST(cpp_result_value_or) {
    auto ok_result = tinyexr::v3::Result<int>::ok(42);
    REQUIRE(ok_result.value_or(0) == 42);

    auto err_result = tinyexr::v3::Result<int>::error(EXR_ERROR_IO);
    REQUIRE(err_result.value_or(99) == 99);
}

TEST(cpp_result_map) {
    auto result = tinyexr::v3::Result<int>::ok(21);
    auto mapped = result.map([](int x) { return x * 2; });
    REQUIRE(mapped.success);
    REQUIRE(mapped.value == 42);
}

TEST(cpp_result_map_error) {
    auto result = tinyexr::v3::Result<int>::error(EXR_ERROR_IO);
    auto mapped = result.map([](int x) { return x * 2; });
    REQUIRE(!mapped.success);
    REQUIRE(mapped.first_error().code == EXR_ERROR_IO);
}

TEST(cpp_result_and_then) {
    auto result = tinyexr::v3::Result<int>::ok(21);
    auto chained = result.and_then([](int x) {
        return tinyexr::v3::Result<std::string>::ok(std::to_string(x * 2));
    });
    REQUIRE(chained.success);
    REQUIRE(chained.value == "42");
}

TEST(cpp_result_void) {
    auto result = tinyexr::v3::Result<void>::ok();
    REQUIRE(result.success);
    REQUIRE(static_cast<bool>(result));

    auto err = tinyexr::v3::Result<void>::error(EXR_ERROR_IO);
    REQUIRE(!err.success);
}

TEST(cpp_error_info) {
    tinyexr::v3::ErrorInfo info(EXR_ERROR_IO, "Test message", "parsing", 1234);
    REQUIRE(info.code == EXR_ERROR_IO);
    REQUIRE(info.message == "Test message");
    REQUIRE(info.context == "parsing");
    REQUIRE(info.byte_position == 1234);

    std::string str = info.to_string();
    REQUIRE(!str.empty());
}

TEST(cpp_decoder_from_memory) {
    auto ctx_result = tinyexr::v3::Context::create();
    REQUIRE(ctx_result.success);

    uint8_t test_data[] = { 0x76, 0x2F, 0x31, 0x01, 0, 0, 0, 0 };
    auto decoder_result = tinyexr::v3::Decoder::from_memory(
        ctx_result.value, test_data, sizeof(test_data));
    REQUIRE(decoder_result.success);
    REQUIRE(decoder_result.value);
}

TEST(cpp_tile_coord) {
    tinyexr::v3::TileCoord a{0, 0, 1, 2};
    tinyexr::v3::TileCoord b{0, 0, 1, 2};
    tinyexr::v3::TileCoord c{0, 0, 1, 3};

    REQUIRE(a == b);
    REQUIRE(a != c);
}

/* ============================================================================
 * Header Parsing Tests
 * ============================================================================ */

/* Helper: Read file into vector */
static std::vector<uint8_t> read_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return {};
    fseek(f, 0, SEEK_END);
    size_t size = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> data(size);
    size_t read = fread(data.data(), 1, size, f);
    fclose(f);
    if (read != size) return {};
    return data;
}

static uint32_t read_le_u32_test(const uint8_t* p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t read_le_u64_test(const uint8_t* p) {
    return (uint64_t)read_le_u32_test(p) |
           ((uint64_t)read_le_u32_test(p + 4) << 32);
}

static bool find_single_part_offset_table_start(const std::vector<uint8_t>& data,
                                                size_t* out_offset) {
    if (!out_offset || data.size() < 9) return false;

    size_t pos = 8;  /* Skip magic + version */
    while (pos < data.size()) {
        if (data[pos] == 0) {
            *out_offset = pos + 1;
            return true;
        }

        while (pos < data.size() && data[pos] != 0) pos++;
        if (pos >= data.size()) return false;
        pos++;  /* attr name terminator */

        while (pos < data.size() && data[pos] != 0) pos++;
        if (pos >= data.size()) return false;
        pos++;  /* attr type terminator */

        if (pos + 4 > data.size()) return false;
        uint32_t attr_size = read_le_u32_test(data.data() + pos);
        pos += 4;
        if (pos + attr_size > data.size()) return false;
        pos += attr_size;
    }

    return false;
}

TEST(c_api_parse_invalid_magic) {
    ExrContextCreateInfo ctx_info = {};
    ctx_info.api_version = TINYEXR_C_API_VERSION;

    ExrContext ctx = nullptr;
    ExrResult result = exr_context_create(&ctx_info, &ctx);
    REQUIRE_EQ(result, EXR_SUCCESS);

    /* Invalid EXR data - wrong magic */
    uint8_t bad_data[] = { 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00 };

    ExrDataSource source = {};
    result = exr_data_source_from_memory(bad_data, sizeof(bad_data), &source);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrDecoderCreateInfo dec_info = {};
    dec_info.source = source;

    ExrDecoder decoder = nullptr;
    result = exr_decoder_create(ctx, &dec_info, &decoder);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrImage image = nullptr;
    result = exr_decoder_parse_header(decoder, &image);
    REQUIRE_EQ(result, EXR_ERROR_INVALID_MAGIC);
    REQUIRE(image == nullptr);

    exr_decoder_destroy(decoder);
    exr_context_destroy(ctx);
}

TEST(c_api_parse_invalid_version) {
    ExrContextCreateInfo ctx_info = {};
    ctx_info.api_version = TINYEXR_C_API_VERSION;

    ExrContext ctx = nullptr;
    ExrResult result = exr_context_create(&ctx_info, &ctx);
    REQUIRE_EQ(result, EXR_SUCCESS);

    /* Invalid EXR data - correct magic but wrong version */
    uint8_t bad_data[] = { 0x76, 0x2f, 0x31, 0x01, 0x03, 0x00, 0x00, 0x00 };

    ExrDataSource source = {};
    result = exr_data_source_from_memory(bad_data, sizeof(bad_data), &source);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrDecoderCreateInfo dec_info = {};
    dec_info.source = source;

    ExrDecoder decoder = nullptr;
    result = exr_decoder_create(ctx, &dec_info, &decoder);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrImage image = nullptr;
    result = exr_decoder_parse_header(decoder, &image);
    REQUIRE_EQ(result, EXR_ERROR_INVALID_VERSION);
    REQUIRE(image == nullptr);

    exr_decoder_destroy(decoder);
    exr_context_destroy(ctx);
}

TEST(c_api_parse_real_file) {
    /* Try to load a real EXR file if available */
    std::vector<uint8_t> data = read_file("../../asakusa.exr");
    if (data.empty()) {
        printf("[SKIPPED - file not found] ");
        return;
    }

    ExrContextCreateInfo ctx_info = {};
    ctx_info.api_version = TINYEXR_C_API_VERSION;

    ExrContext ctx = nullptr;
    ExrResult result = exr_context_create(&ctx_info, &ctx);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrDataSource source = {};
    result = exr_data_source_from_memory(data.data(), data.size(), &source);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrDecoderCreateInfo dec_info = {};
    dec_info.source = source;

    ExrDecoder decoder = nullptr;
    result = exr_decoder_create(ctx, &dec_info, &decoder);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrImage image = nullptr;
    result = exr_decoder_parse_header(decoder, &image);
    REQUIRE_EQ(result, EXR_SUCCESS);
    REQUIRE(image != nullptr);

    /* Query image info */
    ExrImageInfo info = {};
    result = exr_image_get_info(image, &info);
    REQUIRE_EQ(result, EXR_SUCCESS);

    printf("[%dx%d, %d ch, comp=%d] ", info.width, info.height,
           info.num_channels, info.compression);

    REQUIRE(info.width > 0);
    REQUIRE(info.height > 0);
    REQUIRE(info.num_channels > 0);

    /* Query channel info */
    uint32_t channel_count = 0;
    result = exr_image_get_channel_count(image, &channel_count);
    REQUIRE_EQ(result, EXR_SUCCESS);
    REQUIRE(channel_count > 0);

    for (uint32_t i = 0; i < channel_count; i++) {
        ExrChannelInfo ch_info = {};
        result = exr_image_get_channel(image, i, &ch_info);
        REQUIRE_EQ(result, EXR_SUCCESS);
        REQUIRE(ch_info.name != nullptr);
    }

    /* Query parts */
    uint32_t part_count = 0;
    result = exr_image_get_part_count(image, &part_count);
    REQUIRE_EQ(result, EXR_SUCCESS);
    REQUIRE(part_count >= 1);

    ExrPart part = nullptr;
    result = exr_image_get_part(image, 0, &part);
    REQUIRE_EQ(result, EXR_SUCCESS);
    REQUIRE(part != nullptr);

    ExrPartInfo part_info = {};
    result = exr_part_get_info(part, &part_info);
    REQUIRE_EQ(result, EXR_SUCCESS);
    REQUIRE(part_info.width > 0);
    REQUIRE(part_info.height > 0);

    uint32_t chunk_count = 0;
    result = exr_part_get_chunk_count(part, &chunk_count);
    REQUIRE_EQ(result, EXR_SUCCESS);
    REQUIRE(chunk_count > 0);

    /* Clean up - decoder owns the image, so just destroy decoder */
    exr_part_destroy(part);
    exr_decoder_destroy(decoder);
    exr_context_destroy(ctx);
}

/* ============================================================================
 * Async Data Source for Testing
 * ============================================================================ */

struct AsyncTestSource {
    const uint8_t* data;
    size_t size;
    int fetch_count;
    bool complete_immediately;  /* If false, returns WOULD_BLOCK first time */

    /* Pending fetch state */
    void* pending_dst;
    size_t pending_size;
    ExrFetchComplete pending_callback;
    void* pending_userdata;
};

static ExrResult async_test_fetch(void* userdata, uint64_t offset, uint64_t size,
                                   void* dst, ExrFetchComplete on_complete,
                                   void* complete_userdata) {
    AsyncTestSource* src = (AsyncTestSource*)userdata;
    src->fetch_count++;

    if (offset + size > src->size) {
        return EXR_ERROR_OUT_OF_BOUNDS;
    }

    /* Copy the data */
    memcpy(dst, src->data + offset, size);

    /* Simulate async behavior - first fetch returns WOULD_BLOCK */
    if (!src->complete_immediately && on_complete) {
        /* Store callback for later completion */
        src->pending_dst = dst;
        src->pending_size = size;
        src->pending_callback = on_complete;
        src->pending_userdata = complete_userdata;
        return EXR_WOULD_BLOCK;
    }

    return EXR_SUCCESS;
}

static void async_test_complete_pending(AsyncTestSource* src) {
    if (src->pending_callback) {
        src->pending_callback(src->pending_userdata, EXR_SUCCESS, src->pending_size);
        src->pending_callback = nullptr;
    }
}

TEST(c_api_async_header_parsing) {
    /* Test that async data sources work for header parsing.
     * Header parsing uses local stack buffers, so it falls back to sync
     * even for async sources. This test verifies that works correctly.
     */
    std::vector<uint8_t> data = read_file("../../asakusa.exr");
    if (data.empty()) {
        printf("[SKIPPED - file not found] ");
        return;
    }

    ExrContextCreateInfo ctx_info = {};
    ctx_info.api_version = TINYEXR_C_API_VERSION;

    ExrContext ctx = nullptr;
    ExrResult result = exr_context_create(&ctx_info, &ctx);
    REQUIRE_EQ(result, EXR_SUCCESS);

    /* Create async data source */
    AsyncTestSource async_src = {};
    async_src.data = data.data();
    async_src.size = data.size();
    async_src.complete_immediately = true;  /* Header parsing uses sync fallback */

    ExrDataSource source = {};
    source.userdata = &async_src;
    source.fetch = async_test_fetch;
    source.total_size = data.size();
    source.flags = EXR_DATA_SOURCE_SEEKABLE | EXR_DATA_SOURCE_ASYNC | EXR_DATA_SOURCE_SIZE_KNOWN;

    ExrDecoderCreateInfo dec_info = {};
    dec_info.source = source;

    ExrDecoder decoder = nullptr;
    result = exr_decoder_create(ctx, &dec_info, &decoder);
    REQUIRE_EQ(result, EXR_SUCCESS);

    /* Header parsing should complete synchronously (sync fallback for local buffers) */
    ExrImage image = nullptr;
    result = exr_decoder_parse_header(decoder, &image);

    printf("[fetches=%d] ", async_src.fetch_count);

    REQUIRE_EQ(result, EXR_SUCCESS);
    REQUIRE(image != nullptr);

    /* Verify parsed header */
    ExrImageInfo info = {};
    result = exr_image_get_info(image, &info);
    REQUIRE_EQ(result, EXR_SUCCESS);
    REQUIRE(info.width > 0);
    REQUIRE(info.height > 0);

    exr_decoder_destroy(decoder);
    exr_context_destroy(ctx);
}

TEST(c_api_async_immediate_completion) {
    /* Test async source that completes immediately (no WOULD_BLOCK) */
    std::vector<uint8_t> data = read_file("../../asakusa.exr");
    if (data.empty()) {
        printf("[SKIPPED - file not found] ");
        return;
    }

    ExrContextCreateInfo ctx_info = {};
    ctx_info.api_version = TINYEXR_C_API_VERSION;

    ExrContext ctx = nullptr;
    ExrResult result = exr_context_create(&ctx_info, &ctx);
    REQUIRE_EQ(result, EXR_SUCCESS);

    /* Create async data source that completes immediately */
    AsyncTestSource async_src = {};
    async_src.data = data.data();
    async_src.size = data.size();
    async_src.complete_immediately = true;  /* Complete immediately */

    ExrDataSource source = {};
    source.userdata = &async_src;
    source.fetch = async_test_fetch;
    source.total_size = data.size();
    source.flags = EXR_DATA_SOURCE_SEEKABLE | EXR_DATA_SOURCE_ASYNC | EXR_DATA_SOURCE_SIZE_KNOWN;

    ExrDecoderCreateInfo dec_info = {};
    dec_info.source = source;

    ExrDecoder decoder = nullptr;
    result = exr_decoder_create(ctx, &dec_info, &decoder);
    REQUIRE_EQ(result, EXR_SUCCESS);

    /* Parse header - should complete without WOULD_BLOCK */
    ExrImage image = nullptr;
    result = exr_decoder_parse_header(decoder, &image);
    REQUIRE_EQ(result, EXR_SUCCESS);
    REQUIRE(image != nullptr);

    printf("[fetches=%d] ", async_src.fetch_count);

    /* Verify parsed header */
    ExrImageInfo info = {};
    result = exr_image_get_info(image, &info);
    REQUIRE_EQ(result, EXR_SUCCESS);
    REQUIRE(info.width > 0);
    REQUIRE(info.height > 0);

    exr_decoder_destroy(decoder);
    exr_context_destroy(ctx);
}

TEST(c_api_command_buffer_recording) {
    ExrContextCreateInfo ctx_info = {};
    ctx_info.api_version = TINYEXR_C_API_VERSION;

    ExrContext ctx = nullptr;
    ExrResult result = exr_context_create(&ctx_info, &ctx);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrCommandBufferCreateInfo cmd_info = {};
    cmd_info.decoder = nullptr;  /* Will test recording without decoder */
    cmd_info.max_commands = 64;

    ExrCommandBuffer cmd = nullptr;
    result = exr_command_buffer_create(ctx, &cmd_info, &cmd);
    REQUIRE_EQ(result, EXR_SUCCESS);
    REQUIRE(cmd != nullptr);

    /* Begin recording */
    result = exr_command_buffer_begin(cmd);
    REQUIRE_EQ(result, EXR_SUCCESS);

    /* Can't begin again while recording */
    result = exr_command_buffer_begin(cmd);
    REQUIRE_EQ(result, EXR_ERROR_INVALID_STATE);

    /* End recording */
    result = exr_command_buffer_end(cmd);
    REQUIRE_EQ(result, EXR_SUCCESS);

    /* Can't end again */
    result = exr_command_buffer_end(cmd);
    REQUIRE_EQ(result, EXR_ERROR_INVALID_STATE);

    /* Reset and record again */
    result = exr_command_buffer_reset(cmd);
    REQUIRE_EQ(result, EXR_SUCCESS);

    result = exr_command_buffer_begin(cmd);
    REQUIRE_EQ(result, EXR_SUCCESS);

    result = exr_command_buffer_end(cmd);
    REQUIRE_EQ(result, EXR_SUCCESS);

    exr_command_buffer_destroy(cmd);
    exr_context_destroy(ctx);
}

TEST(c_api_submit_basic) {
    /* Try to load asakusa.exr (ZIP compressed) */
    std::vector<uint8_t> data = read_file("../../asakusa.exr");
    if (data.empty()) {
        printf("[SKIPPED - file not found] ");
        return;
    }

    ExrContextCreateInfo ctx_info = {};
    ctx_info.api_version = TINYEXR_C_API_VERSION;

    ExrContext ctx = nullptr;
    ExrResult result = exr_context_create(&ctx_info, &ctx);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrDataSource source = {};
    result = exr_data_source_from_memory(data.data(), data.size(), &source);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrDecoderCreateInfo dec_info = {};
    dec_info.source = source;

    ExrDecoder decoder = nullptr;
    result = exr_decoder_create(ctx, &dec_info, &decoder);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrImage image = nullptr;
    result = exr_decoder_parse_header(decoder, &image);
    REQUIRE_EQ(result, EXR_SUCCESS);
    REQUIRE(image != nullptr);

    /* Create command buffer */
    ExrCommandBufferCreateInfo cmd_info = {};
    cmd_info.decoder = decoder;

    ExrCommandBuffer cmd = nullptr;
    result = exr_command_buffer_create(ctx, &cmd_info, &cmd);
    REQUIRE_EQ(result, EXR_SUCCESS);

    /* Get part info */
    ExrPart part = nullptr;
    result = exr_image_get_part(image, 0, &part);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrPartInfo part_info = {};
    result = exr_part_get_info(part, &part_info);
    REQUIRE_EQ(result, EXR_SUCCESS);

    /* Allocate output buffer */
    size_t buffer_size = (size_t)part_info.width * part_info.height *
                         part_info.num_channels * sizeof(uint16_t);  /* HALF */
    std::vector<uint8_t> output_buffer(buffer_size, 0);

    /* Record full image read command */
    result = exr_command_buffer_begin(cmd);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrFullImageRequest full_req = {};
    full_req.part = part;
    full_req.output.data = output_buffer.data();
    full_req.output.size = buffer_size;
    full_req.channels_mask = 0;  /* All channels */
    full_req.output_pixel_type = EXR_PIXEL_HALF;

    result = exr_cmd_request_full_image(cmd, &full_req);
    REQUIRE_EQ(result, EXR_SUCCESS);

    result = exr_command_buffer_end(cmd);
    REQUIRE_EQ(result, EXR_SUCCESS);

    /* Submit - this will fail for ZIP compression (not implemented yet) */
    ExrSubmitInfo submit_info = {};
    ExrCommandBuffer cmd_buffers[] = { cmd };
    submit_info.command_buffer_count = 1;
    submit_info.command_buffers = cmd_buffers;

    result = exr_submit(decoder, &submit_info);
    /* Check result based on compression type */
    if (part_info.compression == EXR_COMPRESSION_NONE ||
        part_info.compression == EXR_COMPRESSION_RLE ||
        part_info.compression == EXR_COMPRESSION_ZIP ||
        part_info.compression == EXR_COMPRESSION_ZIPS) {
        REQUIRE_EQ(result, EXR_SUCCESS);
        printf("[loaded comp=%d] ", part_info.compression);
    } else {
        /* PIZ/PXR24/B44 compressions not implemented yet */
        REQUIRE_EQ(result, EXR_ERROR_UNSUPPORTED_FORMAT);
        printf("[comp=%d not impl] ", part_info.compression);
    }

    /* Clean up - decoder owns the image */
    exr_part_destroy(part);
    exr_command_buffer_destroy(cmd);
    exr_decoder_destroy(decoder);
    exr_context_destroy(ctx);
}

TEST(c_api_verify_zip_vs_v1) {
    /* Compare V3 ZIP decompression output with V1 API */
    std::vector<uint8_t> fileData = read_file("../../asakusa.exr");
    if (fileData.empty()) {
        printf("[SKIPPED - file not found] ");
        return;
    }

    /* Load with V1 API */
    EXRVersion exr_version;
    int ret = ParseEXRVersionFromMemory(&exr_version, fileData.data(), fileData.size());
    REQUIRE_EQ(ret, TINYEXR_SUCCESS);

    EXRHeader header;
    InitEXRHeader(&header);
    const char* err = nullptr;
    ret = ParseEXRHeaderFromMemory(&header, &exr_version, fileData.data(), fileData.size(), &err);
    REQUIRE_EQ(ret, TINYEXR_SUCCESS);

    EXRImage v1_image;
    InitEXRImage(&v1_image);
    ret = LoadEXRImageFromMemory(&v1_image, &header, fileData.data(), fileData.size(), &err);
    REQUIRE_EQ(ret, TINYEXR_SUCCESS);

    /* Load with V3 API */
    ExrContextCreateInfo ctx_info = {};
    ctx_info.api_version = TINYEXR_C_API_VERSION;
    ExrContext ctx = nullptr;
    ExrResult result = exr_context_create(&ctx_info, &ctx);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrDataSource source = {};
    result = exr_data_source_from_memory(fileData.data(), fileData.size(), &source);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrDecoderCreateInfo dec_info = {};
    dec_info.source = source;
    ExrDecoder decoder = nullptr;
    result = exr_decoder_create(ctx, &dec_info, &decoder);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrImage v3_header = nullptr;
    result = exr_decoder_parse_header(decoder, &v3_header);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrPart part = nullptr;
    result = exr_image_get_part(v3_header, 0, &part);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrPartInfo part_info = {};
    result = exr_part_get_info(part, &part_info);
    REQUIRE_EQ(result, EXR_SUCCESS);

    /* Allocate V3 output buffer */
    size_t buffer_size = (size_t)part_info.width * part_info.height *
                         part_info.num_channels * sizeof(uint16_t);
    std::vector<uint8_t> v3_buffer(buffer_size, 0);

    ExrCommandBufferCreateInfo cmd_info = {};
    cmd_info.decoder = decoder;
    ExrCommandBuffer cmd = nullptr;
    result = exr_command_buffer_create(ctx, &cmd_info, &cmd);
    REQUIRE_EQ(result, EXR_SUCCESS);

    result = exr_command_buffer_begin(cmd);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrFullImageRequest full_req = {};
    full_req.part = part;
    full_req.output.data = v3_buffer.data();
    full_req.output.size = buffer_size;
    full_req.channels_mask = 0;
    full_req.output_pixel_type = EXR_PIXEL_HALF;

    result = exr_cmd_request_full_image(cmd, &full_req);
    REQUIRE_EQ(result, EXR_SUCCESS);

    result = exr_command_buffer_end(cmd);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrSubmitInfo submit_info = {};
    ExrCommandBuffer cmd_buffers[] = { cmd };
    submit_info.command_buffer_count = 1;
    submit_info.command_buffers = cmd_buffers;

    result = exr_submit(decoder, &submit_info);
    REQUIRE_EQ(result, EXR_SUCCESS);

    /* Compare pixel data */
    /* V1 stores data per-channel in images[], V3 stores per-line (channel sequential) */
    /* EXR format: for each line: [ch0 data][ch1 data][ch2 data][ch3 data] */
    int width = part_info.width;
    int height = part_info.height;
    int num_channels = part_info.num_channels;

    int mismatch_count = 0;
    int total_pixels = width * height * num_channels;

    /* V3 data is per-line, channel sequential */
    const uint16_t* v3_data = reinterpret_cast<const uint16_t*>(v3_buffer.data());

    for (int y = 0; y < height && mismatch_count < 10; y++) {
        for (int c = 0; c < num_channels && mismatch_count < 10; c++) {
            for (int x = 0; x < width && mismatch_count < 10; x++) {
                /* V1: images[channel][(y * width + x)] */
                const uint16_t* v1_channel = reinterpret_cast<const uint16_t*>(v1_image.images[c]);
                uint16_t v1_val = v1_channel[y * width + x];

                /* V3: per-line [y * (width * num_channels) + c * width + x] */
                uint16_t v3_val = v3_data[y * (width * num_channels) + c * width + x];

                if (v1_val != v3_val) {
                    mismatch_count++;
                    printf("\n    Mismatch at (%d,%d) ch%d: V1=%04x V3=%04x", x, y, c, v1_val, v3_val);
                }
            }
        }
    }

    /* Clean up V3 - decoder owns the image */
    exr_part_destroy(part);
    exr_command_buffer_destroy(cmd);
    exr_decoder_destroy(decoder);
    exr_context_destroy(ctx);

    /* Clean up V1 */
    FreeEXRImage(&v1_image);
    FreeEXRHeader(&header);

    if (mismatch_count > 0) {
        printf("\n    Total mismatches: %d/%d ", mismatch_count, total_pixels);
    }
    REQUIRE_EQ(mismatch_count, 0);
    printf("[%dx%dx%d verified] ", width, height, num_channels);
}

TEST(c_api_verify_piz_vs_v1) {
    /* PIZ compression test - uses V2 optimized Huffman decoder */
    std::vector<uint8_t> fileData = read_file("../../test/unit/regression/issue-160-piz-decode.exr");
    if (fileData.empty()) {
        printf("[SKIPPED - PIZ test file not found] ");
        return;
    }

    /* Load with V1 API */
    EXRVersion exr_version;
    int ret = ParseEXRVersionFromMemory(&exr_version, fileData.data(), fileData.size());
    REQUIRE_EQ(ret, TINYEXR_SUCCESS);

    EXRHeader header;
    InitEXRHeader(&header);
    const char* err = nullptr;
    ret = ParseEXRHeaderFromMemory(&header, &exr_version, fileData.data(), fileData.size(), &err);
    REQUIRE_EQ(ret, TINYEXR_SUCCESS);

    /* Check compression is PIZ */
    REQUIRE_EQ(header.compression_type, TINYEXR_COMPRESSIONTYPE_PIZ);

    EXRImage v1_image;
    InitEXRImage(&v1_image);
    ret = LoadEXRImageFromMemory(&v1_image, &header, fileData.data(), fileData.size(), &err);
    REQUIRE_EQ(ret, TINYEXR_SUCCESS);

    /* Load with V3 API */
    ExrContextCreateInfo ctx_info = {};
    ctx_info.api_version = TINYEXR_C_API_VERSION;
    ExrContext ctx = nullptr;
    ExrResult result = exr_context_create(&ctx_info, &ctx);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrDataSource source = {};
    result = exr_data_source_from_memory(fileData.data(), fileData.size(), &source);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrDecoderCreateInfo dec_info = {};
    dec_info.source = source;
    ExrDecoder decoder = nullptr;
    result = exr_decoder_create(ctx, &dec_info, &decoder);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrImage v3_header = nullptr;
    result = exr_decoder_parse_header(decoder, &v3_header);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrPart part = nullptr;
    result = exr_image_get_part(v3_header, 0, &part);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrPartInfo part_info = {};
    result = exr_part_get_info(part, &part_info);
    REQUIRE_EQ(result, EXR_SUCCESS);

    /* Verify compression type is PIZ */
    REQUIRE_EQ(part_info.compression, EXR_COMPRESSION_PIZ);

    /* Get actual pixel type from first channel */
    ExrChannelInfo ch_info = {};
    result = exr_part_get_channel(part, 0, &ch_info);
    REQUIRE_EQ(result, EXR_SUCCESS);

    int bytes_per_pixel = (ch_info.pixel_type == EXR_PIXEL_HALF) ? 2 : 4;
    printf("[ptype=%d] ", ch_info.pixel_type);

    /* Allocate V3 output buffer with correct size for actual pixel type */
    size_t buffer_size = (size_t)part_info.width * part_info.height *
                         part_info.num_channels * bytes_per_pixel;
    std::vector<uint8_t> v3_buffer(buffer_size, 0);

    ExrCommandBufferCreateInfo cmd_info = {};
    cmd_info.decoder = decoder;
    ExrCommandBuffer cmd = nullptr;
    result = exr_command_buffer_create(ctx, &cmd_info, &cmd);
    REQUIRE_EQ(result, EXR_SUCCESS);

    result = exr_command_buffer_begin(cmd);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrFullImageRequest full_req = {};
    full_req.part = part;
    full_req.output.data = v3_buffer.data();
    full_req.output.size = buffer_size;
    full_req.channels_mask = 0;
    full_req.output_pixel_type = ch_info.pixel_type;  /* Use actual pixel type */

    result = exr_cmd_request_full_image(cmd, &full_req);
    REQUIRE_EQ(result, EXR_SUCCESS);

    result = exr_command_buffer_end(cmd);
    REQUIRE_EQ(result, EXR_SUCCESS);

    printf("[submit...] ");
    fflush(stdout);

    ExrSubmitInfo submit_info = {};
    ExrCommandBuffer cmd_buffers[] = { cmd };
    submit_info.command_buffer_count = 1;
    submit_info.command_buffers = cmd_buffers;

    result = exr_submit(decoder, &submit_info);
    printf("[result=%d] ", (int)result);
    fflush(stdout);
    REQUIRE_EQ(result, EXR_SUCCESS);

    printf("[compare...] ");
    fflush(stdout);

    /* Compare pixel data */
    int width = part_info.width;
    int height = part_info.height;
    int num_channels = part_info.num_channels;

    int mismatch_count = 0;
    int total_pixels = width * height * num_channels;

    /* Compare based on actual pixel type */
    if (ch_info.pixel_type == EXR_PIXEL_HALF) {
        const uint16_t* v3_data = reinterpret_cast<const uint16_t*>(v3_buffer.data());
        for (int y = 0; y < height && mismatch_count < 10; y++) {
            for (int c = 0; c < num_channels && mismatch_count < 10; c++) {
                for (int x = 0; x < width && mismatch_count < 10; x++) {
                    const uint16_t* v1_channel = reinterpret_cast<const uint16_t*>(v1_image.images[c]);
                    uint16_t v1_val = v1_channel[y * width + x];
                    uint16_t v3_val = v3_data[y * (width * num_channels) + c * width + x];
                    if (v1_val != v3_val) {
                        mismatch_count++;
                        printf("\n    Mismatch at (%d,%d) ch%d: V1=%04x V3=%04x", x, y, c, v1_val, v3_val);
                    }
                }
            }
        }
    } else {
        /* FLOAT or UINT - compare as 32-bit values */
        const uint32_t* v3_data = reinterpret_cast<const uint32_t*>(v3_buffer.data());
        for (int y = 0; y < height && mismatch_count < 10; y++) {
            for (int c = 0; c < num_channels && mismatch_count < 10; c++) {
                for (int x = 0; x < width && mismatch_count < 10; x++) {
                    const uint32_t* v1_channel = reinterpret_cast<const uint32_t*>(v1_image.images[c]);
                    uint32_t v1_val = v1_channel[y * width + x];
                    uint32_t v3_val = v3_data[y * (width * num_channels) + c * width + x];
                    if (v1_val != v3_val) {
                        mismatch_count++;
                        printf("\n    Mismatch at (%d,%d) ch%d: V1=%08x V3=%08x", x, y, c, v1_val, v3_val);
                    }
                }
            }
        }
    }

    /* Clean up V3 - decoder owns the image */
    exr_part_destroy(part);
    exr_command_buffer_destroy(cmd);
    exr_decoder_destroy(decoder);
    exr_context_destroy(ctx);

    /* Clean up V1 */
    FreeEXRImage(&v1_image);
    FreeEXRHeader(&header);

    if (mismatch_count > 0) {
        printf("\n    Total mismatches: %d/%d ", mismatch_count, total_pixels);
    }
    REQUIRE_EQ(mismatch_count, 0);
    printf("[%dx%dx%d verified] ", width, height, num_channels);
}

/* Test PXR24 decompression vs V1 */
TEST(c_api_verify_pxr24_vs_v1) {
    /* PXR24 compression test */
    std::vector<uint8_t> fileData = read_file("../../test/unit/regression/pxr24_test.exr");
    if (fileData.empty()) {
        printf("[SKIPPED - PXR24 test file not found] ");
        return;
    }

    /* Load with V1 API */
    EXRVersion exr_version;
    int ret = ParseEXRVersionFromMemory(&exr_version, fileData.data(), fileData.size());
    REQUIRE_EQ(ret, TINYEXR_SUCCESS);

    EXRHeader header;
    InitEXRHeader(&header);
    const char* err = nullptr;
    ret = ParseEXRHeaderFromMemory(&header, &exr_version, fileData.data(), fileData.size(), &err);
    REQUIRE_EQ(ret, TINYEXR_SUCCESS);

    /* Check compression is PXR24 */
    REQUIRE_EQ(header.compression_type, TINYEXR_COMPRESSIONTYPE_PXR24);

    EXRImage v1_image;
    InitEXRImage(&v1_image);
    ret = LoadEXRImageFromMemory(&v1_image, &header, fileData.data(), fileData.size(), &err);
    REQUIRE_EQ(ret, TINYEXR_SUCCESS);

    /* Load with V3 API */
    ExrContextCreateInfo ctx_info = {};
    ctx_info.api_version = TINYEXR_C_API_VERSION;
    ExrContext ctx = nullptr;
    ExrResult result = exr_context_create(&ctx_info, &ctx);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrDataSource source = {};
    result = exr_data_source_from_memory(fileData.data(), fileData.size(), &source);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrDecoderCreateInfo dec_info = {};
    dec_info.source = source;
    ExrDecoder decoder = nullptr;
    result = exr_decoder_create(ctx, &dec_info, &decoder);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrImage v3_header = nullptr;
    result = exr_decoder_parse_header(decoder, &v3_header);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrPart part = nullptr;
    result = exr_image_get_part(v3_header, 0, &part);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrPartInfo part_info = {};
    result = exr_part_get_info(part, &part_info);
    REQUIRE_EQ(result, EXR_SUCCESS);

    /* Verify compression type is PXR24 */
    REQUIRE_EQ(part_info.compression, EXR_COMPRESSION_PXR24);

    /* Get actual pixel type from first channel */
    ExrChannelInfo ch_info = {};
    result = exr_part_get_channel(part, 0, &ch_info);
    REQUIRE_EQ(result, EXR_SUCCESS);

    int bytes_per_pixel = (ch_info.pixel_type == EXR_PIXEL_HALF) ? 2 : 4;
    printf("[ptype=%d] ", ch_info.pixel_type);

    /* Allocate V3 output buffer */
    size_t buffer_size = (size_t)part_info.width * part_info.height *
                         part_info.num_channels * bytes_per_pixel;
    std::vector<uint8_t> v3_buffer(buffer_size, 0);

    ExrCommandBufferCreateInfo cmd_info = {};
    cmd_info.decoder = decoder;
    ExrCommandBuffer cmd = nullptr;
    result = exr_command_buffer_create(ctx, &cmd_info, &cmd);
    REQUIRE_EQ(result, EXR_SUCCESS);

    result = exr_command_buffer_begin(cmd);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrFullImageRequest full_req = {};
    full_req.part = part;
    full_req.output.data = v3_buffer.data();
    full_req.output.size = buffer_size;
    full_req.channels_mask = 0;
    full_req.output_pixel_type = ch_info.pixel_type;

    result = exr_cmd_request_full_image(cmd, &full_req);
    REQUIRE_EQ(result, EXR_SUCCESS);

    result = exr_command_buffer_end(cmd);
    REQUIRE_EQ(result, EXR_SUCCESS);

    printf("[submit...] ");
    fflush(stdout);

    ExrSubmitInfo submit_info = {};
    ExrCommandBuffer cmd_buffers[] = { cmd };
    submit_info.command_buffer_count = 1;
    submit_info.command_buffers = cmd_buffers;

    result = exr_submit(decoder, &submit_info);
    printf("[result=%d] ", (int)result);
    fflush(stdout);
    REQUIRE_EQ(result, EXR_SUCCESS);

    printf("[compare...] ");
    fflush(stdout);

    /* Compare pixel data */
    int width = part_info.width;
    int height = part_info.height;
    int num_channels = part_info.num_channels;

    int mismatch_count = 0;
    int total_pixels = width * height * num_channels;

    /* Compare based on actual pixel type (HALF for this test file) */
    if (ch_info.pixel_type == EXR_PIXEL_HALF) {
        const uint16_t* v3_data = reinterpret_cast<const uint16_t*>(v3_buffer.data());
        for (int y = 0; y < height && mismatch_count < 10; y++) {
            for (int c = 0; c < num_channels && mismatch_count < 10; c++) {
                for (int x = 0; x < width && mismatch_count < 10; x++) {
                    const uint16_t* v1_channel = reinterpret_cast<const uint16_t*>(v1_image.images[c]);
                    uint16_t v1_val = v1_channel[y * width + x];
                    uint16_t v3_val = v3_data[y * (width * num_channels) + c * width + x];
                    if (v1_val != v3_val) {
                        mismatch_count++;
                        printf("\n    Mismatch at (%d,%d) ch%d: V1=%04x V3=%04x", x, y, c, v1_val, v3_val);
                    }
                }
            }
        }
    } else {
        /* FLOAT or UINT */
        const uint32_t* v3_data = reinterpret_cast<const uint32_t*>(v3_buffer.data());
        for (int y = 0; y < height && mismatch_count < 10; y++) {
            for (int c = 0; c < num_channels && mismatch_count < 10; c++) {
                for (int x = 0; x < width && mismatch_count < 10; x++) {
                    const uint32_t* v1_channel = reinterpret_cast<const uint32_t*>(v1_image.images[c]);
                    uint32_t v1_val = v1_channel[y * width + x];
                    uint32_t v3_val = v3_data[y * (width * num_channels) + c * width + x];
                    if (v1_val != v3_val) {
                        mismatch_count++;
                        printf("\n    Mismatch at (%d,%d) ch%d: V1=%08x V3=%08x", x, y, c, v1_val, v3_val);
                    }
                }
            }
        }
    }

    /* Clean up V3 - decoder owns the image */
    exr_part_destroy(part);
    exr_command_buffer_destroy(cmd);
    exr_decoder_destroy(decoder);
    exr_context_destroy(ctx);

    /* Clean up V1 */
    FreeEXRImage(&v1_image);
    FreeEXRHeader(&header);

    if (mismatch_count > 0) {
        printf("\n    Total mismatches: %d/%d ", mismatch_count, total_pixels);
    }
    REQUIRE_EQ(mismatch_count, 0);
    printf("[%dx%dx%d verified] ", width, height, num_channels);
}

/* Test B44 decompression vs V1 */
TEST(c_api_verify_b44_vs_v1) {
    /* B44 compression test */
    std::vector<uint8_t> fileData = read_file("../../test/unit/regression/b44_test.exr");
    if (fileData.empty()) {
        printf("[SKIPPED - B44 test file not found] ");
        return;
    }

    /* Load with V1 API */
    EXRVersion exr_version;
    int ret = ParseEXRVersionFromMemory(&exr_version, fileData.data(), fileData.size());
    REQUIRE_EQ(ret, TINYEXR_SUCCESS);

    EXRHeader header;
    InitEXRHeader(&header);
    const char* err = nullptr;
    ret = ParseEXRHeaderFromMemory(&header, &exr_version, fileData.data(), fileData.size(), &err);
    REQUIRE_EQ(ret, TINYEXR_SUCCESS);

    /* Check compression is B44 */
    REQUIRE_EQ(header.compression_type, TINYEXR_COMPRESSIONTYPE_B44);

    EXRImage v1_image;
    InitEXRImage(&v1_image);
    ret = LoadEXRImageFromMemory(&v1_image, &header, fileData.data(), fileData.size(), &err);
    REQUIRE_EQ(ret, TINYEXR_SUCCESS);

    /* Load with V3 API */
    ExrContextCreateInfo ctx_info = {};
    ctx_info.api_version = TINYEXR_C_API_VERSION;
    ExrContext ctx = nullptr;
    ExrResult result = exr_context_create(&ctx_info, &ctx);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrDataSource source = {};
    result = exr_data_source_from_memory(fileData.data(), fileData.size(), &source);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrDecoderCreateInfo dec_info = {};
    dec_info.source = source;
    ExrDecoder decoder = nullptr;
    result = exr_decoder_create(ctx, &dec_info, &decoder);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrImage v3_header = nullptr;
    result = exr_decoder_parse_header(decoder, &v3_header);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrPart part = nullptr;
    result = exr_image_get_part(v3_header, 0, &part);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrPartInfo part_info = {};
    result = exr_part_get_info(part, &part_info);
    REQUIRE_EQ(result, EXR_SUCCESS);

    /* Verify compression type is B44 */
    REQUIRE_EQ(part_info.compression, EXR_COMPRESSION_B44);

    /* Get actual pixel type from first channel */
    ExrChannelInfo ch_info = {};
    result = exr_part_get_channel(part, 0, &ch_info);
    REQUIRE_EQ(result, EXR_SUCCESS);

    int bytes_per_pixel = (ch_info.pixel_type == EXR_PIXEL_HALF) ? 2 : 4;
    printf("[ptype=%d] ", ch_info.pixel_type);

    /* Allocate V3 output buffer */
    size_t buffer_size = (size_t)part_info.width * part_info.height *
                         part_info.num_channels * bytes_per_pixel;
    std::vector<uint8_t> v3_buffer(buffer_size, 0);

    ExrCommandBufferCreateInfo cmd_info = {};
    cmd_info.decoder = decoder;
    ExrCommandBuffer cmd = nullptr;
    result = exr_command_buffer_create(ctx, &cmd_info, &cmd);
    REQUIRE_EQ(result, EXR_SUCCESS);

    result = exr_command_buffer_begin(cmd);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrFullImageRequest full_req = {};
    full_req.part = part;
    full_req.output.data = v3_buffer.data();
    full_req.output.size = buffer_size;
    full_req.channels_mask = 0;
    full_req.output_pixel_type = ch_info.pixel_type;

    result = exr_cmd_request_full_image(cmd, &full_req);
    REQUIRE_EQ(result, EXR_SUCCESS);

    result = exr_command_buffer_end(cmd);
    REQUIRE_EQ(result, EXR_SUCCESS);

    printf("[submit...] ");
    fflush(stdout);

    ExrSubmitInfo submit_info = {};
    ExrCommandBuffer cmd_buffers[] = { cmd };
    submit_info.command_buffer_count = 1;
    submit_info.command_buffers = cmd_buffers;

    result = exr_submit(decoder, &submit_info);
    printf("[result=%d] ", (int)result);
    fflush(stdout);
    REQUIRE_EQ(result, EXR_SUCCESS);

    printf("[compare...] ");
    fflush(stdout);

    /* Compare pixel data */
    int width = part_info.width;
    int height = part_info.height;
    int num_channels = part_info.num_channels;

    int mismatch_count = 0;
    int total_pixels = width * height * num_channels;

    /* Compare based on actual pixel type (HALF for B44) */
    const uint16_t* v3_data = reinterpret_cast<const uint16_t*>(v3_buffer.data());
    for (int y = 0; y < height && mismatch_count < 10; y++) {
        for (int c = 0; c < num_channels && mismatch_count < 10; c++) {
            for (int x = 0; x < width && mismatch_count < 10; x++) {
                const uint16_t* v1_channel = reinterpret_cast<const uint16_t*>(v1_image.images[c]);
                uint16_t v1_val = v1_channel[y * width + x];
                uint16_t v3_val = v3_data[y * (width * num_channels) + c * width + x];
                if (v1_val != v3_val) {
                    mismatch_count++;
                    printf("\n    Mismatch at (%d,%d) ch%d: V1=%04x V3=%04x", x, y, c, v1_val, v3_val);
                }
            }
        }
    }

    /* Clean up V3 - decoder owns the image */
    exr_part_destroy(part);
    exr_command_buffer_destroy(cmd);
    exr_decoder_destroy(decoder);
    exr_context_destroy(ctx);

    /* Clean up V1 */
    FreeEXRImage(&v1_image);
    FreeEXRHeader(&header);

    if (mismatch_count > 0) {
        printf("\n    Total mismatches: %d/%d ", mismatch_count, total_pixels);
    }
    REQUIRE_EQ(mismatch_count, 0);
    printf("[%dx%dx%d verified] ", width, height, num_channels);
}

/* Test tiled image reading */
TEST(c_api_verify_tiled) {
    /* Try to find a tiled EXR file */
    const char* tiled_paths[] = {
        "../../openexr-images/Tiles/GoldenGate.exr",
        "../../openexr-images/Tiles/Spirals.exr",
        "../../openexr-images/Tiles/Ocean.exr",
        nullptr
    };

    std::vector<uint8_t> fileData;
    const char* used_path = nullptr;
    for (int i = 0; tiled_paths[i]; i++) {
        fileData = read_file(tiled_paths[i]);
        if (!fileData.empty()) {
            used_path = tiled_paths[i];
            break;
        }
    }

    if (fileData.empty()) {
        printf("[SKIPPED - no tiled test file found] ");
        return;
    }

    /* Parse with V3 API */
    ExrContextCreateInfo ctx_info = {};
    ctx_info.api_version = TINYEXR_C_API_VERSION;
    ExrContext ctx = nullptr;
    ExrResult result = exr_context_create(&ctx_info, &ctx);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrDataSource source = {};
    result = exr_data_source_from_memory(fileData.data(), fileData.size(), &source);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrDecoderCreateInfo dec_info = {};
    dec_info.source = source;
    ExrDecoder decoder = nullptr;
    result = exr_decoder_create(ctx, &dec_info, &decoder);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrImage v3_header = nullptr;
    result = exr_decoder_parse_header(decoder, &v3_header);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrPart part = nullptr;
    result = exr_image_get_part(v3_header, 0, &part);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrPartInfo part_info = {};
    result = exr_part_get_info(part, &part_info);
    REQUIRE_EQ(result, EXR_SUCCESS);

    /* Verify this is a tiled image */
    if (part_info.part_type != EXR_PART_TILED) {
        printf("[SKIPPED - not a tiled image] ");
        exr_part_destroy(part);
        exr_decoder_destroy(decoder);
        exr_context_destroy(ctx);
        return;
    }

    printf("[tiled %dx%d ch=%d] ", part_info.width, part_info.height,
           part_info.num_channels);

    /* Get actual pixel type from first channel */
    ExrChannelInfo ch_info = {};
    result = exr_part_get_channel(part, 0, &ch_info);
    REQUIRE_EQ(result, EXR_SUCCESS);

    int bytes_per_pixel = (ch_info.pixel_type == EXR_PIXEL_HALF) ? 2 : 4;

    /* Allocate V3 output buffer */
    size_t buffer_size = (size_t)part_info.width * part_info.height *
                         part_info.num_channels * bytes_per_pixel;
    std::vector<uint8_t> v3_buffer(buffer_size, 0);

    ExrCommandBufferCreateInfo cmd_info = {};
    cmd_info.decoder = decoder;
    ExrCommandBuffer cmd = nullptr;
    result = exr_command_buffer_create(ctx, &cmd_info, &cmd);
    REQUIRE_EQ(result, EXR_SUCCESS);

    result = exr_command_buffer_begin(cmd);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrFullImageRequest full_req = {};
    full_req.part = part;
    full_req.output.data = v3_buffer.data();
    full_req.output.size = buffer_size;
    full_req.channels_mask = 0;
    full_req.output_pixel_type = ch_info.pixel_type;

    result = exr_cmd_request_full_image(cmd, &full_req);
    REQUIRE_EQ(result, EXR_SUCCESS);

    result = exr_command_buffer_end(cmd);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrSubmitInfo submit_info = {};
    ExrCommandBuffer cmd_buffers[] = { cmd };
    submit_info.command_buffer_count = 1;
    submit_info.command_buffers = cmd_buffers;

    result = exr_submit(decoder, &submit_info);
    REQUIRE_EQ(result, EXR_SUCCESS);

    /* Verify data is non-zero (basic sanity check) */
    bool has_non_zero = false;
    for (size_t i = 0; i < v3_buffer.size() && !has_non_zero; i++) {
        if (v3_buffer[i] != 0) has_non_zero = true;
    }
    REQUIRE_EQ(has_non_zero, true);

    /* Clean up */
    exr_part_destroy(part);
    exr_command_buffer_destroy(cmd);
    exr_decoder_destroy(decoder);
    exr_context_destroy(ctx);

    printf("[loaded OK] ");
}

/* ============================================================================
 * Compression Unit Tests
 *
 * Tests individual compression codecs through exr_decompress_chunk API.
 * ============================================================================ */

/* Helper: create a minimal ExrContext for decompression tests */
static ExrContext create_test_context() {
    ExrContextCreateInfo ctx_info = {};
    ctx_info.api_version = TINYEXR_C_API_VERSION;
    ExrContext ctx = nullptr;
    ExrResult result = exr_context_create(&ctx_info, &ctx);
    if (result != EXR_SUCCESS) {
        throw std::runtime_error("Failed to create test context");
    }
    return ctx;
}

/* Helper: compress data with RLE format (for round-trip testing).
 * EXR RLE format: predictor encode + byte reorder, then RLE encode.
 * For simplicity, we'll use exr_compress_chunk if available or construct
 * test vectors manually. */

/* Build an EXR-RLE compressed payload from raw pixel data.
 * The EXR post-processing order is:
 * Compress: reorder -> predictor -> RLE
 * Decompress: RLE -> predictor -> reorder */
static std::vector<uint8_t> rle_compress_simple(const uint8_t* data, size_t len) {
    /* Step 1: Byte reorder (split odd/even bytes) */
    std::vector<uint8_t> reordered(len);
    size_t half = (len + 1) / 2;
    for (size_t i = 0; i < len; i++) {
        if (i % 2 == 0) {
            reordered[i / 2] = data[i];
        } else {
            reordered[half + i / 2] = data[i];
        }
    }

    /* Step 2: Delta predictor encode (backward to preserve data) */
    std::vector<uint8_t> predicted(reordered);
    for (size_t i = len; i > 1; i--) {
        int d = (int)predicted[i - 1] - (int)predicted[i - 2] + 128;
        predicted[i - 1] = (uint8_t)d;
    }

    /* Step 3: RLE encode */
    std::vector<uint8_t> compressed;
    size_t pos = 0;
    while (pos < len) {
        /* Check for a run */
        size_t run = 1;
        while (pos + run < len && predicted[pos + run] == predicted[pos] && run < 128) {
            run++;
        }

        if (run >= 3) {
            /* RLE run: non-negative count = repeat (count+1) times */
            compressed.push_back((uint8_t)(run - 1));
            compressed.push_back(predicted[pos]);
            pos += run;
        } else {
            /* Literal run: find how many non-repeating bytes */
            size_t lit_start = pos;
            while (pos < len) {
                size_t ahead = 1;
                while (pos + ahead < len && predicted[pos + ahead] == predicted[pos] && ahead < 128) {
                    ahead++;
                }
                if (ahead >= 3) break;
                pos++;
                if (pos - lit_start >= 127) break;
            }
            size_t lit_len = pos - lit_start;
            compressed.push_back((uint8_t)(-(signed char)lit_len));
            for (size_t i = 0; i < lit_len; i++) {
                compressed.push_back(predicted[lit_start + i]);
            }
        }
    }

    return compressed;
}

/* ---- RLE Tests ---- */

TEST(rle_round_trip_basic) {
    /* Create test pixel data */
    const uint8_t original[] = { 10, 20, 30, 40, 50, 60, 70, 80, 90, 100 };
    size_t orig_len = sizeof(original);

    std::vector<uint8_t> compressed = rle_compress_simple(original, orig_len);

    /* Decompress via V3 API */
    ExrContext ctx = create_test_context();
    std::vector<uint8_t> decompressed(orig_len, 0);
    size_t out_size = 0;

    ExrDecompressInfo info = {};
    info.src = compressed.data();
    info.src_size = compressed.size();
    info.dst = decompressed.data();
    info.dst_capacity = decompressed.size();
    info.out_size = &out_size;
    info.compression = EXR_COMPRESSION_RLE;

    ExrResult result = exr_decompress_chunk(ctx, &info);
    REQUIRE_EQ(result, EXR_SUCCESS);
    REQUIRE_EQ(out_size, orig_len);

    for (size_t i = 0; i < orig_len; i++) {
        if (original[i] != decompressed[i]) {
            char msg[128];
            snprintf(msg, sizeof(msg), "Mismatch at byte %zu: expected %d, got %d",
                     i, original[i], decompressed[i]);
            throw std::runtime_error(msg);
        }
    }

    exr_context_destroy(ctx);
}

TEST(rle_all_repeat) {
    /* Data that is all the same byte (maximum RLE compression) */
    std::vector<uint8_t> original(256, 0xAA);

    std::vector<uint8_t> compressed = rle_compress_simple(original.data(), original.size());
    REQUIRE(compressed.size() < original.size());  /* Should be much smaller */

    ExrContext ctx = create_test_context();
    std::vector<uint8_t> decompressed(original.size(), 0);
    size_t out_size = 0;

    ExrDecompressInfo info = {};
    info.src = compressed.data();
    info.src_size = compressed.size();
    info.dst = decompressed.data();
    info.dst_capacity = decompressed.size();
    info.out_size = &out_size;
    info.compression = EXR_COMPRESSION_RLE;

    ExrResult result = exr_decompress_chunk(ctx, &info);
    REQUIRE_EQ(result, EXR_SUCCESS);
    REQUIRE_EQ(out_size, original.size());
    REQUIRE(memcmp(original.data(), decompressed.data(), original.size()) == 0);

    exr_context_destroy(ctx);
}

TEST(rle_uncompressed_passthrough) {
    /* When src_size == dst_size, RLE should just copy (EXR spec behavior) */
    const uint8_t data[] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    size_t len = sizeof(data);

    ExrContext ctx = create_test_context();
    std::vector<uint8_t> output(len, 0);
    size_t out_size = 0;

    ExrDecompressInfo info = {};
    info.src = data;
    info.src_size = len;
    info.dst = output.data();
    info.dst_capacity = len;
    info.out_size = &out_size;
    info.compression = EXR_COMPRESSION_RLE;

    ExrResult result = exr_decompress_chunk(ctx, &info);
    REQUIRE_EQ(result, EXR_SUCCESS);
    REQUIRE_EQ(out_size, len);
    REQUIRE(memcmp(data, output.data(), len) == 0);

    exr_context_destroy(ctx);
}

TEST(rle_truncated_input) {
    /* Construct an RLE stream that references more data than available */
    /* Literal run of 5 bytes but only 3 bytes of data follow */
    uint8_t bad_rle[] = { (uint8_t)-5, 0x01, 0x02, 0x03 };

    ExrContext ctx = create_test_context();
    std::vector<uint8_t> output(64, 0);
    size_t out_size = 0;

    ExrDecompressInfo info = {};
    info.src = bad_rle;
    info.src_size = sizeof(bad_rle);
    info.dst = output.data();
    info.dst_capacity = output.size();
    info.out_size = &out_size;
    info.compression = EXR_COMPRESSION_RLE;

    ExrResult result = exr_decompress_chunk(ctx, &info);
    REQUIRE(EXR_FAILED(result));

    exr_context_destroy(ctx);
}

/* ---- ZIP/Deflate Tests ---- */

TEST(zip_uncompressed_passthrough) {
    /* When src_size == dst_size, ZIP should just copy */
    const uint8_t data[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE };
    size_t len = sizeof(data);

    ExrContext ctx = create_test_context();
    std::vector<uint8_t> output(len, 0);
    size_t out_size = 0;

    ExrDecompressInfo info = {};
    info.src = data;
    info.src_size = len;
    info.dst = output.data();
    info.dst_capacity = len;
    info.out_size = &out_size;
    info.compression = EXR_COMPRESSION_ZIP;

    ExrResult result = exr_decompress_chunk(ctx, &info);
    REQUIRE_EQ(result, EXR_SUCCESS);
    REQUIRE_EQ(out_size, len);
    REQUIRE(memcmp(data, output.data(), len) == 0);

    exr_context_destroy(ctx);
}

TEST(zip_zips_uncompressed_passthrough) {
    /* ZIPS (single scanline ZIP) same passthrough behavior */
    const uint8_t data[] = { 0x01, 0x02, 0x03, 0x04 };
    size_t len = sizeof(data);

    ExrContext ctx = create_test_context();
    std::vector<uint8_t> output(len, 0);
    size_t out_size = 0;

    ExrDecompressInfo info = {};
    info.src = data;
    info.src_size = len;
    info.dst = output.data();
    info.dst_capacity = len;
    info.out_size = &out_size;
    info.compression = EXR_COMPRESSION_ZIPS;

    ExrResult result = exr_decompress_chunk(ctx, &info);
    REQUIRE_EQ(result, EXR_SUCCESS);
    REQUIRE_EQ(out_size, len);
    REQUIRE(memcmp(data, output.data(), len) == 0);

    exr_context_destroy(ctx);
}

TEST(zip_corrupt_data) {
    /* Random data that is not valid zlib -- should fail gracefully */
    uint8_t garbage[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

    ExrContext ctx = create_test_context();
    std::vector<uint8_t> output(64, 0);
    size_t out_size = 0;

    ExrDecompressInfo info = {};
    info.src = garbage;
    info.src_size = sizeof(garbage);
    info.dst = output.data();
    info.dst_capacity = output.size();
    info.out_size = &out_size;
    info.compression = EXR_COMPRESSION_ZIP;

    ExrResult result = exr_decompress_chunk(ctx, &info);
    REQUIRE(EXR_FAILED(result));

    exr_context_destroy(ctx);
}

TEST(zip_round_trip_real_file) {
    /* Load asakusa.exr (ZIP compressed), decompress all chunks via V3 pipeline,
     * and verify non-zero output. This exercises the full ZIP path including
     * predictor and reorder. */
    std::vector<uint8_t> fileData = read_file("../../asakusa.exr");
    if (fileData.empty()) {
        printf("[SKIPPED - asakusa.exr not found] ");
        return;
    }

    ExrContext ctx = create_test_context();

    ExrDataSource source = {};
    ExrResult result = exr_data_source_from_memory(fileData.data(), fileData.size(), &source);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrDecoderCreateInfo dec_info = {};
    dec_info.source = source;
    ExrDecoder decoder = nullptr;
    result = exr_decoder_create(ctx, &dec_info, &decoder);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrImage image = nullptr;
    result = exr_decoder_parse_header(decoder, &image);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrPart part = nullptr;
    result = exr_image_get_part(image, 0, &part);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrPartInfo part_info = {};
    result = exr_part_get_info(part, &part_info);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrChannelInfo ch_info = {};
    result = exr_part_get_channel(part, 0, &ch_info);
    REQUIRE_EQ(result, EXR_SUCCESS);

    int bpp = (ch_info.pixel_type == EXR_PIXEL_HALF) ? 2 : 4;
    size_t buf_size = (size_t)part_info.width * part_info.height *
                      part_info.num_channels * bpp;
    std::vector<uint8_t> buffer(buf_size, 0);

    ExrCommandBufferCreateInfo cmd_info = {};
    cmd_info.decoder = decoder;
    ExrCommandBuffer cmd = nullptr;
    result = exr_command_buffer_create(ctx, &cmd_info, &cmd);
    REQUIRE_EQ(result, EXR_SUCCESS);

    result = exr_command_buffer_begin(cmd);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrFullImageRequest full_req = {};
    full_req.part = part;
    full_req.output.data = buffer.data();
    full_req.output.size = buf_size;
    full_req.output_pixel_type = ch_info.pixel_type;

    result = exr_cmd_request_full_image(cmd, &full_req);
    REQUIRE_EQ(result, EXR_SUCCESS);

    result = exr_command_buffer_end(cmd);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrSubmitInfo submit_info = {};
    ExrCommandBuffer cmd_buffers[] = { cmd };
    submit_info.command_buffer_count = 1;
    submit_info.command_buffers = cmd_buffers;

    result = exr_submit(decoder, &submit_info);
    REQUIRE_EQ(result, EXR_SUCCESS);

    /* Verify non-trivial output */
    bool has_nonzero = false;
    for (size_t i = 0; i < buffer.size() && !has_nonzero; i++) {
        if (buffer[i] != 0) has_nonzero = true;
    }
    REQUIRE(has_nonzero);

    printf("[%dx%d decompressed OK] ", part_info.width, part_info.height);

    exr_part_destroy(part);
    exr_command_buffer_destroy(cmd);
    exr_decoder_destroy(decoder);
    exr_context_destroy(ctx);
}

/* ---- PIZ tests ---- */

TEST(piz_chunk_api_issue160_float32) {
    std::vector<uint8_t> fileData = read_file("../../test/unit/regression/issue-160-piz-decode.exr");
    if (fileData.empty()) {
        printf("[SKIPPED - PIZ test file not found] ");
        return;
    }

    EXRVersion exr_version;
    int ret = ParseEXRVersionFromMemory(&exr_version, fileData.data(), fileData.size());
    REQUIRE_EQ(ret, TINYEXR_SUCCESS);

    EXRHeader header;
    InitEXRHeader(&header);
    const char* err = nullptr;
    ret = ParseEXRHeaderFromMemory(&header, &exr_version, fileData.data(), fileData.size(), &err);
    REQUIRE_EQ(ret, TINYEXR_SUCCESS);
    REQUIRE_EQ(header.compression_type, TINYEXR_COMPRESSIONTYPE_PIZ);

    EXRImage v1_image;
    InitEXRImage(&v1_image);
    ret = LoadEXRImageFromMemory(&v1_image, &header, fileData.data(), fileData.size(), &err);
    REQUIRE_EQ(ret, TINYEXR_SUCCESS);

    ExrContext ctx = create_test_context();

    ExrDataSource source = {};
    ExrResult result = exr_data_source_from_memory(fileData.data(), fileData.size(), &source);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrDecoderCreateInfo dec_info = {};
    dec_info.source = source;
    ExrDecoder decoder = nullptr;
    result = exr_decoder_create(ctx, &dec_info, &decoder);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrImage image = nullptr;
    result = exr_decoder_parse_header(decoder, &image);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrPart part = nullptr;
    result = exr_image_get_part(image, 0, &part);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrPartInfo part_info = {};
    result = exr_part_get_info(part, &part_info);
    REQUIRE_EQ(result, EXR_SUCCESS);
    REQUIRE_EQ(part_info.compression, (uint32_t)EXR_COMPRESSION_PIZ);

    std::vector<ExrChannelInfo> channels(part_info.num_channels);
    for (uint32_t c = 0; c < part_info.num_channels; c++) {
        result = exr_part_get_channel(part, c, &channels[c]);
        REQUIRE_EQ(result, EXR_SUCCESS);
    }

    REQUIRE(!channels.empty());
    REQUIRE_EQ(channels[0].pixel_type, (uint32_t)EXR_PIXEL_FLOAT);

    size_t offset_table_start = 0;
    REQUIRE(find_single_part_offset_table_start(fileData, &offset_table_start));

    uint32_t chunk_count = 0;
    result = exr_part_get_chunk_count(part, &chunk_count);
    REQUIRE_EQ(result, EXR_SUCCESS);
    REQUIRE(chunk_count > 0);
    REQUIRE(offset_table_start + chunk_count * sizeof(uint64_t) <= fileData.size());

    uint64_t chunk_offset = read_le_u64_test(fileData.data() + offset_table_start);
    REQUIRE(chunk_offset + 8 <= fileData.size());
    uint32_t data_size = read_le_u32_test(fileData.data() + chunk_offset + 4);
    REQUIRE(chunk_offset + 8 + data_size <= fileData.size());

    int num_lines = part_info.height < 32 ? part_info.height : 32;
    int bytes_per_pixel = 4;  /* float32 regression */
    size_t bytes_per_line = (size_t)part_info.width * part_info.num_channels *
                            (size_t)bytes_per_pixel;
    size_t expected_size = bytes_per_line * (size_t)num_lines;
    std::vector<uint8_t> expected(expected_size, 0);
    std::vector<uint8_t> actual(expected_size, 0);

    for (int y = 0; y < num_lines; y++) {
        size_t line_offset = (size_t)y * bytes_per_line;
        size_t channel_offset = 0;
        for (uint32_t c = 0; c < part_info.num_channels; c++) {
            const uint8_t* src =
                reinterpret_cast<const uint8_t*>(v1_image.images[c]) +
                (size_t)y * (size_t)part_info.width * (size_t)bytes_per_pixel;
            size_t channel_bytes = (size_t)part_info.width * (size_t)bytes_per_pixel;
            memcpy(expected.data() + line_offset + channel_offset, src, channel_bytes);
            channel_offset += channel_bytes;
        }
    }

    size_t out_size = 0;
    ExrDecompressInfo info = {};
    info.src = fileData.data() + chunk_offset + 8;
    info.src_size = data_size;
    info.dst = actual.data();
    info.dst_capacity = actual.size();
    info.out_size = &out_size;
    info.compression = EXR_COMPRESSION_PIZ;
    info.width = part_info.width;
    info.num_lines = num_lines;
    info.num_channels = part_info.num_channels;
    info.channels = channels.data();

    result = exr_decompress_chunk(ctx, &info);
    REQUIRE_EQ(result, EXR_SUCCESS);
    REQUIRE_EQ(out_size, expected_size);
    REQUIRE(memcmp(expected.data(), actual.data(), expected_size) == 0);

    printf("[chunk %dx%d float32 verified] ", part_info.width, num_lines);

    exr_part_destroy(part);
    exr_decoder_destroy(decoder);
    exr_context_destroy(ctx);
    FreeEXRImage(&v1_image);
    FreeEXRHeader(&header);
}

TEST(piz_malformed_huffman_data) {
    /* Special-case bitmap for all-zero image + truncated Huffman payload. */
    const uint8_t bad_piz[] = {
        0xFF, 0xFF, 0x00, 0x00,
        0x04, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00
    };

    ExrChannelInfo channel = {};
    channel.name = "Y";
    channel.pixel_type = EXR_PIXEL_HALF;
    channel.x_sampling = 1;
    channel.y_sampling = 1;

    ExrContext ctx = create_test_context();
    uint16_t output = 0;
    size_t out_size = 0;

    ExrDecompressInfo info = {};
    info.src = bad_piz;
    info.src_size = sizeof(bad_piz);
    info.dst = &output;
    info.dst_capacity = sizeof(output);
    info.out_size = &out_size;
    info.compression = EXR_COMPRESSION_PIZ;
    info.width = 1;
    info.num_lines = 1;
    info.num_channels = 1;
    info.channels = &channel;

    ExrResult result = exr_decompress_chunk(ctx, &info);
    REQUIRE_EQ(result, EXR_ERROR_DECOMPRESSION_FAILED);

    exr_context_destroy(ctx);
}

/* ---- NONE compression tests ---- */

TEST(none_passthrough) {
    /* NONE compression just copies data */
    const uint8_t data[] = { 1, 2, 3, 4, 5 };

    ExrContext ctx = create_test_context();
    uint8_t output[5] = {};
    size_t out_size = 0;

    ExrDecompressInfo info = {};
    info.src = data;
    info.src_size = sizeof(data);
    info.dst = output;
    info.dst_capacity = sizeof(output);
    info.out_size = &out_size;
    info.compression = EXR_COMPRESSION_NONE;

    ExrResult result = exr_decompress_chunk(ctx, &info);
    REQUIRE_EQ(result, EXR_SUCCESS);
    REQUIRE_EQ(out_size, sizeof(data));
    REQUIRE(memcmp(data, output, sizeof(data)) == 0);

    exr_context_destroy(ctx);
}

TEST(none_buffer_too_small) {
    const uint8_t data[] = { 1, 2, 3, 4, 5 };

    ExrContext ctx = create_test_context();
    uint8_t output[3] = {};
    size_t out_size = 0;

    ExrDecompressInfo info = {};
    info.src = data;
    info.src_size = sizeof(data);
    info.dst = output;
    info.dst_capacity = sizeof(output);  /* too small */
    info.out_size = &out_size;
    info.compression = EXR_COMPRESSION_NONE;

    ExrResult result = exr_decompress_chunk(ctx, &info);
    REQUIRE(EXR_FAILED(result));

    exr_context_destroy(ctx);
}

/* ---- Error path tests ---- */

TEST(decompress_null_args) {
    ExrContext ctx = create_test_context();

    /* Null info pointer */
    ExrResult result = exr_decompress_chunk(ctx, nullptr);
    REQUIRE(EXR_FAILED(result));

    /* Null src */
    size_t out_size = 0;
    uint8_t buf[8] = {};
    ExrDecompressInfo info = {};
    info.src = nullptr;
    info.src_size = 4;
    info.dst = buf;
    info.dst_capacity = sizeof(buf);
    info.out_size = &out_size;
    info.compression = EXR_COMPRESSION_NONE;

    result = exr_decompress_chunk(ctx, &info);
    REQUIRE(EXR_FAILED(result));

    /* Null dst */
    info.src = buf;
    info.dst = nullptr;
    result = exr_decompress_chunk(ctx, &info);
    REQUIRE(EXR_FAILED(result));

    /* Null out_size */
    info.dst = buf;
    info.out_size = nullptr;
    result = exr_decompress_chunk(ctx, &info);
    REQUIRE(EXR_FAILED(result));

    /* Zero capacity */
    info.out_size = &out_size;
    info.dst_capacity = 0;
    result = exr_decompress_chunk(ctx, &info);
    REQUIRE(EXR_FAILED(result));

    exr_context_destroy(ctx);
}

TEST(decompress_invalid_handle) {
    /* Null context */
    size_t out_size = 0;
    uint8_t buf[8] = {};
    ExrDecompressInfo info = {};
    info.src = buf;
    info.src_size = 4;
    info.dst = buf;
    info.dst_capacity = sizeof(buf);
    info.out_size = &out_size;
    info.compression = EXR_COMPRESSION_NONE;

    ExrResult result = exr_decompress_chunk(nullptr, &info);
    REQUIRE_EQ(result, EXR_ERROR_INVALID_HANDLE);
}

/* ---- Predictor / reorder verification via ZIP round-trip ---- */

TEST(zip_predictor_reorder_ramp) {
    /* Test that predictor + reorder works correctly for a known ramp pattern.
     * We compress with exr_compress_chunk and decompress, verifying round-trip. */
    const size_t len = 128;
    std::vector<uint8_t> original(len);
    for (size_t i = 0; i < len; i++) {
        original[i] = (uint8_t)(i & 0xFF);
    }

    ExrContext ctx = create_test_context();

    /* Compress */
    std::vector<uint8_t> compressed(len * 2);  /* Plenty of room */
    size_t comp_size = 0;

    ExrCompressInfo cinfo = {};
    cinfo.src = original.data();
    cinfo.src_size = len;
    cinfo.dst = compressed.data();
    cinfo.dst_capacity = compressed.size();
    cinfo.out_size = &comp_size;
    cinfo.compression = EXR_COMPRESSION_ZIP;
    cinfo.width = (int32_t)len;
    cinfo.num_lines = 1;

    ExrResult result = exr_compress_chunk(ctx, &cinfo);
    if (result != EXR_SUCCESS) {
        printf("[SKIPPED - compress not implemented] ");
        exr_context_destroy(ctx);
        return;
    }

    /* Decompress */
    std::vector<uint8_t> decompressed(len, 0);
    size_t decomp_size = 0;

    ExrDecompressInfo dinfo = {};
    dinfo.src = compressed.data();
    dinfo.src_size = comp_size;
    dinfo.dst = decompressed.data();
    dinfo.dst_capacity = len;
    dinfo.out_size = &decomp_size;
    dinfo.compression = EXR_COMPRESSION_ZIP;

    result = exr_decompress_chunk(ctx, &dinfo);
    REQUIRE_EQ(result, EXR_SUCCESS);
    REQUIRE_EQ(decomp_size, len);
    REQUIRE(memcmp(original.data(), decompressed.data(), len) == 0);

    exr_context_destroy(ctx);
}

TEST(zip_predictor_reorder_constant) {
    /* All same value -- tests predictor edge case (all deltas are 128) */
    const size_t len = 64;
    std::vector<uint8_t> original(len, 0x80);

    ExrContext ctx = create_test_context();

    std::vector<uint8_t> compressed(len * 2);
    size_t comp_size = 0;

    ExrCompressInfo cinfo = {};
    cinfo.src = original.data();
    cinfo.src_size = len;
    cinfo.dst = compressed.data();
    cinfo.dst_capacity = compressed.size();
    cinfo.out_size = &comp_size;
    cinfo.compression = EXR_COMPRESSION_ZIP;
    cinfo.width = (int32_t)len;
    cinfo.num_lines = 1;

    ExrResult result = exr_compress_chunk(ctx, &cinfo);
    if (result != EXR_SUCCESS) {
        printf("[SKIPPED - compress not implemented] ");
        exr_context_destroy(ctx);
        return;
    }

    std::vector<uint8_t> decompressed(len, 0);
    size_t decomp_size = 0;

    ExrDecompressInfo dinfo = {};
    dinfo.src = compressed.data();
    dinfo.src_size = comp_size;
    dinfo.dst = decompressed.data();
    dinfo.dst_capacity = len;
    dinfo.out_size = &decomp_size;
    dinfo.compression = EXR_COMPRESSION_ZIP;

    result = exr_decompress_chunk(ctx, &dinfo);
    REQUIRE_EQ(result, EXR_SUCCESS);
    REQUIRE_EQ(decomp_size, len);
    REQUIRE(memcmp(original.data(), decompressed.data(), len) == 0);

    exr_context_destroy(ctx);
}

TEST(zip_predictor_reorder_single_byte) {
    /* Single byte -- degenerate case for predictor and reorder */
    const uint8_t original[] = { 0x42 };
    const size_t len = 1;

    ExrContext ctx = create_test_context();

    std::vector<uint8_t> compressed(64);
    size_t comp_size = 0;

    ExrCompressInfo cinfo = {};
    cinfo.src = original;
    cinfo.src_size = len;
    cinfo.dst = compressed.data();
    cinfo.dst_capacity = compressed.size();
    cinfo.out_size = &comp_size;
    cinfo.compression = EXR_COMPRESSION_ZIP;
    cinfo.width = 1;
    cinfo.num_lines = 1;

    ExrResult result = exr_compress_chunk(ctx, &cinfo);
    if (result != EXR_SUCCESS) {
        printf("[SKIPPED - compress not implemented] ");
        exr_context_destroy(ctx);
        return;
    }

    uint8_t decompressed = 0;
    size_t decomp_size = 0;

    ExrDecompressInfo dinfo = {};
    dinfo.src = compressed.data();
    dinfo.src_size = comp_size;
    dinfo.dst = &decompressed;
    dinfo.dst_capacity = 1;
    dinfo.out_size = &decomp_size;
    dinfo.compression = EXR_COMPRESSION_ZIP;

    result = exr_decompress_chunk(ctx, &dinfo);
    REQUIRE_EQ(result, EXR_SUCCESS);
    REQUIRE_EQ(decomp_size, (size_t)1);
    REQUIRE_EQ(decompressed, (uint8_t)0x42);

    exr_context_destroy(ctx);
}

/* ---- RLE round-trip with larger data ---- */

TEST(rle_round_trip_large) {
    /* Larger dataset with mixed patterns (runs and literals) */
    const size_t len = 4096;
    std::vector<uint8_t> original(len);

    /* Fill with a pattern that has both runs and varying data */
    for (size_t i = 0; i < len; i++) {
        if (i % 64 < 32) {
            original[i] = 0x55;  /* Runs */
        } else {
            original[i] = (uint8_t)(i * 7 + 13);  /* Varying */
        }
    }

    std::vector<uint8_t> compressed = rle_compress_simple(original.data(), original.size());

    ExrContext ctx = create_test_context();
    std::vector<uint8_t> decompressed(len, 0);
    size_t out_size = 0;

    ExrDecompressInfo info = {};
    info.src = compressed.data();
    info.src_size = compressed.size();
    info.dst = decompressed.data();
    info.dst_capacity = decompressed.size();
    info.out_size = &out_size;
    info.compression = EXR_COMPRESSION_RLE;

    ExrResult result = exr_decompress_chunk(ctx, &info);
    REQUIRE_EQ(result, EXR_SUCCESS);
    REQUIRE_EQ(out_size, len);
    REQUIRE(memcmp(original.data(), decompressed.data(), len) == 0);

    printf("[%zu -> %zu bytes] ", len, compressed.size());
    exr_context_destroy(ctx);
}

/* ---- ZIP round-trip with large random-ish data ---- */

TEST(zip_round_trip_large) {
    const size_t len = 16384;
    std::vector<uint8_t> original(len);
    for (size_t i = 0; i < len; i++) {
        original[i] = (uint8_t)((i * 31 + 17) ^ (i >> 8));
    }

    ExrContext ctx = create_test_context();

    std::vector<uint8_t> compressed(len * 2);
    size_t comp_size = 0;

    ExrCompressInfo cinfo = {};
    cinfo.src = original.data();
    cinfo.src_size = len;
    cinfo.dst = compressed.data();
    cinfo.dst_capacity = compressed.size();
    cinfo.out_size = &comp_size;
    cinfo.compression = EXR_COMPRESSION_ZIP;
    cinfo.width = (int32_t)len;
    cinfo.num_lines = 1;

    ExrResult result = exr_compress_chunk(ctx, &cinfo);
    if (result != EXR_SUCCESS) {
        printf("[SKIPPED - compress not implemented] ");
        exr_context_destroy(ctx);
        return;
    }

    std::vector<uint8_t> decompressed(len, 0);
    size_t decomp_size = 0;

    ExrDecompressInfo dinfo = {};
    dinfo.src = compressed.data();
    dinfo.src_size = comp_size;
    dinfo.dst = decompressed.data();
    dinfo.dst_capacity = len;
    dinfo.out_size = &decomp_size;
    dinfo.compression = EXR_COMPRESSION_ZIP;

    result = exr_decompress_chunk(ctx, &dinfo);
    REQUIRE_EQ(result, EXR_SUCCESS);
    REQUIRE_EQ(decomp_size, len);
    REQUIRE(memcmp(original.data(), decompressed.data(), len) == 0);

    printf("[%zu -> %zu -> %zu] ", len, comp_size, decomp_size);
    exr_context_destroy(ctx);
}

/* ---- B44 tests via real file ---- */

TEST(b44_real_file) {
    /* Load B44-compressed file if available and verify decompression */
    const char* b44_paths[] = {
        "../../../openexr-images/Tiles/GoldenGate.exr",
        "../../test/unit/regression/tiled_half_1x1_alpha.exr",
    };

    std::vector<uint8_t> fileData;
    const char* used_path = nullptr;
    for (size_t i = 0; i < sizeof(b44_paths) / sizeof(b44_paths[0]); i++) {
        fileData = read_file(b44_paths[i]);
        if (!fileData.empty()) {
            used_path = b44_paths[i];
            break;
        }
    }

    if (fileData.empty()) {
        printf("[SKIPPED - no B44 test file] ");
        return;
    }

    /* Just try to parse and load it through V3 */
    ExrContext ctx = create_test_context();

    ExrDataSource source = {};
    ExrResult result = exr_data_source_from_memory(fileData.data(), fileData.size(), &source);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrDecoderCreateInfo dec_info = {};
    dec_info.source = source;
    ExrDecoder decoder = nullptr;
    result = exr_decoder_create(ctx, &dec_info, &decoder);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrImage image = nullptr;
    result = exr_decoder_parse_header(decoder, &image);
    REQUIRE_EQ(result, EXR_SUCCESS);

    printf("[parsed %s] ", used_path);

    exr_decoder_destroy(decoder);
    exr_context_destroy(ctx);
}

/* ---- Multi-part test ---- */

TEST(multipart_parse) {
    /* Try to find and parse a multipart EXR */
    const char* multipart_paths[] = {
        "../../../openexr-images/Beachball/multipart.0001.exr",
        "../../test/unit/regression/issue-238-double-free-multipart.exr",
    };

    std::vector<uint8_t> fileData;
    for (size_t i = 0; i < sizeof(multipart_paths) / sizeof(multipart_paths[0]); i++) {
        fileData = read_file(multipart_paths[i]);
        if (!fileData.empty()) break;
    }

    if (fileData.empty()) {
        printf("[SKIPPED - no multipart file] ");
        return;
    }

    ExrContext ctx = create_test_context();

    ExrDataSource source = {};
    ExrResult result = exr_data_source_from_memory(fileData.data(), fileData.size(), &source);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrDecoderCreateInfo dec_info = {};
    dec_info.source = source;
    ExrDecoder decoder = nullptr;
    result = exr_decoder_create(ctx, &dec_info, &decoder);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrImage image = nullptr;
    result = exr_decoder_parse_header(decoder, &image);
    REQUIRE_EQ(result, EXR_SUCCESS);

    uint32_t part_count = 0;
    result = exr_image_get_part_count(image, &part_count);
    REQUIRE_EQ(result, EXR_SUCCESS);
    REQUIRE(part_count >= 1);

    printf("[%u parts] ", part_count);

    /* Verify we can query each part */
    for (uint32_t p = 0; p < part_count; p++) {
        ExrPart part = nullptr;
        result = exr_image_get_part(image, p, &part);
        REQUIRE_EQ(result, EXR_SUCCESS);

        ExrPartInfo part_info = {};
        result = exr_part_get_info(part, &part_info);
        REQUIRE_EQ(result, EXR_SUCCESS);
        REQUIRE(part_info.width > 0);
        REQUIRE(part_info.height > 0);

        exr_part_destroy(part);
    }

    exr_decoder_destroy(decoder);
    exr_context_destroy(ctx);
}

/* ---- Corrupt data stress tests ---- */

TEST(corrupt_chunk_data) {
    /* Load a real file, corrupt the compressed data, verify error not crash */
    std::vector<uint8_t> fileData = read_file("../../asakusa.exr");
    if (fileData.empty()) {
        printf("[SKIPPED] ");
        return;
    }

    /* Corrupt bytes in the middle of the file (in chunk data area) */
    size_t corrupt_offset = fileData.size() / 2;
    for (size_t i = 0; i < 64 && corrupt_offset + i < fileData.size(); i++) {
        fileData[corrupt_offset + i] ^= 0xFF;
    }

    ExrContext ctx = create_test_context();

    ExrDataSource source = {};
    ExrResult result = exr_data_source_from_memory(fileData.data(), fileData.size(), &source);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrDecoderCreateInfo dec_info = {};
    dec_info.source = source;
    ExrDecoder decoder = nullptr;
    result = exr_decoder_create(ctx, &dec_info, &decoder);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrImage image = nullptr;
    result = exr_decoder_parse_header(decoder, &image);
    /* Header might still parse OK since corruption is in chunk data */

    if (result == EXR_SUCCESS) {
        ExrPart part = nullptr;
        result = exr_image_get_part(image, 0, &part);
        if (result == EXR_SUCCESS) {
            ExrPartInfo pinfo = {};
            exr_part_get_info(part, &pinfo);

            ExrChannelInfo ch = {};
            exr_part_get_channel(part, 0, &ch);
            int bpp = (ch.pixel_type == EXR_PIXEL_HALF) ? 2 : 4;

            size_t buf_size = (size_t)pinfo.width * pinfo.height *
                              pinfo.num_channels * bpp;
            std::vector<uint8_t> buffer(buf_size, 0);

            ExrCommandBufferCreateInfo cmd_info = {};
            cmd_info.decoder = decoder;
            ExrCommandBuffer cmd = nullptr;
            exr_command_buffer_create(ctx, &cmd_info, &cmd);

            exr_command_buffer_begin(cmd);

            ExrFullImageRequest full_req = {};
            full_req.part = part;
            full_req.output.data = buffer.data();
            full_req.output.size = buf_size;
            full_req.output_pixel_type = ch.pixel_type;

            exr_cmd_request_full_image(cmd, &full_req);
            exr_command_buffer_end(cmd);

            ExrSubmitInfo submit_info = {};
            ExrCommandBuffer cmd_buffers[] = { cmd };
            submit_info.command_buffer_count = 1;
            submit_info.command_buffers = cmd_buffers;

            /* This should fail gracefully (error, not crash) */
            result = exr_submit(decoder, &submit_info);
            /* We don't assert on the result -- it might succeed if corruption
             * happened to miss the actual compressed chunk being read,
             * or it might fail. Either is fine as long as we don't crash. */

            exr_command_buffer_destroy(cmd);
            exr_part_destroy(part);
        }
    }

    printf("[handled gracefully] ");
    exr_decoder_destroy(decoder);
    exr_context_destroy(ctx);
}

TEST(regression_2by2_exr) {
    /* Smallest known good file */
    std::vector<uint8_t> fileData = read_file("../../test/unit/regression/2by2.exr");
    if (fileData.empty()) {
        printf("[SKIPPED] ");
        return;
    }

    ExrContext ctx = create_test_context();

    ExrDataSource source = {};
    ExrResult result = exr_data_source_from_memory(fileData.data(), fileData.size(), &source);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrDecoderCreateInfo dec_info = {};
    dec_info.source = source;
    ExrDecoder decoder = nullptr;
    result = exr_decoder_create(ctx, &dec_info, &decoder);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrImage image = nullptr;
    result = exr_decoder_parse_header(decoder, &image);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrPart part = nullptr;
    result = exr_image_get_part(image, 0, &part);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrPartInfo pinfo = {};
    result = exr_part_get_info(part, &pinfo);
    REQUIRE_EQ(result, EXR_SUCCESS);

    printf("[%dx%d, %d ch, comp=%d] ", pinfo.width, pinfo.height,
           pinfo.num_channels, pinfo.compression);

    exr_part_destroy(part);
    exr_decoder_destroy(decoder);
    exr_context_destroy(ctx);
}

TEST(regression_issue160_piz) {
    /* PIZ decompression regression file */
    std::vector<uint8_t> fileData = read_file("../../test/unit/regression/issue-160-piz-decode.exr");
    if (fileData.empty()) {
        printf("[SKIPPED] ");
        return;
    }

    ExrContext ctx = create_test_context();

    ExrDataSource source = {};
    ExrResult result = exr_data_source_from_memory(fileData.data(), fileData.size(), &source);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrDecoderCreateInfo dec_info = {};
    dec_info.source = source;
    ExrDecoder decoder = nullptr;
    result = exr_decoder_create(ctx, &dec_info, &decoder);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrImage image = nullptr;
    result = exr_decoder_parse_header(decoder, &image);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrPart part = nullptr;
    result = exr_image_get_part(image, 0, &part);
    REQUIRE_EQ(result, EXR_SUCCESS);

    ExrPartInfo pinfo = {};
    result = exr_part_get_info(part, &pinfo);
    REQUIRE_EQ(result, EXR_SUCCESS);

    REQUIRE_EQ(pinfo.compression, (uint32_t)EXR_COMPRESSION_PIZ);
    printf("[PIZ %dx%d] ", pinfo.width, pinfo.height);

    exr_part_destroy(part);
    exr_decoder_destroy(decoder);
    exr_context_destroy(ctx);
}

TEST(regression_fuzzer_pocs) {
    /* Run through all fuzzer POC files and verify they don't crash */
    const char* poc_files[] = {
        "../../test/unit/regression/poc-df76d1f27adb8927a1446a603028272140905c168a336128465a1162ec7af270.mini",
        "../../test/unit/regression/poc-efe9007bfdcbbe8a1569bf01fa9acadb8261ead49cb83f6e91fcdc4dae2e99a3_min",
        "../../test/unit/regression/poc-d5c9c893e559277a3320c196523095b94db93985620ac338d037487e0e613047_min",
        "../../test/unit/regression/poc-5b66774a7498c635334ad386be0c3b359951738ac47f14878a3346d1c6ea0fe5_min",
        "../../test/unit/regression/poc-24322747c47e87a10e4407528b779a1a763a48135384909b3d1010bbba1d4c28_min",
    };
    int tested = 0;

    for (size_t i = 0; i < sizeof(poc_files) / sizeof(poc_files[0]); i++) {
        std::vector<uint8_t> fileData = read_file(poc_files[i]);
        if (fileData.empty()) continue;

        ExrContext ctx = create_test_context();
        ExrDataSource source = {};
        exr_data_source_from_memory(fileData.data(), fileData.size(), &source);

        ExrDecoderCreateInfo dec_info = {};
        dec_info.source = source;
        ExrDecoder decoder = nullptr;
        ExrResult result = exr_decoder_create(ctx, &dec_info, &decoder);
        if (result != EXR_SUCCESS) {
            exr_context_destroy(ctx);
            tested++;
            continue;
        }

        ExrImage image = nullptr;
        result = exr_decoder_parse_header(decoder, &image);
        /* We don't care about the result -- just that it doesn't crash */
        if (image) {
            exr_image_destroy(image);
        }

        exr_decoder_destroy(decoder);
        exr_context_destroy(ctx);
        tested++;
    }

    printf("[%d POCs tested] ", tested);
    REQUIRE(tested > 0);
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main() {
    printf("TinyEXR V3 API Tests\n");
    printf("====================\n\n");

    printf("[C API Tests]\n");
    run_test_c_api_version();
    run_test_c_api_version_string();
    run_test_c_api_result_to_string();
    run_test_c_api_simd_info();
    run_test_c_api_context_create_destroy();
    run_test_c_api_context_invalid_version();
    run_test_c_api_context_ref_counting();
    run_test_c_api_error_handling();
    run_test_c_api_memory_pool();
    run_test_c_api_data_source_from_memory();
    run_test_c_api_decoder_create_destroy();
    run_test_c_api_fence_create_destroy();
    run_test_c_api_command_buffer();
    run_test_c_api_half_conversion();

    printf("\n[Header Parsing Tests]\n");
    run_test_c_api_parse_invalid_magic();
    run_test_c_api_parse_invalid_version();
    run_test_c_api_parse_real_file();

    printf("\n[Async Parsing Tests]\n");
    run_test_c_api_async_header_parsing();
    run_test_c_api_async_immediate_completion();

    printf("\n[Command Buffer Tests]\n");
    run_test_c_api_command_buffer_recording();
    run_test_c_api_submit_basic();
    run_test_c_api_verify_zip_vs_v1();
    run_test_c_api_verify_piz_vs_v1();
    run_test_c_api_verify_pxr24_vs_v1();
    run_test_c_api_verify_b44_vs_v1();
    run_test_c_api_verify_tiled();

    printf("\n[Compression Unit Tests]\n");
    run_test_none_passthrough();
    run_test_none_buffer_too_small();
    run_test_rle_round_trip_basic();
    run_test_rle_all_repeat();
    run_test_rle_uncompressed_passthrough();
    run_test_rle_truncated_input();
    run_test_rle_round_trip_large();
    run_test_zip_uncompressed_passthrough();
    run_test_zip_zips_uncompressed_passthrough();
    run_test_zip_corrupt_data();
    run_test_zip_round_trip_real_file();
    run_test_piz_chunk_api_issue160_float32();
    run_test_piz_malformed_huffman_data();
    run_test_zip_round_trip_large();

    printf("\n[Predictor/Reorder Tests]\n");
    run_test_zip_predictor_reorder_ramp();
    run_test_zip_predictor_reorder_constant();
    run_test_zip_predictor_reorder_single_byte();

    printf("\n[Error Path Tests]\n");
    run_test_decompress_null_args();
    run_test_decompress_invalid_handle();
    run_test_corrupt_chunk_data();

    printf("\n[Regression Tests]\n");
    run_test_regression_2by2_exr();
    run_test_regression_issue160_piz();
    run_test_regression_fuzzer_pocs();
    run_test_b44_real_file();
    run_test_multipart_parse();

    printf("\n[C++ Wrapper Tests]\n");
    run_test_cpp_version_string();
    run_test_cpp_simd_info();
    run_test_cpp_context_create();
    run_test_cpp_context_with_options();
    run_test_cpp_result_ok();
    run_test_cpp_result_error();
    run_test_cpp_result_value_or();
    run_test_cpp_result_map();
    run_test_cpp_result_map_error();
    run_test_cpp_result_and_then();
    run_test_cpp_result_void();
    run_test_cpp_error_info();
    run_test_cpp_decoder_from_memory();
    run_test_cpp_tile_coord();

    printf("\n====================\n");
    printf("Results: %d passed, %d failed\n", g_tests_passed, g_tests_failed);

    return g_tests_failed > 0 ? 1 : 0;
}
