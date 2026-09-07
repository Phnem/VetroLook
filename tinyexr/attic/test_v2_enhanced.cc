// Test suite for TinyEXR V2 Enhanced Features
//
// Tests: Memory-mapped I/O, ThreadPool, SIMD batch conversion,
//        PIZ compression, Tiled EXR, Parallel processing

#define TINYEXR_USE_MINIZ 1
#define TINYEXR_ENABLE_SIMD 1
#include "tinyexr_v2.hh"
#include "tinyexr_v2_impl.hh"
#include "tinyexr_v2_enhanced.hh"

#include <cstdio>
#include <cmath>
#include <chrono>
#include <algorithm>

using namespace tinyexr::v2;
namespace enh = tinyexr::v2::enhanced;

static int g_total_tests = 0;
static int g_passed_tests = 0;

#define TEST(name) \
  void test_##name(); \
  struct Test_##name { \
    Test_##name() { \
      g_total_tests++; \
      printf("Running %s... ", #name); \
      fflush(stdout); \
      try { \
        test_##name(); \
        g_passed_tests++; \
        printf("PASSED\n"); \
      } catch (const std::exception& e) { \
        printf("FAILED: %s\n", e.what()); \
      } catch (...) { \
        printf("FAILED: unknown exception\n"); \
      } \
    } \
  } g_test_##name; \
  void test_##name()

