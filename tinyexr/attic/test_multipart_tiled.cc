// Debug test for tiled multipart EXR
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
    std::cout << "=== Debug Tiled Multipart Test ===" << std::endl;

    // Create a multipart with one scanline and one tiled part
    MultipartImageData mp_write;

    // Part 1: Scanline (128x128)
    {
        ImageData img;
        img.width = 128;
        img.height = 128;
        img.num_channels = 4;
        img.rgba.resize(128 * 128 * 4);

        for (size_t i = 0; i < img.rgba.size(); i += 4) {
            img.rgba[i + 0] = 0.25f;
            img.rgba[i + 1] = 0.50f;
            img.rgba[i + 2] = 0.75f;
            img.rgba[i + 3] = 1.00f;
        }

        img.header.data_window.min_x = 0;
        img.header.data_window.min_y = 0;
        img.header.data_window.max_x = 127;
        img.header.data_window.max_y = 127;
        img.header.display_window = img.header.data_window;
        img.header.compression = COMPRESSION_NONE;
        img.header.name = "scanline_part";
        img.header.type = "scanlineimage";

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
    }

    // Part 2: Tiled (128x128)
    {
        ImageData img;
        img.width = 128;
        img.height = 128;
        img.num_channels = 4;
        img.rgba.resize(128 * 128 * 4);

        for (size_t i = 0; i < img.rgba.size(); i += 4) {
            img.rgba[i + 0] = 0.75f;
            img.rgba[i + 1] = 0.50f;
            img.rgba[i + 2] = 0.25f;
            img.rgba[i + 3] = 1.00f;
        }

        img.header.data_window.min_x = 0;
        img.header.data_window.min_y = 0;
        img.header.data_window.max_x = 127;
        img.header.data_window.max_y = 127;
        img.header.display_window = img.header.data_window;
        img.header.compression = COMPRESSION_NONE;
        img.header.name = "tiled_part";
        img.header.type = "tiledimage";
        img.header.tiled = true;
        img.header.tile_size_x = 64;
        img.header.tile_size_y = 64;
        img.header.tile_level_mode = 0;
        img.header.tile_rounding_mode = 0;

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
    }

    std::cout << "Writing 2 parts..." << std::endl;

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
    std::cout << "Headers count: " << read_result.value.headers.size() << std::endl;
    for (size_t i = 0; i < read_result.value.headers.size(); i++) {
        std::cout << "  Header " << i << ": " << read_result.value.headers[i].name
                  << " (tiled=" << read_result.value.headers[i].tiled
                  << ", type=" << read_result.value.headers[i].type
                  << ", chunk_count=" << read_result.value.headers[i].chunk_count << ")" << std::endl;
    }
    for (auto& w : read_result.warnings) {
        std::cout << "  Warning: " << w << std::endl;
    }

    if (read_result.value.parts.size() != 2) {
        std::cerr << "Expected 2 parts, got " << read_result.value.parts.size() << std::endl;
        // Continue anyway to see what we got
        if (read_result.value.parts.size() > 0) {
            std::cout << "Part 0: " << read_result.value.parts[0].width << "x"
                      << read_result.value.parts[0].height << std::endl;
            std::cout << "  Name: " << read_result.value.parts[0].header.name << std::endl;
        }
        return 1;
    }

    std::cout << "Part 0: " << read_result.value.parts[0].width << "x"
              << read_result.value.parts[0].height << std::endl;
    std::cout << "  Name: " << read_result.value.parts[0].header.name << std::endl;
    std::cout << "  Tiled: " << read_result.value.parts[0].header.tiled << std::endl;
    std::cout << "  Pixel[0]: R=" << read_result.value.parts[0].rgba[0]
              << " G=" << read_result.value.parts[0].rgba[1]
              << " B=" << read_result.value.parts[0].rgba[2]
              << " A=" << read_result.value.parts[0].rgba[3] << std::endl;

    std::cout << "Part 1: " << read_result.value.parts[1].width << "x"
              << read_result.value.parts[1].height << std::endl;
    std::cout << "  Name: " << read_result.value.parts[1].header.name << std::endl;
    std::cout << "  Tiled: " << read_result.value.parts[1].header.tiled << std::endl;
    std::cout << "  Pixel[0]: R=" << read_result.value.parts[1].rgba[0]
              << " G=" << read_result.value.parts[1].rgba[1]
              << " B=" << read_result.value.parts[1].rgba[2]
              << " A=" << read_result.value.parts[1].rgba[3] << std::endl;

    // Verify pixel values
    bool pass = true;

    // Part 0 (scanline): should be 0.25, 0.50, 0.75, 1.00
    if (std::abs(read_result.value.parts[0].rgba[0] - 0.25f) > 0.01f ||
        std::abs(read_result.value.parts[0].rgba[1] - 0.50f) > 0.01f ||
        std::abs(read_result.value.parts[0].rgba[2] - 0.75f) > 0.01f) {
        std::cerr << "Part 0 pixel mismatch!" << std::endl;
        pass = false;
    }

    // Part 1 (tiled): should be 0.75, 0.50, 0.25, 1.00
    if (std::abs(read_result.value.parts[1].rgba[0] - 0.75f) > 0.01f ||
        std::abs(read_result.value.parts[1].rgba[1] - 0.50f) > 0.01f ||
        std::abs(read_result.value.parts[1].rgba[2] - 0.25f) > 0.01f) {
        std::cerr << "Part 1 pixel mismatch!" << std::endl;
        pass = false;
    }

    if (pass) {
        std::cout << "SUCCESS!" << std::endl;
        return 0;
    } else {
        std::cout << "FAILED!" << std::endl;
        return 1;
    }
}
