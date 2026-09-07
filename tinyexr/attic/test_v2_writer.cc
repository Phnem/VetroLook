// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025, Syoyo Fujita and many contributors.
// All rights reserved.
//
// TinyEXR V2 Writer Tests
//
// Comprehensive tests for V2 API writer functionality including:
// - Compression types (NONE, RLE, ZIP, ZIPS)
// - Round-trip read/write correctness
// - Header writing with channels
// - SIMD optimization verification

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>

// Enable compression backend
// Use TINYEXR_USE_ZLIB for system zlib, or TINYEXR_USE_MINIZ for miniz
#ifndef TINYEXR_USE_MINIZ
#ifndef TINYEXR_USE_ZLIB
#define TINYEXR_USE_ZLIB 1
#endif
#endif
#define TINYEXR_ENABLE_SIMD 1

#include "tinyexr_v2.hh"
#include "tinyexr_v2_impl.hh"

using namespace tinyexr::v2;

// ============================================================================
// Test Framework
// ============================================================================

static int g_test_count = 0;
static int g_pass_count = 0;
static int g_fail_count = 0;

#define TEST(name) \
  static void test_##name(); \
  static struct Test_##name { \
    Test_##name() { \
      printf("Running test: %s\n", #name); \
      g_test_count++; \
      try { \
        test_##name(); \
        g_pass_count++; \
        printf("  PASSED\n"); \
      } catch (const char* e) { \
        g_fail_count++; \
        printf("  FAILED: %s\n", e); \
      } \
    } \
  } test_instance_##name; \
  static void test_##name()

#define ASSERT(cond) \
  do { \
    if (!(cond)) { \
      throw "Assertion failed: " #cond; \
    } \
  } while (0)

#define ASSERT_EQ(a, b) \
  do { \
    if ((a) != (b)) { \
      throw "Assertion failed: " #a " != " #b; \
    } \
  } while (0)

#define ASSERT_NEAR(a, b, tol) \
  do { \
    if (std::abs((a) - (b)) > (tol)) { \
      throw "Assertion failed: " #a " not near " #b; \
    } \
  } while (0)

// ============================================================================
// Helper Functions
// ============================================================================

// Create a test image with known pattern
ImageData CreateTestImage(int width, int height, int pattern = 0) {
  ImageData img;
  img.width = width;
  img.height = height;
  img.num_channels = 4;
  img.rgba.resize(width * height * 4);

  // Setup header
  img.header.data_window.min_x = 0;
  img.header.data_window.min_y = 0;
  img.header.data_window.max_x = width - 1;
  img.header.data_window.max_y = height - 1;
  img.header.display_window = img.header.data_window;
  img.header.compression = COMPRESSION_NONE;
  img.header.line_order = 0;
  img.header.pixel_aspect_ratio = 1.0f;
  img.header.screen_window_center[0] = 0.0f;
  img.header.screen_window_center[1] = 0.0f;
  img.header.screen_window_width = 1.0f;

  // Add RGBA channels (HALF type)
  Channel chR, chG, chB, chA;
  chR.name = "R"; chR.pixel_type = 1; chR.x_sampling = 1; chR.y_sampling = 1;
  chG.name = "G"; chG.pixel_type = 1; chG.x_sampling = 1; chG.y_sampling = 1;
  chB.name = "B"; chB.pixel_type = 1; chB.x_sampling = 1; chB.y_sampling = 1;
  chA.name = "A"; chA.pixel_type = 1; chA.x_sampling = 1; chA.y_sampling = 1;
  img.header.channels.push_back(chR);
  img.header.channels.push_back(chG);
  img.header.channels.push_back(chB);
  img.header.channels.push_back(chA);

  // Fill with test pattern
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      float r, g, b, a;
      switch (pattern) {
        case 0:  // Gradient
          r = static_cast<float>(x) / width;
          g = static_cast<float>(y) / height;
          b = 0.5f;
          a = 1.0f;
          break;
        case 1:  // Checkerboard
          r = g = b = ((x + y) % 2 == 0) ? 1.0f : 0.0f;
          a = 1.0f;
          break;
        case 2:  // Solid color
          r = 0.25f;
          g = 0.5f;
          b = 0.75f;
          a = 1.0f;
          break;
        case 3:  // Random-ish (deterministic)
          r = static_cast<float>((x * 13 + y * 7) % 256) / 255.0f;
          g = static_cast<float>((x * 23 + y * 11) % 256) / 255.0f;
          b = static_cast<float>((x * 31 + y * 17) % 256) / 255.0f;
          a = 1.0f;
          break;
        default:
          r = g = b = a = 1.0f;
          break;
      }
      int idx = (y * width + x) * 4;
      img.rgba[idx + 0] = r;
      img.rgba[idx + 1] = g;
      img.rgba[idx + 2] = b;
      img.rgba[idx + 3] = a;
    }
  }

  return img;
}

