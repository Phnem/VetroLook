// Test multipart EXR support in V2 API
#define TINYEXR_IMPLEMENTATION
#include "tinyexr.h"
#include "tinyexr_v2.hh"
#include "tinyexr_v2_impl.hh"

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cassert>
#include <iostream>
#include <vector>

using namespace tinyexr::v2;

static bool g_all_passed = true;
static int g_test_count = 0;
static int g_pass_count = 0;

#define TEST_ASSERT(cond, msg) do { \
    g_test_count++; \
    if (!(cond)) { \
        std::cerr << "FAILED: " << msg << " at " << __FILE__ << ":" << __LINE__ << std::endl; \
        g_all_passed = false; \
    } else { \
        g_pass_count++; \
    } \
} while(0)

// Create a simple gradient image
static ImageData CreateTestImage(int width, int height, float r_offset, float g_offset, float b_offset, const char* name) {
    ImageData img;
    img.width = width;
    img.height = height;
    img.num_channels = 4;
    img.rgba.resize(static_cast<size_t>(width) * height * 4);

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            size_t idx = (static_cast<size_t>(y) * width + x) * 4;
            img.rgba[idx + 0] = r_offset + static_cast<float>(x) / width;   // R
            img.rgba[idx + 1] = g_offset + static_cast<float>(y) / height;  // G
            img.rgba[idx + 2] = b_offset;                                    // B
            img.rgba[idx + 3] = 1.0f;                                        // A
        }
    }

    // Set up header
    img.header.data_window.min_x = 0;
    img.header.data_window.min_y = 0;
    img.header.data_window.max_x = width - 1;
    img.header.data_window.max_y = height - 1;
    img.header.display_window = img.header.data_window;
    img.header.compression = COMPRESSION_ZIP;
    img.header.name = name;
    img.header.type = "scanlineimage";

    // Add channels
    Channel ch_r, ch_g, ch_b, ch_a;
    ch_r.name = "R"; ch_r.pixel_type = PIXEL_TYPE_HALF;
    ch_g.name = "G"; ch_g.pixel_type = PIXEL_TYPE_HALF;
    ch_b.name = "B"; ch_b.pixel_type = PIXEL_TYPE_HALF;
    ch_a.name = "A"; ch_a.pixel_type = PIXEL_TYPE_HALF;
    img.header.channels.push_back(ch_a);  // alphabetical order
    img.header.channels.push_back(ch_b);
    img.header.channels.push_back(ch_g);
    img.header.channels.push_back(ch_r);

    return img;
}

// Create a tiled test image
static ImageData CreateTiledTestImage(int width, int height, float r_offset, float g_offset, float b_offset, const char* name) {
    ImageData img = CreateTestImage(width, height, r_offset, g_offset, b_offset, name);
    img.header.tiled = true;
    img.header.tile_size_x = 64;
    img.header.tile_size_y = 64;
    img.header.tile_level_mode = 0;  // ONE_LEVEL
    img.header.tile_rounding_mode = 0;
    img.header.type = "tiledimage";
    return img;
}

// Compare two images (with tolerance for half precision)
static bool CompareImages(const ImageData& a, const ImageData& b, float tolerance = 0.01f) {
    if (a.width != b.width || a.height != b.height) {
        std::cerr << "  Dimension mismatch: " << a.width << "x" << a.height
                  << " vs " << b.width << "x" << b.height << std::endl;
        return false;
    }

    if (a.rgba.size() != b.rgba.size()) {
        std::cerr << "  RGBA size mismatch: " << a.rgba.size() << " vs " << b.rgba.size() << std::endl;
        return false;
    }

    int mismatch_count = 0;
    for (size_t i = 0; i < a.rgba.size(); i++) {
        if (std::abs(a.rgba[i] - b.rgba[i]) > tolerance) {
            if (mismatch_count < 5) {
                std::cerr << "  Pixel mismatch at index " << i << ": "
                          << a.rgba[i] << " vs " << b.rgba[i] << std::endl;
            }
            mismatch_count++;
        }
    }

    if (mismatch_count > 0) {
        std::cerr << "  Total mismatches: " << mismatch_count << " / " << a.rgba.size() << std::endl;
        return false;
    }

    return true;
}

