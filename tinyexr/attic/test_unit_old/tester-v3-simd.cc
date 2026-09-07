// TinyEXR V3 SIMD Unit Tests
//
// Build:
//   g++ -std=c++17 -O2 -msse2 -DTINYEXR_ENABLE_SIMD=1 -DTINYEXR_V3_USE_SIMD=1 \
//       -I../.. -o tester-v3-simd tester-v3-simd.cc ../../tinyexr_simd_wrapper.cc
//
// With AVX2:
//   g++ -std=c++17 -O2 -mavx2 -mf16c -DTINYEXR_ENABLE_SIMD=1 -DTINYEXR_V3_USE_SIMD=1 \
//       -I../.. -o tester-v3-simd tester-v3-simd.cc ../../tinyexr_simd_wrapper.cc

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <cstdint>
#include <chrono>

#include "tinyexr_simd_c.h"

// Scalar reference implementations for comparison
namespace reference {

static uint32_t g_mantissa_table[2048];
static uint32_t g_exponent_table[64];
static uint32_t g_offset_table[64];
static uint32_t g_base_table[512];
static uint32_t g_shift_table[512];
static bool g_tables_initialized = false;

void init_half_tables() {
    if (g_tables_initialized) return;

    // Initialize mantissa table
    g_mantissa_table[0] = 0;
    for (int i = 1; i < 1024; i++) {
        int m = i << 13;
        int e = 0;
        while ((m & 0x00800000) == 0) {
            e -= 0x00800000;
            m <<= 1;
        }
        m &= ~0x00800000;
        e += 0x38800000;
        g_mantissa_table[i] = (uint32_t)(m | e);
    }
    for (int i = 1024; i < 2048; i++) {
        g_mantissa_table[i] = (uint32_t)(0x38000000 + ((i - 1024) << 13));
    }

    // Initialize exponent table
    g_exponent_table[0] = 0;
    g_exponent_table[31] = 0x47800000;
    g_exponent_table[32] = 0x80000000;
    g_exponent_table[63] = 0xC7800000;
    for (int i = 1; i < 31; i++) {
        g_exponent_table[i] = (uint32_t)(i << 23);
    }
    for (int i = 33; i < 63; i++) {
        g_exponent_table[i] = (uint32_t)(0x80000000 + ((i - 32) << 23));
    }

    // Initialize offset table
    g_offset_table[0] = 0;
    g_offset_table[32] = 0;
    for (int i = 1; i < 32; i++) {
        g_offset_table[i] = 1024;
    }
    for (int i = 33; i < 64; i++) {
        g_offset_table[i] = 1024;
    }
    g_offset_table[31] = 1024;
    g_offset_table[63] = 1024;

    // Initialize base and shift tables for float-to-half
    for (int i = 0; i < 256; i++) {
        int e = i - 127;
        if (e < -24) {
            g_base_table[i] = 0;
            g_shift_table[i] = 24;
            g_base_table[i | 0x100] = 0x8000;
            g_shift_table[i | 0x100] = 24;
        } else if (e < -14) {
            g_base_table[i] = (0x0400 >> (-e - 14));
            g_shift_table[i] = -e - 1;
            g_base_table[i | 0x100] = (0x0400 >> (-e - 14)) | 0x8000;
            g_shift_table[i | 0x100] = -e - 1;
        } else if (e <= 15) {
            g_base_table[i] = ((e + 15) << 10);
            g_shift_table[i] = 13;
            g_base_table[i | 0x100] = ((e + 15) << 10) | 0x8000;
            g_shift_table[i | 0x100] = 13;
        } else if (e < 128) {
            g_base_table[i] = 0x7C00;
            g_shift_table[i] = 24;
            g_base_table[i | 0x100] = 0xFC00;
            g_shift_table[i | 0x100] = 24;
        } else {
            g_base_table[i] = 0x7C00;
            g_shift_table[i] = 13;
            g_base_table[i | 0x100] = 0xFC00;
            g_shift_table[i | 0x100] = 13;
        }
    }

    g_tables_initialized = true;
}

void half_to_float(const uint16_t* src, float* dst, size_t count) {
    init_half_tables();
    for (size_t i = 0; i < count; i++) {
        uint16_t h = src[i];
        uint32_t f = g_mantissa_table[g_offset_table[h >> 10] + (h & 0x3FF)] +
                     g_exponent_table[h >> 10];
        memcpy(&dst[i], &f, sizeof(float));
    }
}

void float_to_half(const float* src, uint16_t* dst, size_t count) {
    init_half_tables();
    for (size_t i = 0; i < count; i++) {
        uint32_t f;
        memcpy(&f, &src[i], sizeof(float));
        uint32_t sign = (f >> 16) & 0x8000;
        uint32_t exp = (f >> 23) & 0xFF;
        uint32_t sign_idx = (f >> 23) & 0x100;
        dst[i] = (uint16_t)(g_base_table[exp | sign_idx] +
                            ((f & 0x007FFFFF) >> g_shift_table[exp | sign_idx]));
        dst[i] |= (uint16_t)sign;
    }
}

void interleave_rgba(const float* r, const float* g, const float* b, const float* a,
                      float* rgba, size_t count) {
    for (size_t i = 0; i < count; i++) {
        rgba[i * 4 + 0] = r ? r[i] : 0.0f;
        rgba[i * 4 + 1] = g ? g[i] : 0.0f;
        rgba[i * 4 + 2] = b ? b[i] : 0.0f;
        rgba[i * 4 + 3] = a ? a[i] : 1.0f;
    }
}

void deinterleave_rgba(const float* rgba, float* r, float* g, float* b, float* a,
                        size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (r) r[i] = rgba[i * 4 + 0];
        if (g) g[i] = rgba[i * 4 + 1];
        if (b) b[i] = rgba[i * 4 + 2];
        if (a) a[i] = rgba[i * 4 + 3];
    }
}

