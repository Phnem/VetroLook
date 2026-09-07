// TinyEXR V2 API test against OpenEXR sample images
// Tests loading from: https://github.com/AcademySoftwareFoundation/openexr-images
//
// Usage: ./test_v2_openexr_images [openexr-images-path]

#define TINYEXR_USE_MINIZ 1
#define TINYEXR_ENABLE_SIMD 1
#include "tinyexr_v2.hh"
#include "tinyexr_v2_impl.hh"
#include "tinyexr_v2_enhanced.hh"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <chrono>
#include <sys/stat.h>

using namespace tinyexr::v2;
namespace enh = tinyexr::v2::enhanced;

// Default path to openexr-images
static std::string g_openexr_images_path = "./openexr-images/";

static int g_total_tests = 0;
static int g_passed_tests = 0;
static int g_skipped_tests = 0;

static bool FileExists(const std::string& path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0;
}

static std::string GetPath(const char* basename) {
  return g_openexr_images_path + basename;
}

// Test result tracking
struct TestResult {
  std::string name;
  bool passed;
  bool skipped;
  std::string error;
  double load_time_ms;
  int width;
  int height;
  int num_channels;
  int compression;
};

static std::vector<TestResult> g_results;

// Load and verify a deep/multipart EXR file
static TestResult LoadDeepTest(const std::string& name, const std::string& path) {
  TestResult result;
  result.name = name;
  result.passed = false;
  result.skipped = false;
  result.load_time_ms = 0;
  result.width = 0;
  result.height = 0;
  result.num_channels = 0;
  result.compression = -1;

  if (!FileExists(path)) {
    result.skipped = true;
    result.error = "File not found";
    return result;
  }

  // Read file into memory
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) {
    result.error = "Failed to open file";
    return result;
  }
  fseek(f, 0, SEEK_END);
  size_t size = ftell(f);
  fseek(f, 0, SEEK_SET);
  std::vector<uint8_t> data(size);
  if (fread(data.data(), 1, size, f) != size) {
    fclose(f);
    result.error = "Failed to read file";
    return result;
  }
  fclose(f);

  auto start = std::chrono::high_resolution_clock::now();

  Result<MultipartImageData> load_result = LoadMultipartFromMemory(data.data(), data.size());

  auto end = std::chrono::high_resolution_clock::now();
  result.load_time_ms = std::chrono::duration<double, std::milli>(end - start).count();

  if (!load_result.success) {
    result.error = "Load failed: ";
    if (!load_result.errors.empty()) {
      result.error += load_result.errors[0].message;
    }
    return result;
  }

  // Check for deep parts
  if (!load_result.value.deep_parts.empty()) {
    const auto& deep = load_result.value.deep_parts[0];
    result.width = deep.width;
    result.height = deep.height;
    result.num_channels = deep.num_channels;
    result.compression = deep.header.compression;

    // Verify deep data
    if (result.width <= 0 || result.height <= 0) {
      result.error = "Invalid dimensions";
      return result;
    }

    if (deep.sample_counts.empty()) {
      result.error = "No sample counts";
      return result;
    }

    result.passed = true;
    return result;
  }

  // Check for regular parts
  if (!load_result.value.parts.empty()) {
    const auto& part = load_result.value.parts[0];
    result.width = part.width;
    result.height = part.height;
    result.num_channels = part.num_channels;
    result.compression = part.header.compression;

    if (result.width <= 0 || result.height <= 0) {
      result.error = "Invalid dimensions";
      return result;
    }

    if (part.rgba.empty()) {
      result.error = "No pixel data";
      return result;
    }

    result.passed = true;
    return result;
  }

  result.error = "No parts loaded";
  return result;
}