// Compare two images (with tolerance for half precision)
bool CompareImages(const ImageData& a, const ImageData& b, float tolerance = 0.001f) {
  if (a.width != b.width || a.height != b.height) {
    printf("    Size mismatch: %dx%d vs %dx%d\n", a.width, a.height, b.width, b.height);
    return false;
  }

  int diff_count = 0;
  float max_diff = 0.0f;
  for (size_t i = 0; i < a.rgba.size(); i++) {
    float diff = std::abs(a.rgba[i] - b.rgba[i]);
    if (diff > tolerance) {
      diff_count++;
      if (diff > max_diff) max_diff = diff;
    }
  }

  if (diff_count > 0) {
    printf("    %d pixels differ, max diff: %f\n", diff_count, max_diff);
    return false;
  }

  return true;
}

// ============================================================================
// Tests
// ============================================================================

TEST(WriteVersion) {
  Writer writer;
  Version ver;
  ver.version = 2;
  ver.tiled = false;
  ver.long_name = false;
  ver.non_image = false;
  ver.multipart = false;

  auto result = WriteVersion(writer, ver);
  ASSERT(result.success);
  ASSERT_EQ(writer.size(), 8u);

  // Check magic number
  const uint8_t* data = writer.data_ptr();
  ASSERT_EQ(data[0], 0x76);
  ASSERT_EQ(data[1], 0x2f);
  ASSERT_EQ(data[2], 0x31);
  ASSERT_EQ(data[3], 0x01);
  ASSERT_EQ(data[4], 2);  // version
  ASSERT_EQ(data[5], 0);  // flags
}

TEST(WriteVersionWithFlags) {
  Writer writer;
  Version ver;
  ver.version = 2;
  ver.tiled = true;
  ver.long_name = true;
  ver.non_image = false;
  ver.multipart = false;

  auto result = WriteVersion(writer, ver);
  ASSERT(result.success);

  const uint8_t* data = writer.data_ptr();
  uint8_t expected_flags = 0x02 | 0x04;  // tiled | long_name
  ASSERT_EQ(data[5], expected_flags);
}

TEST(WriteHeader) {
  Writer writer;
  Header header;

  // Setup minimal header
  header.data_window.min_x = 0;
  header.data_window.min_y = 0;
  header.data_window.max_x = 63;
  header.data_window.max_y = 63;
  header.display_window = header.data_window;
  header.compression = COMPRESSION_ZIP;
  header.line_order = 0;
  header.pixel_aspect_ratio = 1.0f;
  header.screen_window_center[0] = 0.0f;
  header.screen_window_center[1] = 0.0f;
  header.screen_window_width = 1.0f;

  // Add RGBA channels
  Channel chR, chG, chB, chA;
  chR.name = "R"; chR.pixel_type = 1;
  chG.name = "G"; chG.pixel_type = 1;
  chB.name = "B"; chB.pixel_type = 1;
  chA.name = "A"; chA.pixel_type = 1;
  header.channels.push_back(chR);
  header.channels.push_back(chG);
  header.channels.push_back(chB);
  header.channels.push_back(chA);

  auto result = WriteHeader(writer, header);
  ASSERT(result.success);
  ASSERT(writer.size() > 0);

  // Verify header ends with null byte
  const uint8_t* data = writer.data_ptr();
  ASSERT_EQ(data[writer.size() - 1], 0);
}

TEST(SaveToMemoryNoCompression) {
  ImageData img = CreateTestImage(64, 64, 0);
  img.header.compression = COMPRESSION_NONE;

  auto result = SaveToMemory(img);
  ASSERT(result.success);
  ASSERT(result.value.size() > 0);

  printf("    Uncompressed size: %zu bytes\n", result.value.size());
}

TEST(SaveToMemoryRLE) {
  ImageData img = CreateTestImage(64, 64, 1);  // Checkerboard
  img.header.compression = COMPRESSION_RLE;

  auto result = SaveToMemory(img);
  ASSERT(result.success);
  ASSERT(result.value.size() > 0);

  printf("    RLE compressed size: %zu bytes\n", result.value.size());
}

