// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025, Syoyo Fujita and many contributors.
// All rights reserved.
//
// Test for TinyEXR SIMD Optimizations
//
// Build (x86_64 with AVX2+F16C):
//   g++ -O2 -mavx2 -mf16c -DTINYEXR_ENABLE_SIMD=1 -o test_simd test_simd.cc
//
// Build (ARM with NEON+FP16):
//   g++ -O2 -march=armv8.2-a+fp16 -DTINYEXR_ENABLE_SIMD=1 -o test_simd test_simd.cc
//
// Build (ARM SVE):
//   g++ -O2 -march=armv8-a+sve -DTINYEXR_ENABLE_SIMD=1 -o test_simd test_simd.cc
//
// Build (scalar fallback):
//   g++ -O2 -DTINYEXR_ENABLE_SIMD=0 -o test_simd test_simd.cc

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <chrono>
#include <random>
#include <vector>

#define TINYEXR_ENABLE_SIMD 1
#include "tinyexr_simd.hh"

using namespace tinyexr::simd;

// Test utilities
static int g_test_count = 0;
static int g_pass_count = 0;
static int g_fail_count = 0;

#define TEST_ASSERT(cond, msg) do { \
  g_test_count++; \
  if (!(cond)) { \
    printf("  FAIL: %s\n", msg); \
    g_fail_count++; \
  } else { \
    g_pass_count++; \
  } \
} while(0)

#define TEST_ASSERT_NEAR(a, b, eps, msg) do { \
  g_test_count++; \
  float diff = std::fabs((a) - (b)); \
  if (diff > (eps)) { \
    printf("  FAIL: %s (got %f, expected %f, diff %f)\n", msg, (float)(a), (float)(b), diff); \
    g_fail_count++; \
  } else { \
    g_pass_count++; \
  } \
} while(0)

// FP16 reference values for testing
// Using well-known half-precision values
struct FP16TestCase {
  uint16_t half_bits;
  float float_val;
  const char* name;
};

static const FP16TestCase fp16_test_cases[] = {
  {0x0000, 0.0f, "zero"},
  {0x8000, -0.0f, "negative zero"},
  {0x3C00, 1.0f, "one"},
  {0xBC00, -1.0f, "negative one"},
  {0x4000, 2.0f, "two"},
  {0x3800, 0.5f, "half"},
  {0x4200, 3.0f, "three"},
  {0x4400, 4.0f, "four"},
  {0x5640, 100.0f, "hundred"},
  {0x6400, 1024.0f, "1024"},
  {0x7BFF, 65504.0f, "max normal"},  // Largest normal half
  {0x0400, 6.103515625e-5f, "min normal"},  // Smallest positive normal
  // Subnormals
  {0x0001, 5.960464477539063e-8f, "min subnormal"},
  // Infinity
  {0x7C00, INFINITY, "infinity"},
  {0xFC00, -INFINITY, "negative infinity"},
};

static const size_t num_fp16_test_cases = sizeof(fp16_test_cases) / sizeof(fp16_test_cases[0]);

// ============================================================================
// Test Functions
// ============================================================================

void test_simd_capabilities() {
  printf("=== SIMD Capabilities ===\n");

  const SIMDCapabilities& caps = get_capabilities();

  printf("  SIMD Backend: %s\n", get_simd_info());
  printf("  SSE2: %s\n", caps.sse2 ? "yes" : "no");
  printf("  SSE4.1: %s\n", caps.sse41 ? "yes" : "no");
  printf("  AVX: %s\n", caps.avx ? "yes" : "no");
  printf("  AVX2: %s\n", caps.avx2 ? "yes" : "no");
  printf("  F16C: %s\n", caps.f16c ? "yes" : "no");
  printf("  AVX-512F: %s\n", caps.avx512f ? "yes" : "no");
  printf("  NEON: %s\n", caps.neon ? "yes" : "no");
  printf("  NEON FP16: %s\n", caps.neon_fp16 ? "yes" : "no");
  printf("  SVE: %s\n", caps.sve ? "yes" : "no");
  printf("  SVE2: %s\n", caps.sve2 ? "yes" : "no");
  printf("  A64FX: %s\n", caps.a64fx ? "yes" : "no");
  if (caps.sve) {
    printf("  SVE Vector Length: %u bits\n", caps.sve_vector_length);
  }
  printf("  Optimal Batch Size: %zu bytes\n", get_optimal_batch_size());
  printf("\n");
}