// Test 1: Create and save a multipart image with two scanline parts
static void TestMultipartScanlineWrite() {
    std::cout << "Test: Multipart scanline write..." << std::endl;

    MultipartImageData mp_data;

    // Create two image parts
    mp_data.parts.push_back(CreateTestImage(128, 128, 0.0f, 0.0f, 0.5f, "background"));
    mp_data.parts.push_back(CreateTestImage(64, 64, 0.5f, 0.0f, 0.0f, "foreground"));

    // Save to memory
    auto result = SaveMultipartToMemory(mp_data);
    TEST_ASSERT(result.success, "SaveMultipartToMemory should succeed");
    TEST_ASSERT(!result.value.empty(), "Output data should not be empty");

    // Verify EXR magic number
    TEST_ASSERT(result.value.size() >= 4, "Output should have at least 4 bytes");
    TEST_ASSERT(result.value[0] == 0x76, "Magic byte 0");
    TEST_ASSERT(result.value[1] == 0x2f, "Magic byte 1");
    TEST_ASSERT(result.value[2] == 0x31, "Magic byte 2");
    TEST_ASSERT(result.value[3] == 0x01, "Magic byte 3");

    std::cout << "  Saved multipart EXR: " << result.value.size() << " bytes" << std::endl;
}

// Test 2: Round-trip test - write and read back multipart
static void TestMultipartRoundTrip() {
    std::cout << "Test: Multipart round-trip (write then read)..." << std::endl;

    MultipartImageData mp_write;

    // Create two image parts with different sizes
    mp_write.parts.push_back(CreateTestImage(100, 80, 0.1f, 0.2f, 0.3f, "layer1"));
    mp_write.parts.push_back(CreateTestImage(50, 40, 0.4f, 0.5f, 0.6f, "layer2"));

    // Save to memory
    auto write_result = SaveMultipartToMemory(mp_write);
    TEST_ASSERT(write_result.success, "SaveMultipartToMemory should succeed");

    // Read back
    auto read_result = LoadMultipartFromMemory(write_result.value.data(), write_result.value.size());
    TEST_ASSERT(read_result.success, "LoadMultipartFromMemory should succeed");
    TEST_ASSERT(read_result.value.parts.size() == 2, "Should have 2 parts");

    // Verify dimensions
    TEST_ASSERT(read_result.value.parts[0].width == 100, "Part 0 width");
    TEST_ASSERT(read_result.value.parts[0].height == 80, "Part 0 height");
    TEST_ASSERT(read_result.value.parts[1].width == 50, "Part 1 width");
    TEST_ASSERT(read_result.value.parts[1].height == 40, "Part 1 height");

    // Compare pixel data (with tolerance for half precision conversion)
    bool part0_match = CompareImages(mp_write.parts[0], read_result.value.parts[0], 0.002f);
    bool part1_match = CompareImages(mp_write.parts[1], read_result.value.parts[1], 0.002f);

    TEST_ASSERT(part0_match, "Part 0 pixel data should match");
    TEST_ASSERT(part1_match, "Part 1 pixel data should match");

    std::cout << "  Round-trip successful!" << std::endl;
}

// Test 3: Mixed scanline and tiled multipart
static void TestMultipartMixed() {
    std::cout << "Test: Mixed scanline and tiled multipart..." << std::endl;

    MultipartImageData mp_write;

    // Create one scanline part and one tiled part
    mp_write.parts.push_back(CreateTestImage(128, 128, 0.0f, 0.0f, 0.0f, "scanline_layer"));
    mp_write.parts.push_back(CreateTiledTestImage(128, 128, 0.5f, 0.5f, 0.5f, "tiled_layer"));

    // Save to memory
    auto write_result = SaveMultipartToMemory(mp_write);
    TEST_ASSERT(write_result.success, "SaveMultipartToMemory with mixed types should succeed");

    // Read back
    auto read_result = LoadMultipartFromMemory(write_result.value.data(), write_result.value.size());
    TEST_ASSERT(read_result.success, "LoadMultipartFromMemory with mixed types should succeed");
    TEST_ASSERT(read_result.value.parts.size() == 2, "Should have 2 parts (mixed)");

    // Verify both parts
    bool scanline_match = CompareImages(mp_write.parts[0], read_result.value.parts[0], 0.002f);
    bool tiled_match = CompareImages(mp_write.parts[1], read_result.value.parts[1], 0.002f);

    TEST_ASSERT(scanline_match, "Scanline part data should match");
    TEST_ASSERT(tiled_match, "Tiled part data should match");

    std::cout << "  Mixed multipart successful!" << std::endl;
}

