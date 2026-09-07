// Test StreamWriter and v2 Writer API
#include <iostream>
#include <iomanip>
#include "tinyexr_v2.hh"
#include "tinyexr_v2_impl.hh"

void print_separator() {
  std::cout << "\n" << std::string(70, '=') << "\n\n";
}

void print_hex(const uint8_t* data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    std::cout << std::hex << std::setfill('0') << std::setw(2)
              << static_cast<int>(data[i]) << " ";
    if ((i + 1) % 16 == 0) std::cout << "\n";
  }
  std::cout << std::dec << "\n";
}

void test_streamwriter_dynamic() {
  std::cout << "TEST 1: StreamWriter dynamic mode\n";
  print_separator();

  tinyexr::StreamWriter writer(tinyexr::Endian::Little);

  std::cout << "Writing magic number, version, and some data...\n";

  // Write magic
  const uint8_t magic[] = {0x76, 0x2f, 0x31, 0x01};
  if (!writer.write(4, magic)) {
    std::cout << "✗ FAILED to write magic\n";
    return;
  }

  // Write version
  if (!writer.write1(0x02)) {
    std::cout << "✗ FAILED to write version\n";
    return;
  }

  // Write flags
  if (!writer.write1(0x00)) {
    std::cout << "✗ FAILED to write flags\n";
    return;
  }

  // Write padding
  if (!writer.write2(0x0000)) {
    std::cout << "✗ FAILED to write padding\n";
    return;
  }

  // Write a 32-bit value
  if (!writer.write4(0x12345678)) {
    std::cout << "✗ FAILED to write 32-bit value\n";
    return;
  }

  std::cout << "✓ Wrote " << writer.size() << " bytes\n\n";
  std::cout << "Data (hex):\n";
  print_hex(writer.data_ptr(), writer.size());

  std::cout << "Position: " << writer.tell() << "\n";
  std::cout << "Size: " << writer.size() << "\n";
}

void test_streamwriter_bounded() {
  std::cout << "TEST 2: StreamWriter bounded mode\n";
  print_separator();

  uint8_t buffer[64];
  tinyexr::StreamWriter writer(buffer, sizeof(buffer), tinyexr::Endian::Little);

  std::cout << "Writing to fixed 64-byte buffer...\n";

  // Write some data
  if (!writer.write4(0xDEADBEEF)) {
    std::cout << "✗ FAILED to write\n";
    return;
  }

  if (!writer.write_float(3.14159f)) {
    std::cout << "✗ FAILED to write float\n";
    return;
  }

  if (!writer.write_string("Hello")) {
    std::cout << "✗ FAILED to write string\n";
    return;
  }

  std::cout << "✓ Wrote " << writer.size() << " bytes\n";
  std::cout << "Capacity: " << writer.capacity() << "\n";
  std::cout << "Remaining: " << writer.remaining() << "\n\n";

  std::cout << "Data (hex):\n";
  print_hex(writer.data_ptr(), writer.size());

  // Try to write beyond capacity
  std::cout << "\nAttempting to write beyond capacity...\n";
  uint8_t large_data[100];
  if (!writer.write(100, large_data)) {
    std::cout << "✓ Correctly rejected out-of-bounds write\n";
  } else {
    std::cout << "✗ Should have rejected write\n";
  }
}

void test_v2_writer_error_accumulation() {
  std::cout << "TEST 3: v2::Writer error accumulation\n";
  print_separator();

  uint8_t buffer[10];  // Very small buffer
  tinyexr::v2::Writer writer(buffer, sizeof(buffer));
  writer.set_context("Test write operations");

  std::cout << "Attempting to write beyond small 10-byte buffer...\n\n";

  uint8_t data[20];
  bool ok1 = writer.write(20, data);  // Should fail
  bool ok2 = writer.write4(0x12345678);  // Should also fail

  std::cout << "  Write 20 bytes: " << (ok1 ? "OK" : "FAILED") << "\n";
  std::cout << "  Write 4 bytes: " << (ok2 ? "OK" : "FAILED") << "\n\n";

  if (writer.has_error()) {
    std::cout << "Accumulated errors:\n";
    std::cout << writer.error_string() << "\n";
  }
}