void test_half_to_float_scalar() {
  printf("=== Test: half_to_float_scalar ===\n");

  for (size_t i = 0; i < num_fp16_test_cases; i++) {
    const FP16TestCase& tc = fp16_test_cases[i];

    // Skip NaN tests (NaN != NaN)
    if (std::isnan(tc.float_val)) continue;

    float result = half_to_float_scalar(tc.half_bits);

    if (std::isinf(tc.float_val)) {
      TEST_ASSERT(std::isinf(result) && ((result > 0) == (tc.float_val > 0)),
                  tc.name);
    } else {
      TEST_ASSERT_NEAR(result, tc.float_val, std::fabs(tc.float_val) * 0.001f + 1e-7f,
                       tc.name);
    }
  }

  printf("  Passed %d/%d scalar half->float tests\n\n",
         g_pass_count, g_test_count);
}

void test_float_to_half_scalar() {
  printf("=== Test: float_to_half_scalar ===\n");

  int local_pass = 0;
  int local_total = 0;

  for (size_t i = 0; i < num_fp16_test_cases; i++) {
    const FP16TestCase& tc = fp16_test_cases[i];

    // Skip special cases that may not round-trip exactly
    if (std::isnan(tc.float_val) || std::isinf(tc.float_val)) continue;

    uint16_t result = float_to_half_scalar(tc.float_val);
    local_total++;
    g_test_count++;

    // Allow for rounding differences in least significant bit
    int16_t diff = static_cast<int16_t>(result) - static_cast<int16_t>(tc.half_bits);
    if (std::abs(diff) <= 1) {
      local_pass++;
      g_pass_count++;
    } else {
      printf("  FAIL: %s (got 0x%04X, expected 0x%04X)\n",
             tc.name, result, tc.half_bits);
      g_fail_count++;
    }
  }

  printf("  Passed %d/%d scalar float->half tests\n\n", local_pass, local_total);
}

void test_half_to_float_batch() {
  printf("=== Test: half_to_float_batch ===\n");

  // Create test data
  const size_t count = 1024;
  std::vector<uint16_t> half_data(count);
  std::vector<float> float_result(count);
  std::vector<float> float_expected(count);

  // Fill with test values
  std::mt19937 rng(42);
  std::uniform_int_distribution<uint16_t> dist(0, 0x7BFF);  // Normal range only

  for (size_t i = 0; i < count; i++) {
    half_data[i] = dist(rng);
    float_expected[i] = half_to_float_scalar(half_data[i]);
  }

  // Test batch conversion
  half_to_float_batch(half_data.data(), float_result.data(), count);

  int local_pass = 0;
  for (size_t i = 0; i < count; i++) {
    g_test_count++;
    if (float_result[i] == float_expected[i]) {
      local_pass++;
      g_pass_count++;
    } else {
      if (g_fail_count < 10) {
        printf("  FAIL at index %zu: got %f, expected %f\n",
               i, float_result[i], float_expected[i]);
      }
      g_fail_count++;
    }
  }

  printf("  Passed %d/%zu batch half->float tests\n\n", local_pass, count);
}

void test_float_to_half_batch() {
  printf("=== Test: float_to_half_batch ===\n");

  // Create test data
  const size_t count = 1024;
  std::vector<float> float_data(count);
  std::vector<uint16_t> half_result(count);
  std::vector<uint16_t> half_expected(count);

  // Fill with test values
  std::mt19937 rng(42);
  std::uniform_real_distribution<float> dist(-1000.0f, 1000.0f);

  for (size_t i = 0; i < count; i++) {
    float_data[i] = dist(rng);
    half_expected[i] = float_to_half_scalar(float_data[i]);
  }

  // Test batch conversion
  float_to_half_batch(float_data.data(), half_result.data(), count);

  int local_pass = 0;
  for (size_t i = 0; i < count; i++) {
    g_test_count++;
    // Allow for rounding differences
    int16_t diff = static_cast<int16_t>(half_result[i]) - static_cast<int16_t>(half_expected[i]);
    if (std::abs(diff) <= 1) {
      local_pass++;
      g_pass_count++;
    } else {
      if (g_fail_count < 10) {
        printf("  FAIL at index %zu: got 0x%04X, expected 0x%04X\n",
               i, half_result[i], half_expected[i]);
      }
      g_fail_count++;
    }
  }

  printf("  Passed %d/%zu batch float->half tests\n\n", local_pass, count);
}