// Test 4: Save and load multipart to/from file
static void TestMultipartFile() {
    std::cout << "Test: Multipart file I/O..." << std::endl;

    const char* filename = "/tmp/test_multipart.exr";

    MultipartImageData mp_write;
    mp_write.parts.push_back(CreateTestImage(64, 64, 0.1f, 0.2f, 0.3f, "part_a"));
    mp_write.parts.push_back(CreateTestImage(32, 32, 0.7f, 0.8f, 0.9f, "part_b"));

    // Save to file
    auto save_result = SaveMultipartToFile(filename, mp_write);
    TEST_ASSERT(save_result.success, "SaveMultipartToFile should succeed");

    // Read file to memory
    FILE* fp = fopen(filename, "rb");
    TEST_ASSERT(fp != nullptr, "File should exist");

    fseek(fp, 0, SEEK_END);
    size_t file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    std::vector<uint8_t> file_data(file_size);
    size_t read_size = fread(file_data.data(), 1, file_size, fp);
    fclose(fp);

    TEST_ASSERT(read_size == file_size, "Should read entire file");

    // Load from memory
    auto load_result = LoadMultipartFromMemory(file_data.data(), file_data.size());
    TEST_ASSERT(load_result.success, "LoadMultipartFromMemory from file should succeed");
    TEST_ASSERT(load_result.value.parts.size() == 2, "Should have 2 parts");

    // Clean up
    remove(filename);

    std::cout << "  File I/O successful!" << std::endl;
}

// Test 5: Get part by name
static void TestGetPartByName() {
    std::cout << "Test: Get part by name..." << std::endl;

    MultipartImageData mp_write;
    mp_write.parts.push_back(CreateTestImage(64, 64, 0.0f, 0.0f, 0.0f, "diffuse"));
    mp_write.parts.push_back(CreateTestImage(64, 64, 0.5f, 0.5f, 0.5f, "normal"));
    mp_write.parts.push_back(CreateTestImage(64, 64, 1.0f, 1.0f, 1.0f, "specular"));

    auto write_result = SaveMultipartToMemory(mp_write);
    TEST_ASSERT(write_result.success, "Save should succeed");

    auto read_result = LoadMultipartFromMemory(write_result.value.data(), write_result.value.size());
    TEST_ASSERT(read_result.success, "Load should succeed");

    // Test get_part()
    const ImageData* diffuse = read_result.value.get_part("diffuse");
    const ImageData* normal = read_result.value.get_part("normal");
    const ImageData* specular = read_result.value.get_part("specular");
    const ImageData* notfound = read_result.value.get_part("notfound");

    TEST_ASSERT(diffuse != nullptr, "Should find diffuse part");
    TEST_ASSERT(normal != nullptr, "Should find normal part");
    TEST_ASSERT(specular != nullptr, "Should find specular part");
    TEST_ASSERT(notfound == nullptr, "Should not find nonexistent part");

    std::cout << "  Get part by name successful!" << std::endl;
}

// Test 6: Empty multipart (should fail gracefully)
static void TestEmptyMultipart() {
    std::cout << "Test: Empty multipart handling..." << std::endl;

    MultipartImageData empty_mp;

    auto result = SaveMultipartToMemory(empty_mp);
    TEST_ASSERT(!result.success, "Empty multipart save should fail");
    TEST_ASSERT(!result.errors.empty(), "Should have error message");

    std::cout << "  Empty multipart handled correctly" << std::endl;
}