void test_write_version() {
  std::cout << "TEST 4: Write EXR version header\n";
  print_separator();

  tinyexr::v2::Writer writer;
  tinyexr::v2::Version version;
  version.version = 2;
  version.tiled = false;
  version.long_name = false;
  version.non_image = false;
  version.multipart = false;

  auto result = tinyexr::v2::WriteVersion(writer, version);

  if (!result.success) {
    std::cout << "✗ FAILED to write version\n";
    std::cout << result.error_string();
    return;
  }

  std::cout << "✓ Successfully wrote version header\n";
  std::cout << "Size: " << writer.size() << " bytes (expected 8)\n\n";

  std::cout << "Data (hex):\n";
  print_hex(writer.data_ptr(), writer.size());

  // Verify by reading it back
  tinyexr::v2::Reader reader(writer.data_ptr(), writer.size());
  auto read_result = tinyexr::v2::ParseVersion(reader);

  if (read_result.success) {
    std::cout << "✓ Successfully read back version\n";
    std::cout << "  Version: " << read_result.value.version << "\n";
  } else {
    std::cout << "✗ Failed to read back\n";
  }
}

void test_roundtrip() {
  std::cout << "TEST 5: Write and read back (round-trip)\n";
  print_separator();

  // Create minimal image data
  tinyexr::v2::ImageData image;
  image.width = 64;
  image.height = 64;
  image.num_channels = 4;
  image.header.data_window.min_x = 0;
  image.header.data_window.min_y = 0;
  image.header.data_window.max_x = 63;
  image.header.data_window.max_y = 63;
  image.header.display_window = image.header.data_window;
  image.header.compression = 0;  // No compression
  image.header.line_order = 0;
  image.header.pixel_aspect_ratio = 1.0f;
  image.header.screen_window_center[0] = 0.0f;
  image.header.screen_window_center[1] = 0.0f;
  image.header.screen_window_width = 1.0f;
  image.header.tiled = false;

  // Save to memory
  std::cout << "Saving image to memory...\n";
  auto save_result = tinyexr::v2::SaveToMemory(image);

  if (!save_result.success) {
    std::cout << "✗ FAILED to save\n";
    std::cout << save_result.error_string();
    return;
  }

  std::cout << "✓ Saved " << save_result.value.size() << " bytes\n";

  if (!save_result.warnings.empty()) {
    std::cout << "\nWarnings:\n" << save_result.warnings_string() << "\n";
  }

  std::cout << "\nFirst 64 bytes (hex):\n";
  print_hex(save_result.value.data(), std::min(save_result.value.size(), size_t(64)));

  // Try to load it back
  std::cout << "\nLoading back from memory...\n";
  auto load_result = tinyexr::v2::LoadFromMemory(
    save_result.value.data(), save_result.value.size());

  if (!load_result.success) {
    std::cout << "✗ FAILED to load back\n";
    std::cout << load_result.error_string();
    return;
  }

  std::cout << "✓ Loaded successfully\n";
  std::cout << "  Dimensions: " << load_result.value.width
            << "x" << load_result.value.height << "\n";
  std::cout << "  Compression: " << load_result.value.header.compression << "\n";
  std::cout << "  Pixel aspect: " << load_result.value.header.pixel_aspect_ratio << "\n";
}

int main() {
  std::cout << "\n";
  std::cout << "╔════════════════════════════════════════════════════════════════════╗\n";
  std::cout << "║      TinyEXR V2 Writer API Test Suite                             ║\n";
  std::cout << "╚════════════════════════════════════════════════════════════════════╝\n";

  test_streamwriter_dynamic();
  print_separator();

  test_streamwriter_bounded();
  print_separator();

  test_v2_writer_error_accumulation();
  print_separator();

  test_write_version();
  print_separator();

  test_roundtrip();
  print_separator();

  std::cout << "\n✓ All tests completed\n\n";

  return 0;
}