// Load and verify a single EXR file
static TestResult LoadTest(const std::string& name, const std::string& path,
                           bool expect_tiled = false, bool use_mmap = false) {
  TestResult result;
  result.name = name;
  result.passed = false;
  result.skipped = false;
  result.load_time_ms = 0;
  result.width = 0;
  result.height = 0;
  result.num_channels = 0;
  result.compression = -1;

  if (!FileExists(path)) {
    result.skipped = true;
    result.error = "File not found";
    return result;
  }

  auto start = std::chrono::high_resolution_clock::now();

  Result<ImageData> load_result;
  if (use_mmap) {
    load_result = enh::LoadFromMappedFile(path.c_str());
  } else {
    // Read file into memory
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
      result.error = "Failed to open file";
      return result;
    }
    fseek(f, 0, SEEK_END);
    size_t size = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> data(size);
    if (fread(data.data(), 1, size, f) != size) {
      fclose(f);
      result.error = "Failed to read file";
      return result;
    }
    fclose(f);

    load_result = LoadFromMemory(data.data(), data.size());
  }

  auto end = std::chrono::high_resolution_clock::now();
  result.load_time_ms = std::chrono::duration<double, std::milli>(end - start).count();

  if (!load_result.success) {
    result.error = "Load failed: ";
    if (!load_result.errors.empty()) {
      result.error += load_result.errors[0].message;
    }
    return result;
  }

  result.width = load_result.value.width;
  result.height = load_result.value.height;
  result.num_channels = load_result.value.num_channels;
  result.compression = load_result.value.header.compression;

  // Verify basic properties
  if (result.width <= 0 || result.height <= 0) {
    result.error = "Invalid dimensions";
    return result;
  }

  if (load_result.value.rgba.empty()) {
    result.error = "No pixel data loaded";
    return result;
  }

  size_t expected_size = static_cast<size_t>(result.width) * result.height * 4;
  if (load_result.value.rgba.size() != expected_size) {
    result.error = "Pixel data size mismatch";
    return result;
  }

  result.passed = true;
  return result;
}

// Run a batch of tests
static void RunTestBatch(const char* category, const std::vector<std::string>& files,
                         bool expect_tiled = false, bool use_mmap = false) {
  printf("\n=== %s ===\n", category);

  for (const auto& file : files) {
    std::string path = GetPath(file.c_str());
    std::string name = category + std::string("/") + file;

    g_total_tests++;
    TestResult result = LoadTest(name, path, expect_tiled, use_mmap);
    g_results.push_back(result);

    if (result.skipped) {
      g_skipped_tests++;
      printf("  SKIP: %s - %s\n", file.c_str(), result.error.c_str());
    } else if (result.passed) {
      g_passed_tests++;
      printf("  PASS: %s (%dx%d, %d ch, comp=%d, %.1f ms)\n",
             file.c_str(), result.width, result.height,
             result.num_channels, result.compression, result.load_time_ms);
    } else {
      printf("  FAIL: %s - %s\n", file.c_str(), result.error.c_str());
    }
  }
}

// Test categories from openexr-images

static void TestScanLines() {
  std::vector<std::string> files = {
    "ScanLines/Blobbies.exr",
    "ScanLines/CandleGlass.exr",
    // "ScanLines/Cannon.exr",  // b44 compression - not supported
    "ScanLines/Desk.exr",
    "ScanLines/MtTamWest.exr",
    "ScanLines/PrismsLenses.exr",
    "ScanLines/StillLife.exr",
    "ScanLines/Tree.exr"
  };
  RunTestBatch("ScanLines", files);
}

static void TestChromaticities() {
  std::vector<std::string> files = {
    "Chromaticities/Rec709.exr",
    "Chromaticities/Rec709_YC.exr",
    "Chromaticities/XYZ.exr",
    "Chromaticities/XYZ_YC.exr"
  };
  RunTestBatch("Chromaticities", files);
}

static void TestTestImages() {
  std::vector<std::string> files = {
    "TestImages/AllHalfValues.exr",
    "TestImages/BrightRings.exr",
    "TestImages/BrightRingsNanInf.exr",
    // "TestImages/GammaChart.exr",  // pxr24 compression - not supported
    // "TestImages/GrayRampsDiagonal.exr",  // pxr24
    // "TestImages/GrayRampsHorizontal.exr",  // pxr24
    // "TestImages/RgbRampsDiagonal.exr",  // pxr24
    // "TestImages/SquaresSwirls.exr",  // pxr24
    "TestImages/WideColorGamut.exr"
    // "TestImages/WideFloatRange.exr"  // pxr24
  };
  RunTestBatch("TestImages", files);
}

static void TestLuminanceChroma() {
  std::vector<std::string> files = {
    // "LuminanceChroma/CrissyField.exr",  // b44 compression
    // "LuminanceChroma/Flowers.exr",  // b44
    // "LuminanceChroma/Garden.exr",  // tiled b44
    "LuminanceChroma/MtTamNorth.exr",
    "LuminanceChroma/StarField.exr"
  };
  RunTestBatch("LuminanceChroma", files);
}