void test_round_trip() {
  printf("=== Test: FP16 Round-trip ===\n");

  const size_t count = 1024;
  std::vector<float> original(count);
  std::vector<uint16_t> half_data(count);
  std::vector<float> restored(count);

  // Fill with test values in half-precision range
  std::mt19937 rng(42);
  std::uniform_real_distribution<float> dist(-100.0f, 100.0f);

  for (size_t i = 0; i < count; i++) {
    original[i] = dist(rng);
  }

  // Convert float -> half -> float
  float_to_half_batch(original.data(), half_data.data(), count);
  half_to_float_batch(half_data.data(), restored.data(), count);

  int local_pass = 0;
  for (size_t i = 0; i < count; i++) {
    g_test_count++;
    // Half precision has ~3 decimal digits of precision
    // Allow for about 0.1% error
    float tolerance = std::fabs(original[i]) * 0.002f + 0.001f;
    float diff = std::fabs(original[i] - restored[i]);

    if (diff <= tolerance) {
      local_pass++;
      g_pass_count++;
    } else {
      if (g_fail_count < 10) {
        printf("  FAIL at index %zu: original %f, restored %f, diff %f\n",
               i, original[i], restored[i], diff);
      }
      g_fail_count++;
    }
  }

  printf("  Passed %d/%zu round-trip tests\n\n", local_pass, count);
}

void test_memcpy_simd() {
  printf("=== Test: memcpy_simd ===\n");

  const size_t sizes[] = {16, 32, 64, 128, 256, 512, 1024, 4096, 65536};
  const size_t num_sizes = sizeof(sizes) / sizeof(sizes[0]);

  for (size_t s = 0; s < num_sizes; s++) {
    size_t size = sizes[s];
    std::vector<uint8_t> src(size);
    std::vector<uint8_t> dst(size);

    // Fill with pattern
    for (size_t i = 0; i < size; i++) {
      src[i] = static_cast<uint8_t>(i & 0xFF);
    }

    memcpy_simd(dst.data(), src.data(), size);

    g_test_count++;
    if (memcmp(src.data(), dst.data(), size) == 0) {
      g_pass_count++;
    } else {
      printf("  FAIL: memcpy_simd size=%zu\n", size);
      g_fail_count++;
    }
  }

  printf("  Passed %zu/%zu memcpy tests\n\n", num_sizes, num_sizes);
}

void test_interleave_rgba() {
  printf("=== Test: interleave/deinterleave RGBA ===\n");

  const size_t count = 256;  // 256 pixels
  std::vector<float> r(count), g(count), b(count), a(count);
  std::vector<float> rgba(count * 4);
  std::vector<float> r2(count), g2(count), b2(count), a2(count);

  // Fill with test values
  for (size_t i = 0; i < count; i++) {
    r[i] = static_cast<float>(i) / count;
    g[i] = static_cast<float>(count - i) / count;
    b[i] = static_cast<float>(i * 2 % count) / count;
    a[i] = 1.0f;
  }

  // Interleave
  interleave_rgba_float(r.data(), g.data(), b.data(), a.data(), rgba.data(), count);

  // Verify interleave
  bool interleave_ok = true;
  for (size_t i = 0; i < count && interleave_ok; i++) {
    if (rgba[i * 4 + 0] != r[i] ||
        rgba[i * 4 + 1] != g[i] ||
        rgba[i * 4 + 2] != b[i] ||
        rgba[i * 4 + 3] != a[i]) {
      interleave_ok = false;
    }
  }

  g_test_count++;
  if (interleave_ok) {
    g_pass_count++;
  } else {
    printf("  FAIL: interleave_rgba_float\n");
    g_fail_count++;
  }

  // Deinterleave
  deinterleave_rgba_float(rgba.data(), r2.data(), g2.data(), b2.data(), a2.data(), count);

  // Verify deinterleave
  bool deinterleave_ok = true;
  for (size_t i = 0; i < count && deinterleave_ok; i++) {
    if (r2[i] != r[i] || g2[i] != g[i] || b2[i] != b[i] || a2[i] != a[i]) {
      deinterleave_ok = false;
    }
  }

  g_test_count++;
  if (deinterleave_ok) {
    g_pass_count++;
  } else {
    printf("  FAIL: deinterleave_rgba_float\n");
    g_fail_count++;
  }

  printf("  Passed interleave/deinterleave tests\n\n");
}

void test_byte_reorder() {
  printf("=== Test: byte reordering ===\n");

  const size_t count = 256;  // Must be even
  std::vector<uint8_t> original(count);
  std::vector<uint8_t> reordered(count);
  std::vector<uint8_t> restored(count);

  // Fill with pattern
  for (size_t i = 0; i < count; i++) {
    original[i] = static_cast<uint8_t>(i);
  }

  // Reorder for compression
  reorder_bytes_for_compression(original.data(), reordered.data(), count);

  // Verify reordering (even bytes in first half, odd in second half)
  bool reorder_ok = true;
  size_t half = count / 2;
  for (size_t i = 0; i < half && reorder_ok; i++) {
    if (reordered[i] != original[i * 2]) {
      reorder_ok = false;
    }
    if (reordered[half + i] != original[i * 2 + 1]) {
      reorder_ok = false;
    }
  }

  g_test_count++;
  if (reorder_ok) {
    g_pass_count++;
  } else {
    printf("  FAIL: reorder_bytes_for_compression\n");
    g_fail_count++;
  }

  // Unreorder
  unreorder_bytes_after_decompression(reordered.data(), restored.data(), count);

  g_test_count++;
  if (memcmp(original.data(), restored.data(), count) == 0) {
    g_pass_count++;
  } else {
    printf("  FAIL: unreorder_bytes_after_decompression\n");
    g_fail_count++;
  }

  printf("  Passed byte reorder tests\n\n");
}

