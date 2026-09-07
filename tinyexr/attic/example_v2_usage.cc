// Example: Using TinyEXR V2 API with enhanced error reporting
//
// This example demonstrates the improved error handling in the v2 API

#include <iostream>
#include <fstream>
#include <vector>
#include "tinyexr_v2.hh"
#include "tinyexr_v2_impl.hh"

// Helper to read file into memory
std::vector<uint8_t> ReadFile(const char* filename) {
  std::ifstream file(filename, std::ios::binary | std::ios::ate);
  if (!file.is_open()) {
    return std::vector<uint8_t>();
  }

  std::streamsize size = file.tellg();
  file.seekg(0, std::ios::beg);

  std::vector<uint8_t> buffer(size);
  if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
    return std::vector<uint8_t>();
  }

  return buffer;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cout << "Usage: " << argv[0] << " <exr_file>\n";
    std::cout << "\nExample demonstrating TinyEXR V2 API with enhanced error reporting\n";
    return 1;
  }

  const char* filename = argv[1];

  std::cout << "═══════════════════════════════════════════════════════════════\n";
  std::cout << "  TinyEXR V2 API Example - Enhanced Error Reporting\n";
  std::cout << "═══════════════════════════════════════════════════════════════\n\n";

  // Step 1: Read file
  std::cout << "Reading file: " << filename << "\n";
  std::vector<uint8_t> file_data = ReadFile(filename);

  if (file_data.empty()) {
    std::cerr << "\n[ERROR] Failed to read file\n";
    std::cerr << "  File may not exist or is not readable\n";
    return 1;
  }

  std::cout << "  File size: " << file_data.size() << " bytes\n\n";

  // Step 2: Parse with V2 API
  std::cout << "Parsing EXR file...\n";
  auto result = tinyexr::v2::LoadFromMemory(file_data.data(), file_data.size());

  // Step 3: Check result
  if (!result.success) {
    std::cerr << "\n╔═══════════════════════════════════════════════════════════╗\n";
    std::cerr << "║  PARSING FAILED - Detailed Error Information             ║\n";
    std::cerr << "╚═══════════════════════════════════════════════════════════╝\n\n";
    std::cerr << result.error_string() << "\n";

    if (!result.warnings.empty()) {
      std::cout << "\nWarnings (before failure):\n";
      std::cout << result.warnings_string() << "\n";
    }

    return 1;
  }

  // Step 4: Display success information
  std::cout << "\n╔═══════════════════════════════════════════════════════════╗\n";
  std::cout << "║  PARSING SUCCESSFUL                                       ║\n";
  std::cout << "╚═══════════════════════════════════════════════════════════╝\n\n";

  const auto& img = result.value;
  const auto& hdr = img.header;

  std::cout << "Image Information:\n";
  std::cout << "  Width:  " << img.width << "\n";
  std::cout << "  Height: " << img.height << "\n";
  std::cout << "  Channels: " << img.num_channels << "\n\n";

  std::cout << "Data Window:\n";
  std::cout << "  Min: (" << hdr.data_window.min_x << ", "
            << hdr.data_window.min_y << ")\n";
  std::cout << "  Max: (" << hdr.data_window.max_x << ", "
            << hdr.data_window.max_y << ")\n\n";

  std::cout << "Display Window:\n";
  std::cout << "  Min: (" << hdr.display_window.min_x << ", "
            << hdr.display_window.min_y << ")\n";
  std::cout << "  Max: (" << hdr.display_window.max_x << ", "
            << hdr.display_window.max_y << ")\n\n";

  std::cout << "Format:\n";
  std::cout << "  Compression: " << hdr.compression << "\n";
  std::cout << "  Line order: " << hdr.line_order << "\n";
  std::cout << "  Pixel aspect: " << hdr.pixel_aspect_ratio << "\n";
  std::cout << "  Tiled: " << (hdr.tiled ? "yes" : "no") << "\n";

  if (hdr.tiled) {
    std::cout << "  Tile size: " << hdr.tile_size_x << "x" << hdr.tile_size_y << "\n";
  }

  // Display warnings if any
  if (!result.warnings.empty()) {
    std::cout << "\n" << std::string(63, '-') << "\n";
    std::cout << "Warnings:\n";
    std::cout << result.warnings_string() << "\n";
  }

  std::cout << "\n" << std::string(63, '=') << "\n";
  std::cout << "✓ Processing complete\n\n";

  return 0;
}