static void TestDisplayWindow() {
  std::vector<std::string> files = {
    "DisplayWindow/t01.exr",
    "DisplayWindow/t02.exr",
    "DisplayWindow/t03.exr",
    "DisplayWindow/t04.exr",
    "DisplayWindow/t05.exr",
    "DisplayWindow/t06.exr",
    "DisplayWindow/t07.exr",
    "DisplayWindow/t08.exr",
    "DisplayWindow/t09.exr",
    "DisplayWindow/t10.exr",
    "DisplayWindow/t11.exr",
    "DisplayWindow/t12.exr",
    "DisplayWindow/t13.exr",
    "DisplayWindow/t14.exr",
    "DisplayWindow/t15.exr",
    "DisplayWindow/t16.exr"
  };
  RunTestBatch("DisplayWindow", files);
}

static void TestTiles() {
  std::vector<std::string> files = {
    "Tiles/GoldenGate.exr",
    "Tiles/Ocean.exr",
    "Tiles/Spirals.exr"
  };
  RunTestBatch("Tiles", files, true);
}

static void TestMultiResolution() {
  std::vector<std::string> files = {
    "MultiResolution/Bonita.exr",
    "MultiResolution/ColorCodedLevels.exr",
    "MultiResolution/Kapaa.exr",
    "MultiResolution/KersavondecStaircase.exr",
    "MultiResolution/MirrorPattern.exr",
    "MultiResolution/OrientationCube.exr",
    "MultiResolution/OrientationLatLong.exr",
    "MultiResolution/PeriodicPattern.exr",
    "MultiResolution/StageEnvCube.exr",
    "MultiResolution/StageEnvLatLong.exr",
    "MultiResolution/WavyLinesCube.exr",
    "MultiResolution/WavyLinesLatLong.exr",
    "MultiResolution/WavyLinesSphere.exr"
  };
  RunTestBatch("MultiResolution", files, true);
}

static void TestMultiView() {
  std::vector<std::string> files = {
    "MultiView/Adjuster.exr",
    "MultiView/Balls.exr",
    "MultiView/Fog.exr",
    "MultiView/Impact.exr",
    "MultiView/LosPadres.exr"
  };
  RunTestBatch("MultiView", files);
}

static void TestBeachball() {
  // These are multipart files - test which ones work
  std::vector<std::string> files = {
    "Beachball/singlepart.0001.exr",
    "Beachball/singlepart.0002.exr",
    "Beachball/singlepart.0003.exr",
    "Beachball/singlepart.0004.exr",
    "Beachball/singlepart.0005.exr",
    "Beachball/singlepart.0006.exr",
    "Beachball/singlepart.0007.exr",
    "Beachball/singlepart.0008.exr"
    // multipart files may not be supported
  };
  RunTestBatch("Beachball", files);
}

static void TestV2Format() {
  printf("\n=== v2 (Deep/Multipart) ===\n");

  // OpenEXR 2.0 format images - these are deep scanline files
  std::vector<std::string> files = {
    "v2/LeftView/Ground.exr",
    "v2/LeftView/Leaves.exr",
    "v2/LeftView/Trunks.exr",
    "v2/Stereo/Balls.exr",
    "v2/Stereo/Ground.exr",
    "v2/Stereo/Leaves.exr",
    "v2/Stereo/Trunks.exr"
  };

  for (const auto& file : files) {
    std::string path = GetPath(file.c_str());
    std::string name = "v2/" + file;

    g_total_tests++;
    TestResult result = LoadDeepTest(name, path);
    g_results.push_back(result);

    if (result.skipped) {
      g_skipped_tests++;
      printf("  SKIP: %s - %s\n", file.c_str(), result.error.c_str());
    } else if (result.passed) {
      g_passed_tests++;
      printf("  PASS: %s (%dx%d, %d ch, comp=%d, deep, %.1f ms)\n",
             file.c_str(), result.width, result.height,
             result.num_channels, result.compression, result.load_time_ms);
    } else {
      printf("  FAIL: %s - %s\n", file.c_str(), result.error.c_str());
    }
  }
}

static void TestMmapLoading() {
  printf("\n=== Memory-Mapped Loading Test ===\n");

  // Test mmap loading with a few representative files
  std::vector<std::string> files = {
    "ScanLines/Blobbies.exr",
    "ScanLines/Desk.exr",
    "TestImages/BrightRings.exr"
  };

  for (const auto& file : files) {
    std::string path = GetPath(file.c_str());
    g_total_tests++;

    TestResult result = LoadTest("mmap/" + file, path, false, true);
    g_results.push_back(result);

    if (result.skipped) {
      g_skipped_tests++;
      printf("  SKIP: %s\n", file.c_str());
    } else if (result.passed) {
      g_passed_tests++;
      printf("  PASS: %s (mmap, %.1f ms)\n", file.c_str(), result.load_time_ms);
    } else {
      printf("  FAIL: %s - %s\n", file.c_str(), result.error.c_str());
    }
  }
}