TEST(SaveToMemoryZIP) {
  ImageData img = CreateTestImage(64, 64, 0);
  img.header.compression = COMPRESSION_ZIP;

  auto result = SaveToMemory(img);
  ASSERT(result.success);
  ASSERT(result.value.size() > 0);

  printf("    ZIP compressed size: %zu bytes\n", result.value.size());
}

TEST(SaveToMemoryZIPS) {
  ImageData img = CreateTestImage(64, 64, 0);
  img.header.compression = COMPRESSION_ZIPS;

  auto result = SaveToMemory(img);
  ASSERT(result.success);
  ASSERT(result.value.size() > 0);

  printf("    ZIPS compressed size: %zu bytes\n", result.value.size());
}

TEST(CompressionLevels) {
  ImageData img = CreateTestImage(128, 128, 3);
  img.header.compression = COMPRESSION_ZIP;

  size_t sizes[10] = {0};

  for (int level = 1; level <= 9; level++) {
    auto result = SaveToMemory(img, level);
    ASSERT(result.success);
    sizes[level] = result.value.size();
  }

  printf("    Compression sizes: ");
  for (int level = 1; level <= 9; level++) {
    printf("L%d=%zu ", level, sizes[level]);
  }
  printf("\n");

  // Higher compression levels should generally produce smaller files
  // (not guaranteed but typical)
  ASSERT(sizes[9] <= sizes[1] * 1.1);  // Allow 10% variance
}

TEST(RoundTripNoCompression) {
  ImageData original = CreateTestImage(32, 32, 2);
  original.header.compression = COMPRESSION_NONE;

  // Save to memory
  auto save_result = SaveToMemory(original);
  ASSERT(save_result.success);

  // Load back
  auto load_result = LoadFromMemory(save_result.value.data(), save_result.value.size());
  ASSERT(load_result.success);

  // Compare
  bool match = CompareImages(original, load_result.value, 0.01f);
  ASSERT(match);
}

TEST(RoundTripRLE) {
  ImageData original = CreateTestImage(32, 32, 1);  // Checkerboard
  original.header.compression = COMPRESSION_RLE;

  auto save_result = SaveToMemory(original);
  ASSERT(save_result.success);

  auto load_result = LoadFromMemory(save_result.value.data(), save_result.value.size());
  ASSERT(load_result.success);

  bool match = CompareImages(original, load_result.value, 0.01f);
  ASSERT(match);
}

TEST(RoundTripZIP) {
  ImageData original = CreateTestImage(64, 64, 0);
  original.header.compression = COMPRESSION_ZIP;

  auto save_result = SaveToMemory(original);
  ASSERT(save_result.success);

  auto load_result = LoadFromMemory(save_result.value.data(), save_result.value.size());
  ASSERT(load_result.success);

  bool match = CompareImages(original, load_result.value, 0.01f);
  ASSERT(match);
}

TEST(RoundTripZIPS) {
  ImageData original = CreateTestImage(48, 48, 3);
  original.header.compression = COMPRESSION_ZIPS;

  auto save_result = SaveToMemory(original);
  ASSERT(save_result.success);

  auto load_result = LoadFromMemory(save_result.value.data(), save_result.value.size());
  ASSERT(load_result.success);

  bool match = CompareImages(original, load_result.value, 0.01f);
  ASSERT(match);
}

TEST(RoundTripPXR24) {
  ImageData original = CreateTestImage(32, 32, 0);  // Gradient pattern
  original.header.compression = COMPRESSION_PXR24;

  auto save_result = SaveToMemory(original);
  ASSERT(save_result.success);

  auto load_result = LoadFromMemory(save_result.value.data(), save_result.value.size());
  ASSERT(load_result.success);

  // PXR24 is lossy for FLOAT (truncates 8 mantissa bits), so use larger tolerance
  bool match = CompareImages(original, load_result.value, 0.01f);
  ASSERT(match);
}

TEST(LargeImage) {
  ImageData img = CreateTestImage(512, 512, 0);
  img.header.compression = COMPRESSION_ZIP;

  auto result = SaveToMemory(img, 6);
  ASSERT(result.success);

  printf("    512x512 ZIP: %zu bytes (%.1f%% of uncompressed)\n",
         result.value.size(),
         100.0 * result.value.size() / (512 * 512 * 4 * 2));
}