// Test 7: Single part multipart (valid case)
static void TestSinglePartMultipart() {
    std::cout << "Test: Single part multipart..." << std::endl;

    MultipartImageData mp_write;
    mp_write.parts.push_back(CreateTestImage(100, 100, 0.5f, 0.5f, 0.5f, "only_part"));

    auto write_result = SaveMultipartToMemory(mp_write);
    TEST_ASSERT(write_result.success, "Single part multipart save should succeed");

    auto read_result = LoadMultipartFromMemory(write_result.value.data(), write_result.value.size());
    TEST_ASSERT(read_result.success, "Single part multipart load should succeed");
    TEST_ASSERT(read_result.value.parts.size() == 1, "Should have 1 part");

    std::cout << "  Single part multipart successful!" << std::endl;
}

// Test 8: Many parts
static void TestManyParts() {
    std::cout << "Test: Many parts (10 parts)..." << std::endl;

    MultipartImageData mp_write;

    for (int i = 0; i < 10; i++) {
        float offset = static_cast<float>(i) / 10.0f;
        std::string name = "layer_" + std::to_string(i);
        mp_write.parts.push_back(CreateTestImage(32, 32, offset, offset, offset, name.c_str()));
    }

    auto write_result = SaveMultipartToMemory(mp_write);
    TEST_ASSERT(write_result.success, "Many parts save should succeed");

    auto read_result = LoadMultipartFromMemory(write_result.value.data(), write_result.value.size());
    TEST_ASSERT(read_result.success, "Many parts load should succeed");
    TEST_ASSERT(read_result.value.parts.size() == 10, "Should have 10 parts");

    // Verify all parts
    bool all_match = true;
    for (int i = 0; i < 10; i++) {
        if (!CompareImages(mp_write.parts[i], read_result.value.parts[i], 0.002f)) {
            all_match = false;
            std::cerr << "  Part " << i << " mismatch" << std::endl;
        }
    }
    TEST_ASSERT(all_match, "All 10 parts should match");

    std::cout << "  Many parts successful!" << std::endl;
}

// Test 9: Different compression types
static void TestDifferentCompression() {
    std::cout << "Test: Different compression types..." << std::endl;

    MultipartImageData mp_write;

    ImageData img1 = CreateTestImage(64, 64, 0.0f, 0.0f, 0.0f, "none_comp");
    img1.header.compression = COMPRESSION_NONE;

    ImageData img2 = CreateTestImage(64, 64, 0.5f, 0.5f, 0.5f, "rle_comp");
    img2.header.compression = COMPRESSION_RLE;

    ImageData img3 = CreateTestImage(64, 64, 1.0f, 1.0f, 1.0f, "zip_comp");
    img3.header.compression = COMPRESSION_ZIP;

    mp_write.parts.push_back(img1);
    mp_write.parts.push_back(img2);
    mp_write.parts.push_back(img3);

    auto write_result = SaveMultipartToMemory(mp_write);
    TEST_ASSERT(write_result.success, "Mixed compression save should succeed");

    auto read_result = LoadMultipartFromMemory(write_result.value.data(), write_result.value.size());
    TEST_ASSERT(read_result.success, "Mixed compression load should succeed");
    TEST_ASSERT(read_result.value.parts.size() == 3, "Should have 3 parts");

    std::cout << "  Different compression types successful!" << std::endl;
}

int main() {
    std::cout << "=== TinyEXR V2 Multipart Tests ===" << std::endl << std::endl;

    TestMultipartScanlineWrite();
    TestMultipartRoundTrip();
    TestMultipartMixed();
    TestMultipartFile();
    TestGetPartByName();
    TestEmptyMultipart();
    TestSinglePartMultipart();
    TestManyParts();
    TestDifferentCompression();

    std::cout << std::endl;
    std::cout << "=== Results ===" << std::endl;
    std::cout << "Tests passed: " << g_pass_count << " / " << g_test_count << std::endl;

    if (g_all_passed) {
        std::cout << "ALL TESTS PASSED!" << std::endl;
        return 0;
    } else {
        std::cout << "SOME TESTS FAILED!" << std::endl;
        return 1;
    }
}