void reorder_bytes(const uint8_t* src, uint8_t* dst, size_t size) {
    size_t half = (size + 1) / 2;
    uint8_t* t1 = dst;
    uint8_t* t2 = dst + half;
    const uint8_t* s = src;
    const uint8_t* stop = src + size;
    while (s < stop) {
        if (s < stop) *t1++ = *s++;
        if (s < stop) *t2++ = *s++;
    }
}

void delta_encode(uint8_t* data, size_t size) {
    if (size < 2) return;
    uint8_t* end = data + size - 1;
    while (end > data) {
        int d = (int)end[0] - (int)end[-1] + 128;
        end[0] = (uint8_t)d;
        --end;
    }
}

}  // namespace reference

// Test framework
static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST(name) \
    do { \
        printf("  [TEST] %s... ", #name); \
        fflush(stdout); \
    } while(0)

#define PASS() \
    do { \
        printf("PASSED\n"); \
        g_tests_passed++; \
    } while(0)

#define FAIL(msg) \
    do { \
        printf("FAILED: %s\n", msg); \
        g_tests_failed++; \
    } while(0)

// ============================================================================
// Tests
// ============================================================================

void test_simd_info() {
    TEST(simd_info);
    const char* info = exr_simd_get_info();
    if (info && strlen(info) > 0) {
        printf("[%s] ", info);
        PASS();
    } else {
        FAIL("No SIMD info returned");
    }
}

void test_half_to_float_correctness() {
    TEST(half_to_float_correctness);

    // Test various half values
    std::vector<uint16_t> test_values = {
        0x0000,  // +0
        0x8000,  // -0
        0x3C00,  // 1.0
        0xBC00,  // -1.0
        0x4000,  // 2.0
        0x3800,  // 0.5
        0x7C00,  // +inf
        0xFC00,  // -inf
        0x7E00,  // NaN
        0x0001,  // Smallest denormal
        0x0400,  // Smallest normal
        0x7BFF,  // Largest finite
    };

    // Add random values
    for (int i = 0; i < 100; i++) {
        test_values.push_back((uint16_t)(rand() & 0x7FFF));  // Random positive
        test_values.push_back((uint16_t)(rand() | 0x8000));  // Random negative
    }

    std::vector<float> ref_out(test_values.size());
    std::vector<float> simd_out(test_values.size());

    reference::half_to_float(test_values.data(), ref_out.data(), test_values.size());
    exr_simd_half_to_float(test_values.data(), simd_out.data(), test_values.size());

    bool match = true;
    for (size_t i = 0; i < test_values.size(); i++) {
        uint32_t ref_bits, simd_bits;
        memcpy(&ref_bits, &ref_out[i], 4);
        memcpy(&simd_bits, &simd_out[i], 4);
        if (ref_bits != simd_bits) {
            // NaN comparison: both should be NaN
            if (std::isnan(ref_out[i]) && std::isnan(simd_out[i])) continue;
            printf("\n    Mismatch at %zu: half=0x%04X ref=0x%08X simd=0x%08X",
                   i, test_values[i], ref_bits, simd_bits);
            match = false;
        }
    }

    if (match) PASS(); else FAIL("Value mismatch");
}

void test_float_to_half_correctness() {
    TEST(float_to_half_correctness);

    std::vector<float> test_values = {
        0.0f, -0.0f, 1.0f, -1.0f, 2.0f, 0.5f, 0.25f,
        65504.0f,  // Max half
        -65504.0f,
        6.1035156e-5f,  // Min normal half
        5.96046e-8f,    // Denormal
    };

    // Add random values
    for (int i = 0; i < 100; i++) {
        float v = ((float)rand() / RAND_MAX) * 200.0f - 100.0f;
        test_values.push_back(v);
    }

    std::vector<uint16_t> ref_out(test_values.size());
    std::vector<uint16_t> simd_out(test_values.size());

    reference::float_to_half(test_values.data(), ref_out.data(), test_values.size());
    exr_simd_float_to_half(test_values.data(), simd_out.data(), test_values.size());

    bool match = true;
    for (size_t i = 0; i < test_values.size(); i++) {
        if (ref_out[i] != simd_out[i]) {
            // Allow 1 bit difference for rounding
            int diff = abs((int)ref_out[i] - (int)simd_out[i]);
            if (diff > 1) {
                printf("\n    Mismatch at %zu: float=%f ref=0x%04X simd=0x%04X",
                       i, test_values[i], ref_out[i], simd_out[i]);
                match = false;
            }
        }
    }

    if (match) PASS(); else FAIL("Value mismatch");
}

void test_interleave_rgba() {
    TEST(interleave_rgba);

    const size_t count = 1000;
    std::vector<float> r(count), g(count), b(count), a(count);
    std::vector<float> ref_rgba(count * 4);
    std::vector<float> simd_rgba(count * 4);

    for (size_t i = 0; i < count; i++) {
        r[i] = (float)i / count;
        g[i] = (float)(count - i) / count;
        b[i] = 0.5f;
        a[i] = 1.0f;
    }

    reference::interleave_rgba(r.data(), g.data(), b.data(), a.data(),
                                ref_rgba.data(), count);
    exr_simd_interleave_rgba(r.data(), g.data(), b.data(), a.data(),
                              simd_rgba.data(), count);

    bool match = memcmp(ref_rgba.data(), simd_rgba.data(), count * 4 * sizeof(float)) == 0;
    if (match) PASS(); else FAIL("Data mismatch");
}

void test_deinterleave_rgba() {
    TEST(deinterleave_rgba);

    const size_t count = 1000;
    std::vector<float> rgba(count * 4);
    std::vector<float> ref_r(count), ref_g(count), ref_b(count), ref_a(count);
    std::vector<float> simd_r(count), simd_g(count), simd_b(count), simd_a(count);

    for (size_t i = 0; i < count; i++) {
        rgba[i * 4 + 0] = (float)i / count;
        rgba[i * 4 + 1] = (float)(count - i) / count;
        rgba[i * 4 + 2] = 0.5f;
        rgba[i * 4 + 3] = 1.0f;
    }

    reference::deinterleave_rgba(rgba.data(), ref_r.data(), ref_g.data(),
                                  ref_b.data(), ref_a.data(), count);
    exr_simd_deinterleave_rgba(rgba.data(), simd_r.data(), simd_g.data(),
                                simd_b.data(), simd_a.data(), count);

    bool match = true;
    match &= memcmp(ref_r.data(), simd_r.data(), count * sizeof(float)) == 0;
    match &= memcmp(ref_g.data(), simd_g.data(), count * sizeof(float)) == 0;
    match &= memcmp(ref_b.data(), simd_b.data(), count * sizeof(float)) == 0;
    match &= memcmp(ref_a.data(), simd_a.data(), count * sizeof(float)) == 0;

    if (match) PASS(); else FAIL("Data mismatch");
}

void test_reorder_bytes() {
    TEST(reorder_bytes);

    const size_t size = 1000;
    std::vector<uint8_t> src(size);
    std::vector<uint8_t> ref_dst(size);
    std::vector<uint8_t> simd_dst(size);

    for (size_t i = 0; i < size; i++) {
        src[i] = (uint8_t)(i & 0xFF);
    }

    reference::reorder_bytes(src.data(), ref_dst.data(), size);
    exr_simd_reorder_bytes(src.data(), simd_dst.data(), size);

    bool match = memcmp(ref_dst.data(), simd_dst.data(), size) == 0;
    if (match) PASS(); else FAIL("Data mismatch");
}

void test_delta_encode() {
    TEST(delta_encode);

    const size_t size = 1000;
    std::vector<uint8_t> ref_data(size);
    std::vector<uint8_t> simd_data(size);

    for (size_t i = 0; i < size; i++) {
        ref_data[i] = simd_data[i] = (uint8_t)(i * 7 + 13);
    }

    reference::delta_encode(ref_data.data(), size);
    exr_simd_delta_encode(simd_data.data(), size);

    bool match = memcmp(ref_data.data(), simd_data.data(), size) == 0;
    if (match) PASS(); else FAIL("Data mismatch");
}

void benchmark_half_to_float() {
    TEST(benchmark_half_to_float);

    const size_t count = 1000000;
    std::vector<uint16_t> src(count);
    std::vector<float> dst(count);

    for (size_t i = 0; i < count; i++) {
        src[i] = (uint16_t)(rand() & 0x7FFF);
    }

    // Warmup
    for (int i = 0; i < 3; i++) {
        exr_simd_half_to_float(src.data(), dst.data(), count);
    }

    // Benchmark SIMD
    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < 100; i++) {
        exr_simd_half_to_float(src.data(), dst.data(), count);
    }
    auto end = std::chrono::high_resolution_clock::now();
    double simd_ms = std::chrono::duration<double, std::milli>(end - start).count();

    // Benchmark reference
    start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < 100; i++) {
        reference::half_to_float(src.data(), dst.data(), count);
    }
    end = std::chrono::high_resolution_clock::now();
    double ref_ms = std::chrono::duration<double, std::milli>(end - start).count();

    double speedup = ref_ms / simd_ms;
    printf("[%.1fx speedup] ", speedup);
    PASS();
}