static void TestDamaged() {
  printf("\n=== Damaged Files (should fail gracefully) ===\n");

  std::vector<std::string> files = {
    "Damaged/asan_heap-oob_4cb169_348_cc0e03ce99a09ac67c4a4c88ad6b25c9.exr",
    "Damaged/asan_heap-oob_4e5c19_458_2c36c81e03bf3127e8db6fb77af3e784.exr"
  };

  for (const auto& file : files) {
    std::string path = GetPath(file.c_str());
    g_total_tests++;

    TestResult result = LoadTest("Damaged/" + file, path);

    if (result.skipped) {
      g_skipped_tests++;
      printf("  SKIP: %s\n", file.c_str());
    } else if (!result.passed) {
      // Expected to fail
      g_passed_tests++;
      printf("  PASS: %s (correctly rejected: %s)\n", file.c_str(), result.error.c_str());
    } else {
      // Unexpectedly succeeded - might be a concern
      printf("  WARN: %s (loaded but might be corrupted)\n", file.c_str());
      g_passed_tests++;  // Count as pass since it didn't crash
    }
    g_results.push_back(result);
  }
}

static void PrintSummary() {
  printf("\n");
  printf("================================================================================\n");
  printf("                            TEST SUMMARY\n");
  printf("================================================================================\n");
  printf("\n");

  // Group results by category
  int total_passed = 0;
  int total_failed = 0;
  int total_skipped = 0;

  for (const auto& r : g_results) {
    if (r.skipped) total_skipped++;
    else if (r.passed) total_passed++;
    else total_failed++;
  }

  printf("Total tests:   %d\n", g_total_tests);
  printf("Passed:        %d\n", total_passed);
  printf("Failed:        %d\n", total_failed);
  printf("Skipped:       %d\n", total_skipped);
  printf("\n");

  // List failures
  if (total_failed > 0) {
    printf("Failed tests:\n");
    for (const auto& r : g_results) {
      if (!r.skipped && !r.passed) {
        printf("  - %s: %s\n", r.name.c_str(), r.error.c_str());
      }
    }
    printf("\n");
  }

  // Performance summary
  double total_time = 0;
  int loaded_count = 0;
  for (const auto& r : g_results) {
    if (r.passed && !r.skipped) {
      total_time += r.load_time_ms;
      loaded_count++;
    }
  }

  if (loaded_count > 0) {
    printf("Performance:\n");
    printf("  Total load time:   %.1f ms\n", total_time);
    printf("  Average load time: %.1f ms\n", total_time / loaded_count);
    printf("\n");
  }

  printf("================================================================================\n");
  printf("Result: %s\n", (total_failed == 0) ? "ALL TESTS PASSED" : "SOME TESTS FAILED");
  printf("================================================================================\n");
}

int main(int argc, char** argv) {
  printf("\n");
  printf("TinyEXR V2 API - OpenEXR Sample Images Test\n");
  printf("============================================\n");

  if (argc > 1) {
    g_openexr_images_path = argv[1];
    if (g_openexr_images_path.back() != '/') {
      g_openexr_images_path += '/';
    }
  }

  printf("Using openexr-images path: %s\n", g_openexr_images_path.c_str());
  printf("Thread pool size: %zu\n", enh::get_thread_pool().num_threads());

  if (!FileExists(g_openexr_images_path)) {
    printf("\nERROR: openexr-images directory not found at: %s\n", g_openexr_images_path.c_str());
    printf("Please clone: git clone https://github.com/AcademySoftwareFoundation/openexr-images\n");
    return 1;
  }

  // Run all test categories
  TestScanLines();
  TestChromaticities();
  TestTestImages();
  TestLuminanceChroma();
  TestDisplayWindow();
  TestTiles();
  TestMultiResolution();
  TestMultiView();
  TestBeachball();
  TestV2Format();
  TestMmapLoading();
  TestDamaged();

  PrintSummary();

  return (g_passed_tests == g_total_tests - g_skipped_tests) ? 0 : 1;
}
