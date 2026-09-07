// Debug test for multipart EXR
#define TINYEXR_IMPLEMENTATION
#include "tinyexr.h"
#include "tinyexr_v2.hh"
#include "tinyexr_v2_impl.hh"

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <iostream>

using namespace tinyexr::v2;

int main() {
    std::cout << "=== Debug Multipart Test ===" << std::endl;

    // Create a very simple 4x4 image with COMPRESSION_NONE
    MultipartImageData mp_write;

    ImageData img;
    img.width = 4;
    img.height = 4;
    img.num_channels = 4;
    img.rgba.resize(4 * 4 * 4);

    // Fill with known values
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            size_t idx = (y * 4 + x) * 4;
            img.rgba[idx + 0] = 0.25f;  // R
            img.rgba[idx + 1] = 0.50f;  // G
            img.rgba[idx + 2] = 0.75f;  // B
            img.rgba[idx + 3] = 1.00f;  // A
        }
    }

    // Set up header with NO compression
    img.header.data_window.min_x = 0;
    img.header.data_window.min_y = 0;
    img.header.data_window.max_x = 3;
    img.header.data_window.max_y = 3;
    img.header.display_window = img.header.data_window;
    img.header.compression = COMPRESSION_NONE;
    img.header.name = "test_part";
    img.header.type = "scanlineimage";

    // Add channels (alphabetically sorted)
    Channel ch_a, ch_b, ch_g, ch_r;
    ch_a.name = "A"; ch_a.pixel_type = PIXEL_TYPE_HALF;
    ch_b.name = "B"; ch_b.pixel_type = PIXEL_TYPE_HALF;
    ch_g.name = "G"; ch_g.pixel_type = PIXEL_TYPE_HALF;
    ch_r.name = "R"; ch_r.pixel_type = PIXEL_TYPE_HALF;
    img.header.channels.push_back(ch_a);
    img.header.channels.push_back(ch_b);
    img.header.channels.push_back(ch_g);
    img.header.channels.push_back(ch_r);

    mp_write.parts.push_back(img);

    std::cout << "Original pixel[0]: R=" << img.rgba[0] << " G=" << img.rgba[1]
              << " B=" << img.rgba[2] << " A=" << img.rgba[3] << std::endl;

    // Save to memory
    auto write_result = SaveMultipartToMemory(mp_write);
    if (!write_result.success) {
        std::cerr << "SaveMultipartToMemory failed!" << std::endl;
        for (auto& e : write_result.errors) {
            std::cerr << "  Error: " << e.message << std::endl;
        }
        return 1;
    }
    std::cout << "Saved " << write_result.value.size() << " bytes" << std::endl;

    // Read back
    auto read_result = LoadMultipartFromMemory(write_result.value.data(), write_result.value.size());
    if (!read_result.success) {
        std::cerr << "LoadMultipartFromMemory failed!" << std::endl;
        for (auto& e : read_result.errors) {
            std::cerr << "  Error: " << e.message << std::endl;
        }
        return 1;
    }

    std::cout << "Loaded " << read_result.value.parts.size() << " parts" << std::endl;

    if (read_result.value.parts.empty()) {
        std::cerr << "No parts loaded!" << std::endl;
        return 1;
    }

    const ImageData& read_img = read_result.value.parts[0];
    std::cout << "Read image: " << read_img.width << "x" << read_img.height << std::endl;
    std::cout << "RGBA size: " << read_img.rgba.size() << std::endl;
    std::cout << "Header channels: " << read_img.header.channels.size() << std::endl;
    for (size_t c = 0; c < read_img.header.channels.size(); c++) {
        std::cout << "  Channel " << c << ": " << read_img.header.channels[c].name
                  << " (type " << read_img.header.channels[c].pixel_type << ")" << std::endl;
    }

    std::cout << "Read pixel[0]: R=" << read_img.rgba[0] << " G=" << read_img.rgba[1]
              << " B=" << read_img.rgba[2] << " A=" << read_img.rgba[3] << std::endl;

    // Compare first few pixels
    int mismatches = 0;
    for (size_t i = 0; i < std::min(size_t(16), img.rgba.size()); i++) {
        float orig = img.rgba[i];
        float read_val = read_img.rgba[i];
        float diff = std::abs(orig - read_val);
        if (diff > 0.01f) {
            std::cout << "  Mismatch at " << i << ": " << orig << " vs " << read_val << std::endl;
            mismatches++;
        }
    }

    if (mismatches == 0) {
        std::cout << "SUCCESS: All values match!" << std::endl;
        return 0;
    } else {
        std::cout << "FAILED: " << mismatches << " mismatches" << std::endl;
        return 1;
    }
}