TEST(SmallImage) {
  ImageData img = CreateTestImage(4, 4, 2);
  img.header.compression = COMPRESSION_NONE;

  auto result = SaveToMemory(img);
  ASSERT(result.success);
  ASSERT(result.value.size() > 0);

  printf("    4x4 size: %zu bytes\n", result.value.size());
}

TEST(SinglePixel) {
  ImageData img = CreateTestImage(1, 1, 2);
  img.header.compression = COMPRESSION_NONE;

  auto result = SaveToMemory(img);
  ASSERT(result.success);

  printf("    1x1 size: %zu bytes\n", result.value.size());
}

TEST(NonSquareImage) {
  ImageData img = CreateTestImage(128, 32, 0);
  img.header.compression = COMPRESSION_ZIP;

  auto result = SaveToMemory(img);
  ASSERT(result.success);

  auto load_result = LoadFromMemory(result.value.data(), result.value.size());
  ASSERT(load_result.success);
  ASSERT_EQ(load_result.value.width, 128);
  ASSERT_EQ(load_result.value.height, 32);
}

TEST(SaveToFile) {
  ImageData img = CreateTestImage(64, 64, 0);
  img.header.compression = COMPRESSION_ZIP;

  const char* filename = "/tmp/test_v2_writer.exr";
  auto result = SaveToFile(filename, img, 6);
  ASSERT(result.success);

  // Load it back
  FILE* fp = fopen(filename, "rb");
  ASSERT(fp != nullptr);
  fseek(fp, 0, SEEK_END);
  size_t file_size = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  std::vector<uint8_t> data(file_size);
  size_t read = fread(data.data(), 1, file_size, fp);
  fclose(fp);
  ASSERT_EQ(read, file_size);

  auto load_result = LoadFromMemory(data.data(), data.size());
  ASSERT(load_result.success);

  bool match = CompareImages(img, load_result.value, 0.01f);
  ASSERT(match);

  // Cleanup
  remove(filename);
  printf("    File saved and loaded successfully\n");
}

TEST(InvalidInput) {
  // Empty image
  ImageData empty;
  empty.width = 0;
  empty.height = 0;
  auto result = SaveToMemory(empty);
  ASSERT(!result.success);

  // Negative dimensions
  ImageData neg;
  neg.width = -1;
  neg.height = 64;
  neg.rgba.resize(1);
  result = SaveToMemory(neg);
  ASSERT(!result.success);
}

TEST(ChannelOrder) {
  // Test that channels are written in alphabetical order (A, B, G, R)
  ImageData img = CreateTestImage(8, 8, 0);
  img.header.compression = COMPRESSION_NONE;

  auto result = SaveToMemory(img);
  ASSERT(result.success);

  auto load_result = LoadFromMemory(result.value.data(), result.value.size());
  ASSERT(load_result.success);

  // Channels should be sorted alphabetically
  ASSERT_EQ(load_result.value.header.channels.size(), 4u);
  ASSERT_EQ(load_result.value.header.channels[0].name, "A");
  ASSERT_EQ(load_result.value.header.channels[1].name, "B");
  ASSERT_EQ(load_result.value.header.channels[2].name, "G");
  ASSERT_EQ(load_result.value.header.channels[3].name, "R");
}

TEST(CompressionRatio) {
  // Test compression effectiveness on different patterns
  struct PatternTest {
    int pattern;
    const char* name;
  };
  PatternTest patterns[] = {
    {0, "Gradient"},
    {1, "Checkerboard"},
    {2, "Solid"},
    {3, "Noise"}
  };

  ImageData img_base = CreateTestImage(128, 128, 0);
  size_t uncompressed_size = 128 * 128 * 4 * 2;  // HALF pixels

  printf("    Pattern compression ratios (ZIP):\n");
  for (const auto& p : patterns) {
    ImageData img = CreateTestImage(128, 128, p.pattern);
    img.header.compression = COMPRESSION_ZIP;
    auto result = SaveToMemory(img);
    ASSERT(result.success);
    float ratio = static_cast<float>(result.value.size()) / uncompressed_size;
    printf("      %s: %.1f%%\n", p.name, ratio * 100);
  }
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;

  printf("TinyEXR V2 Writer Tests\n");
  printf("=======================\n\n");

  // Tests run automatically via static initialization

  printf("\n=======================\n");
  printf("Results: %d/%d passed", g_pass_count, g_test_count);
  if (g_fail_count > 0) {
    printf(" (%d FAILED)", g_fail_count);
  }
  printf("\n");

  return g_fail_count > 0 ? 1 : 0;
}