#define ASSERT(cond) \
  do { \
    if (!(cond)) { \
      throw std::runtime_error("Assertion failed: " #cond); \
    } \
  } while(0)

#define ASSERT_NEAR(a, b, eps) \
  do { \
    if (std::abs((a) - (b)) > (eps)) { \
      char buf[256]; \
      snprintf(buf, sizeof(buf), "Expected %g ~= %g (eps=%g)", \
               static_cast<double>(a), static_cast<double>(b), static_cast<double>(eps)); \
      throw std::runtime_error(buf); \
    } \
  } while(0)

// ============================================================================
// Memory-Mapped File Tests
// ============================================================================

TEST(MemoryMappedFile_OpenClose) {
  // First create a test file
  const char* test_file = "/tmp/test_mmap.exr";

  ImageData img;
  img.width = 64;
  img.height = 64;
  img.num_channels = 4;
  img.rgba.resize(64 * 64 * 4);

  for (int i = 0; i < 64 * 64 * 4; i++) {
    img.rgba[i] = static_cast<float>(i % 256) / 255.0f;
  }

  img.header.data_window.min_x = 0;
  img.header.data_window.min_y = 0;
  img.header.data_window.max_x = 63;
  img.header.data_window.max_y = 63;
  img.header.display_window = img.header.data_window;
  img.header.compression = 0;  // NONE

  Channel ch;
  ch.pixel_type = 1;
  ch.x_sampling = 1;
  ch.y_sampling = 1;
  ch.name = "R"; img.header.channels.push_back(ch);
  ch.name = "G"; img.header.channels.push_back(ch);
  ch.name = "B"; img.header.channels.push_back(ch);
  ch.name = "A"; img.header.channels.push_back(ch);

  auto save_result = SaveToFile(test_file, img);
  ASSERT(save_result.success);

  // Now test mmap
  enh::MemoryMappedFile mmap;
  ASSERT(mmap.open(test_file));
  ASSERT(mmap.is_mapped());
  ASSERT(mmap.data() != nullptr);
  ASSERT(mmap.size() > 0);

  // Check magic number
  ASSERT(mmap.data()[0] == 0x76);  // 'v'
  ASSERT(mmap.data()[1] == 0x2f);  // '/'
  ASSERT(mmap.data()[2] == 0x31);  // '1'
  ASSERT(mmap.data()[3] == 0x01);  // version 1

  mmap.close();
  ASSERT(!mmap.is_mapped());
}

TEST(MemoryMappedFile_LoadFromMapped) {
  const char* test_file = "/tmp/test_mmap.exr";

  auto result = enh::LoadFromMappedFile(test_file);
  ASSERT(result.success);
  ASSERT(result.value.width == 64);
  ASSERT(result.value.height == 64);
  ASSERT(result.value.rgba.size() == 64 * 64 * 4);
}

TEST(MemoryMappedFile_NonExistent) {
  enh::MemoryMappedFile mmap;
  ASSERT(!mmap.open("/tmp/nonexistent_file_12345.exr"));
  ASSERT(!mmap.is_mapped());
}

TEST(MemoryMappedFile_Move) {
  const char* test_file = "/tmp/test_mmap.exr";

  enh::MemoryMappedFile mmap1;
  ASSERT(mmap1.open(test_file));

  const uint8_t* data_ptr = mmap1.data();
  size_t file_size = mmap1.size();

  // Move construct
  enh::MemoryMappedFile mmap2(std::move(mmap1));
  ASSERT(!mmap1.is_mapped());
  ASSERT(mmap2.is_mapped());
  ASSERT(mmap2.data() == data_ptr);
  ASSERT(mmap2.size() == file_size);

  // Move assign
  enh::MemoryMappedFile mmap3;
  mmap3 = std::move(mmap2);
  ASSERT(!mmap2.is_mapped());
  ASSERT(mmap3.is_mapped());
  ASSERT(mmap3.data() == data_ptr);
}

// ============================================================================
// Thread Pool Tests
// ============================================================================

TEST(ThreadPool_Basic) {
  enh::ThreadPool& pool = enh::get_thread_pool();
  ASSERT(pool.num_threads() > 0);
}

TEST(ThreadPool_Submit) {
  enh::ThreadPool& pool = enh::get_thread_pool();

  std::atomic<int> counter{0};

  std::vector<std::future<int>> futures;
  for (int i = 0; i < 100; i++) {
    futures.push_back(pool.submit([&counter, i]() {
      counter++;
      return i * 2;
    }));
  }

  // Wait and verify results
  for (int i = 0; i < 100; i++) {
    ASSERT(futures[i].get() == i * 2);
  }

  ASSERT(counter == 100);
}

TEST(ThreadPool_Parallel) {
  enh::ThreadPool& pool = enh::get_thread_pool();

  const int N = 1000;
  std::vector<int> data(N);
  std::atomic<int> sum{0};

  // Initialize in parallel
  std::vector<std::future<void>> init_futures;
  for (int i = 0; i < N; i++) {
    init_futures.push_back(pool.submit([&data, i]() {
      data[i] = i;
    }));
  }
  for (auto& f : init_futures) f.get();

  // Sum in parallel
  std::vector<std::future<void>> sum_futures;
  for (int i = 0; i < N; i++) {
    sum_futures.push_back(pool.submit([&data, &sum, i]() {
      sum += data[i];
    }));
  }
  for (auto& f : sum_futures) f.get();

  ASSERT(sum == (N * (N - 1)) / 2);
}

// ============================================================================
// SIMD Batch Conversion Tests
// ============================================================================

TEST(ConvertHalfToFloatBatch_Basic) {
  const size_t count = 100;
  std::vector<uint16_t> src(count);
  std::vector<float> dst(count);

  // Create test data (half precision 1.0, 2.0, 0.5, etc.)
  for (size_t i = 0; i < count; i++) {
    float val = static_cast<float>(i) / 10.0f;
    // Manual float to half
    union { float f; uint32_t u; } fi;
    fi.f = val;
    uint32_t sign = (fi.u >> 16) & 0x8000;
    int32_t exp = ((fi.u >> 23) & 0xFF) - 127 + 15;
    uint32_t mant = fi.u & 0x7FFFFF;
    if (exp <= 0) src[i] = static_cast<uint16_t>(sign);
    else if (exp >= 31) src[i] = static_cast<uint16_t>(sign | 0x7C00);
    else src[i] = static_cast<uint16_t>(sign | (exp << 10) | (mant >> 13));
  }

  enh::ConvertHalfToFloatBatch(src.data(), dst.data(), count);

  // Verify
  for (size_t i = 0; i < count; i++) {
    float expected = static_cast<float>(i) / 10.0f;
    ASSERT_NEAR(dst[i], expected, 0.01f);
  }
}

TEST(ConvertFloatToHalfBatch_Basic) {
  const size_t count = 100;
  std::vector<float> src(count);
  std::vector<uint16_t> dst(count);

  for (size_t i = 0; i < count; i++) {
    src[i] = static_cast<float>(i) / 10.0f;
  }

  enh::ConvertFloatToHalfBatch(src.data(), dst.data(), count);

  // Convert back and verify
  std::vector<float> verify(count);
  enh::ConvertHalfToFloatBatch(dst.data(), verify.data(), count);

  for (size_t i = 0; i < count; i++) {
    ASSERT_NEAR(verify[i], src[i], 0.01f);
  }
}

TEST(DeinterleaveRGBA_Basic) {
  const size_t count = 16;
  std::vector<float> interleaved(count * 4);
  std::vector<float> r(count), g(count), b(count), a(count);

  // Create interleaved RGBA data
  for (size_t i = 0; i < count; i++) {
    interleaved[i * 4 + 0] = static_cast<float>(i);          // R
    interleaved[i * 4 + 1] = static_cast<float>(i + 100);    // G
    interleaved[i * 4 + 2] = static_cast<float>(i + 200);    // B
    interleaved[i * 4 + 3] = static_cast<float>(i + 300);    // A
  }

  enh::DeinterleaveRGBA(interleaved.data(), r.data(), g.data(), b.data(), a.data(), count);

  // Verify
  for (size_t i = 0; i < count; i++) {
    ASSERT_NEAR(r[i], static_cast<float>(i), 0.001f);
    ASSERT_NEAR(g[i], static_cast<float>(i + 100), 0.001f);
    ASSERT_NEAR(b[i], static_cast<float>(i + 200), 0.001f);
    ASSERT_NEAR(a[i], static_cast<float>(i + 300), 0.001f);
  }
}

TEST(InterleaveRGBA_Basic) {
  const size_t count = 16;
  std::vector<float> r(count), g(count), b(count), a(count);
  std::vector<float> interleaved(count * 4);

  for (size_t i = 0; i < count; i++) {
    r[i] = static_cast<float>(i);
    g[i] = static_cast<float>(i + 100);
    b[i] = static_cast<float>(i + 200);
    a[i] = static_cast<float>(i + 300);
  }

  enh::InterleaveRGBA(r.data(), g.data(), b.data(), a.data(), interleaved.data(), count);

  for (size_t i = 0; i < count; i++) {
    ASSERT_NEAR(interleaved[i * 4 + 0], static_cast<float>(i), 0.001f);
    ASSERT_NEAR(interleaved[i * 4 + 1], static_cast<float>(i + 100), 0.001f);
    ASSERT_NEAR(interleaved[i * 4 + 2], static_cast<float>(i + 200), 0.001f);
    ASSERT_NEAR(interleaved[i * 4 + 3], static_cast<float>(i + 300), 0.001f);
  }
}

TEST(InterleaveDeinterleave_RoundTrip) {
  const size_t count = 64;
  std::vector<float> original(count * 4);
  std::vector<float> r(count), g(count), b(count), a(count);
  std::vector<float> result(count * 4);

  for (size_t i = 0; i < count * 4; i++) {
    original[i] = static_cast<float>(i) * 0.01f;
  }

  enh::DeinterleaveRGBA(original.data(), r.data(), g.data(), b.data(), a.data(), count);
  enh::InterleaveRGBA(r.data(), g.data(), b.data(), a.data(), result.data(), count);

  for (size_t i = 0; i < count * 4; i++) {
    ASSERT_NEAR(result[i], original[i], 0.001f);
  }
}

// ============================================================================
// PIZ Compression Tests
// ============================================================================

TEST(PIZ_Compress_Basic) {
  const int width = 16;
  const int height = 16;
  const int num_channels = 1;
  const int num_pixels = width * height * num_channels;

  std::vector<uint16_t> data(num_pixels);
  for (int i = 0; i < num_pixels; i++) {
    data[i] = static_cast<uint16_t>(i % 1024);
  }

  // Need extra space for bitmap (8KB) + header + encoded data
  std::vector<uint8_t> compressed(num_pixels * 4 + 16384);

  auto result = enh::piz::CompressPiz(data.data(), num_pixels, width, height,
                                       num_channels, compressed.data(), compressed.size());

  if (!result.success) {
    printf("[error: %s] ", result.error.c_str());
  }
  ASSERT(result.success);
  ASSERT(result.compressed_size > 0);
  ASSERT(result.compressed_size <= compressed.size());

  printf("[compressed %d -> %zu bytes] ", num_pixels * 2, result.compressed_size);
}

TEST(PIZ_Compress_MultiChannel) {
  const int width = 32;
  const int height = 32;
  const int num_channels = 4;
  const int num_pixels = width * height * num_channels;

  std::vector<uint16_t> data(num_pixels);

  // Create gradient data per channel
  for (int ch = 0; ch < num_channels; ch++) {
    for (int y = 0; y < height; y++) {
      for (int x = 0; x < width; x++) {
        int idx = ch * width * height + y * width + x;
        data[idx] = static_cast<uint16_t>((x + y * ch) % 65536);
      }
    }
  }

  std::vector<uint8_t> compressed(num_pixels * 4);

  auto result = enh::piz::CompressPiz(data.data(), num_pixels, width, height,
                                       num_channels, compressed.data(), compressed.size());

  ASSERT(result.success);
  ASSERT(result.compressed_size > 0);

  // Compression should provide some benefit on gradient data
  float ratio = static_cast<float>(num_pixels * 2) / result.compressed_size;
  printf("[ratio: %.2fx] ", ratio);
}

TEST(PIZ_Compress_AllZeros) {
  const int width = 64;
  const int height = 64;
  const int num_channels = 1;
  const int num_pixels = width * height;

  std::vector<uint16_t> data(num_pixels, 0);
  std::vector<uint8_t> compressed(num_pixels * 4);

  auto result = enh::piz::CompressPiz(data.data(), num_pixels, width, height,
                                       num_channels, compressed.data(), compressed.size());

  ASSERT(result.success);

  // All zeros should compress very well
  float ratio = static_cast<float>(num_pixels * 2) / result.compressed_size;
  printf("[ratio: %.2fx] ", ratio);
  ASSERT(ratio > 10.0f);  // At least 10x compression for all zeros
}

TEST(PIZ_Compress_Random) {
  const int width = 32;
  const int height = 32;
  const int num_channels = 1;
  const int num_pixels = width * height;

  std::vector<uint16_t> data(num_pixels);

  // Generate pseudo-random data
  uint32_t seed = 12345;
  for (int i = 0; i < num_pixels; i++) {
    seed = seed * 1103515245 + 12345;
    data[i] = static_cast<uint16_t>(seed >> 16);
  }

  // Need extra space for bitmap (8KB) + header + encoded data
  std::vector<uint8_t> compressed(num_pixels * 4 + 16384);

  auto result = enh::piz::CompressPiz(data.data(), num_pixels, width, height,
                                       num_channels, compressed.data(), compressed.size());

  if (!result.success) {
    printf("[error: %s] ", result.error.c_str());
  }
  ASSERT(result.success);
  // Random data may not compress well, just check it doesn't crash
}

// ============================================================================
// Tiled EXR Tests
// ============================================================================

TEST(TiledEXR_SaveBasic) {
  enh::TiledImageData img;
  img.width = 128;
  img.height = 128;
  img.tile_desc.x_size = 32;
  img.tile_desc.y_size = 32;
  img.tile_desc.mode = 0;  // Single level

  img.header.data_window.min_x = 0;
  img.header.data_window.min_y = 0;
  img.header.data_window.max_x = 127;
  img.header.data_window.max_y = 127;
  img.header.display_window = img.header.data_window;
  img.header.compression = 0;  // NONE

  Channel ch;
  ch.pixel_type = 1;  // HALF
  ch.x_sampling = 1;
  ch.y_sampling = 1;
  ch.name = "R"; img.header.channels.push_back(ch);
  ch.name = "G"; img.header.channels.push_back(ch);
  ch.name = "B"; img.header.channels.push_back(ch);
  ch.name = "A"; img.header.channels.push_back(ch);

  // Create gradient image
  img.rgba.resize(128 * 128 * 4);
  for (int y = 0; y < 128; y++) {
    for (int x = 0; x < 128; x++) {
      int idx = (y * 128 + x) * 4;
      img.rgba[idx + 0] = static_cast<float>(x) / 127.0f;  // R
      img.rgba[idx + 1] = static_cast<float>(y) / 127.0f;  // G
      img.rgba[idx + 2] = 0.5f;                            // B
      img.rgba[idx + 3] = 1.0f;                            // A
    }
  }

  auto result = enh::SaveTiledToMemory(img);
  ASSERT(result.success);
  ASSERT(result.value.size() > 0);

  printf("[size: %zu bytes] ", result.value.size());
}

TEST(TiledEXR_TileBounds) {
  enh::TiledImageData img;
  img.width = 100;
  img.height = 75;
  img.tile_desc.x_size = 32;
  img.tile_desc.y_size = 32;

  int x0, y0, x1, y1;

  // First tile
  img.get_tile_bounds(0, 0, x0, y0, x1, y1);
  ASSERT(x0 == 0);
  ASSERT(y0 == 0);
  ASSERT(x1 == 32);
  ASSERT(y1 == 32);

  // Last tile (should be clipped)
  img.get_tile_bounds(3, 2, x0, y0, x1, y1);
  ASSERT(x0 == 96);
  ASSERT(y0 == 64);
  ASSERT(x1 == 100);  // Clipped to image width
  ASSERT(y1 == 75);   // Clipped to image height
}

TEST(TiledEXR_SaveToFile) {
  enh::TiledImageData img;
  img.width = 64;
  img.height = 64;
  img.tile_desc.x_size = 32;
  img.tile_desc.y_size = 32;

  img.header.data_window.min_x = 0;
  img.header.data_window.min_y = 0;
  img.header.data_window.max_x = 63;
  img.header.data_window.max_y = 63;
  img.header.display_window = img.header.data_window;
  img.header.compression = 0;

  Channel ch;
  ch.pixel_type = 1;
  ch.x_sampling = 1;
  ch.y_sampling = 1;
  ch.name = "R"; img.header.channels.push_back(ch);
  ch.name = "G"; img.header.channels.push_back(ch);
  ch.name = "B"; img.header.channels.push_back(ch);
  ch.name = "A"; img.header.channels.push_back(ch);

  img.rgba.resize(64 * 64 * 4);
  for (int i = 0; i < 64 * 64; i++) {
    img.rgba[i * 4 + 0] = 1.0f;
    img.rgba[i * 4 + 1] = 0.5f;
    img.rgba[i * 4 + 2] = 0.25f;
    img.rgba[i * 4 + 3] = 1.0f;
  }

  auto result = enh::SaveTiledToMemory(img);
  ASSERT(result.success);

  // Save to file
  FILE* f = fopen("/tmp/test_tiled.exr", "wb");
  ASSERT(f != nullptr);
  fwrite(result.value.data(), 1, result.value.size(), f);
  fclose(f);

  printf("[saved to /tmp/test_tiled.exr] ");
}

// ============================================================================
// Parallel Processing Tests
// ============================================================================

TEST(ParallelScanlines_Basic) {
  const int height = 100;
  std::vector<int> processed(height, 0);
  std::mutex mtx;

  enh::ProcessScanlinesParallel(height, 16, [&](int block, int y_start, int y_end) {
    std::lock_guard<std::mutex> lock(mtx);
    for (int y = y_start; y < y_end; y++) {
      processed[y] = 1;
    }
  });

  // Verify all scanlines were processed
  for (int y = 0; y < height; y++) {
    ASSERT(processed[y] == 1);
  }
}

TEST(ParallelTiles_Basic) {
  const int num_x = 5;
  const int num_y = 4;
  std::vector<std::vector<int>> processed(num_y, std::vector<int>(num_x, 0));
  std::mutex mtx;

  enh::ProcessTilesParallel(num_x, num_y, [&](int tx, int ty) {
    std::lock_guard<std::mutex> lock(mtx);
    processed[ty][tx] = 1;
  });

  // Verify all tiles were processed
  for (int ty = 0; ty < num_y; ty++) {
    for (int tx = 0; tx < num_x; tx++) {
      ASSERT(processed[ty][tx] == 1);
    }
  }
}

// ============================================================================
// Performance Tests
// ============================================================================

TEST(Performance_BatchConversion) {
  const size_t count = 1000000;
  std::vector<uint16_t> half_data(count);
  std::vector<float> float_data(count);

  // Initialize with test data
  for (size_t i = 0; i < count; i++) {
    half_data[i] = static_cast<uint16_t>(i % 65536);
  }

  auto start = std::chrono::high_resolution_clock::now();

  enh::ConvertHalfToFloatBatch(half_data.data(), float_data.data(), count);

  auto end = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

  printf("[%zu conversions in %lld us] ", count, static_cast<long long>(duration.count()));
}

TEST(Performance_Interleave) {
  const size_t count = 250000;  // 250k pixels, 1M floats total
  std::vector<float> interleaved(count * 4);
  std::vector<float> r(count), g(count), b(count), a(count);

  for (size_t i = 0; i < count * 4; i++) {
    interleaved[i] = static_cast<float>(i);
  }

  auto start = std::chrono::high_resolution_clock::now();

  enh::DeinterleaveRGBA(interleaved.data(), r.data(), g.data(), b.data(), a.data(), count);
  enh::InterleaveRGBA(r.data(), g.data(), b.data(), a.data(), interleaved.data(), count);

  auto end = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);

  printf("[%zu pixels in %lld us] ", count, static_cast<long long>(duration.count()));
}

// ============================================================================
// API Info Test
// ============================================================================

TEST(GetEnhancedFeaturesInfo) {
  const char* info = enh::GetEnhancedFeaturesInfo();
  ASSERT(info != nullptr);
  ASSERT(strlen(info) > 0);

  // Should contain key features
  ASSERT(strstr(info, "Memory-mapped") != nullptr);
  ASSERT(strstr(info, "Thread pool") != nullptr);
  ASSERT(strstr(info, "SIMD") != nullptr);
  ASSERT(strstr(info, "PIZ") != nullptr);
  ASSERT(strstr(info, "Tiled") != nullptr);
}

// ============================================================================
// Main
// ============================================================================

int main() {
  printf("\n=== TinyEXR V2 Enhanced Features Test Suite ===\n\n");
  printf("Thread pool size: %zu\n\n", enh::get_thread_pool().num_threads());

  // Tests are auto-registered and run via global constructors

  printf("\n=== Test Summary ===\n");
  printf("Passed: %d / %d\n\n", g_passed_tests, g_total_tests);

  return (g_passed_tests == g_total_tests) ? 0 : 1;
}