void benchmark_interleave_rgba() {
    TEST(benchmark_interleave_rgba);

    const size_t count = 1000000;
    std::vector<float> r(count), g(count), b(count), a(count);
    std::vector<float> rgba(count * 4);

    for (size_t i = 0; i < count; i++) {
        r[i] = g[i] = b[i] = a[i] = 1.0f;
    }

    // Warmup
    for (int i = 0; i < 3; i++) {
        exr_simd_interleave_rgba(r.data(), g.data(), b.data(), a.data(),
                                  rgba.data(), count);
    }

    // Benchmark SIMD
    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < 100; i++) {
        exr_simd_interleave_rgba(r.data(), g.data(), b.data(), a.data(),
                                  rgba.data(), count);
    }
    auto end = std::chrono::high_resolution_clock::now();
    double simd_ms = std::chrono::duration<double, std::milli>(end - start).count();

    // Benchmark reference
    start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < 100; i++) {
        reference::interleave_rgba(r.data(), g.data(), b.data(), a.data(),
                                    rgba.data(), count);
    }
    end = std::chrono::high_resolution_clock::now();
    double ref_ms = std::chrono::duration<double, std::milli>(end - start).count();

    double speedup = ref_ms / simd_ms;
    printf("[%.1fx speedup] ", speedup);
    PASS();
}

// ============================================================================
// Main
// ============================================================================

int main() {
    printf("TinyEXR V3 SIMD Unit Tests\n");
    printf("==========================\n\n");

    printf("[Correctness Tests]\n");
    test_simd_info();
    test_half_to_float_correctness();
    test_float_to_half_correctness();
    test_interleave_rgba();
    test_deinterleave_rgba();
    test_reorder_bytes();
    test_delta_encode();

    printf("\n[Benchmark Tests]\n");
    benchmark_half_to_float();
    benchmark_interleave_rgba();

    printf("\n==========================\n");
    printf("Results: %d passed, %d failed\n", g_tests_passed, g_tests_failed);

    return g_tests_failed > 0 ? 1 : 0;
}