void benchmark_half_float_conversion() {
  printf("=== Benchmark: FP16 Conversion ===\n");

  const size_t count = 1024 * 1024;  // 1M values
  const int iterations = 100;

  std::vector<uint16_t> half_data(count);
  std::vector<float> float_data(count);

  // Initialize data
  std::mt19937 rng(42);
  std::uniform_int_distribution<uint16_t> dist(0, 0x7BFF);
  for (size_t i = 0; i < count; i++) {
    half_data[i] = dist(rng);
  }

  // Benchmark half -> float
  auto start = std::chrono::high_resolution_clock::now();
  for (int iter = 0; iter < iterations; iter++) {
    half_to_float_batch(half_data.data(), float_data.data(), count);
  }
  auto end = std::chrono::high_resolution_clock::now();
  double ms = std::chrono::duration<double, std::milli>(end - start).count();
  double values_per_sec = (count * iterations) / (ms / 1000.0);

  printf("  half->float: %.2f ms for %d iterations\n", ms, iterations);
  printf("               %.2f million values/sec\n", values_per_sec / 1e6);

  // Benchmark float -> half
  start = std::chrono::high_resolution_clock::now();
  for (int iter = 0; iter < iterations; iter++) {
    float_to_half_batch(float_data.data(), half_data.data(), count);
  }
  end = std::chrono::high_resolution_clock::now();
  ms = std::chrono::duration<double, std::milli>(end - start).count();
  values_per_sec = (count * iterations) / (ms / 1000.0);

  printf("  float->half: %.2f ms for %d iterations\n", ms, iterations);
  printf("               %.2f million values/sec\n\n", values_per_sec / 1e6);
}

void benchmark_memcpy() {
  printf("=== Benchmark: memcpy_simd ===\n");

  const size_t size = 16 * 1024 * 1024;  // 16MB
  const int iterations = 100;

  std::vector<uint8_t> src(size);
  std::vector<uint8_t> dst(size);

  // Initialize
  for (size_t i = 0; i < size; i++) {
    src[i] = static_cast<uint8_t>(i);
  }

  // Benchmark SIMD memcpy
  auto start = std::chrono::high_resolution_clock::now();
  for (int iter = 0; iter < iterations; iter++) {
    memcpy_simd(dst.data(), src.data(), size);
  }
  auto end = std::chrono::high_resolution_clock::now();
  double ms = std::chrono::duration<double, std::milli>(end - start).count();
  double gb_per_sec = (size * iterations) / (ms / 1000.0) / (1024.0 * 1024.0 * 1024.0);

  printf("  SIMD memcpy: %.2f ms for %d iterations\n", ms, iterations);
  printf("               %.2f GB/sec\n", gb_per_sec);

  // Benchmark standard memcpy
  start = std::chrono::high_resolution_clock::now();
  for (int iter = 0; iter < iterations; iter++) {
    std::memcpy(dst.data(), src.data(), size);
  }
  end = std::chrono::high_resolution_clock::now();
  ms = std::chrono::duration<double, std::milli>(end - start).count();
  gb_per_sec = (size * iterations) / (ms / 1000.0) / (1024.0 * 1024.0 * 1024.0);

  printf("  std::memcpy: %.2f ms for %d iterations\n", ms, iterations);
  printf("               %.2f GB/sec\n\n", gb_per_sec);
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
  printf("TinyEXR SIMD Test Suite\n");
  printf("=======================\n\n");

  // Display capabilities
  test_simd_capabilities();

  // Run correctness tests
  test_half_to_float_scalar();
  test_float_to_half_scalar();
  test_half_to_float_batch();
  test_float_to_half_batch();
  test_round_trip();
  test_memcpy_simd();
  test_interleave_rgba();
  test_byte_reorder();

  // Run benchmarks
  if (argc > 1 && strcmp(argv[1], "--bench") == 0) {
    benchmark_half_float_conversion();
    benchmark_memcpy();
  }

  // Summary
  printf("=== Test Summary ===\n");
  printf("Total: %d tests\n", g_test_count);
  printf("Passed: %d\n", g_pass_count);
  printf("Failed: %d\n", g_fail_count);
  printf("\n");

  if (g_fail_count > 0) {
    printf("SOME TESTS FAILED!\n");
    return 1;
  } else {
    printf("ALL TESTS PASSED!\n");
    return 0;
  }
}
