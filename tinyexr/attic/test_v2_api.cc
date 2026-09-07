// Test TinyEXR V2 API with enhanced error reporting
#include <iostream>
#include <cstdio>
#include "tinyexr_v2.hh"
#include "tinyexr_v2_impl.hh"

void print_separator() {
  std::cout << "\n" << std::string(70, '=') << "\n\n";
}

void test_valid_version() {
  std::cout << "TEST 1: Valid EXR version header\n";
  print_separator();

  // Valid EXR version header
  const uint8_t data[] = {
    0x76, 0x2f, 0x31, 0x01,  // Magic number
    0x02,                     // Version 2
    0x00,                     // Flags (no special features)
    0x00, 0x00                // Padding
  };

  tinyexr::v2::Reader reader(data, sizeof(data));
  auto result = tinyexr::v2::ParseVersion(reader);

  if (result.success) {
    std::cout << "✓ SUCCESS: Parsed version header\n";
    std::cout << "  Version: " << result.value.version << "\n";
    std::cout << "  Tiled: " << (result.value.tiled ? "yes" : "no") << "\n";
    std::cout << "  Long names: " << (result.value.long_name ? "yes" : "no") << "\n";
    std::cout << "  Non-image: " << (result.value.non_image ? "yes" : "no") << "\n";
    std::cout << "  Multipart: " << (result.value.multipart ? "yes" : "no") << "\n";
  } else {
    std::cout << "✗ FAILED\n";
    std::cout << result.error_string();
  }
}

void test_invalid_magic() {
  std::cout << "TEST 2: Invalid magic number (should fail with clear error)\n";
  print_separator();

  // Invalid magic number
  const uint8_t data[] = {
    0x00, 0x00, 0x00, 0x00,  // Wrong magic
    0x02, 0x00, 0x00, 0x00
  };

  tinyexr::v2::Reader reader(data, sizeof(data));
  auto result = tinyexr::v2::ParseVersion(reader);

  if (!result.success) {
    std::cout << "✓ EXPECTED FAILURE: Correctly detected invalid magic number\n\n";
    std::cout << "Error details:\n";
    std::cout << result.error_string();
  } else {
    std::cout << "✗ TEST FAILED: Should have rejected invalid magic number\n";
  }
}

void test_invalid_version() {
  std::cout << "TEST 3: Invalid version number (should fail with clear error)\n";
  print_separator();

  // Invalid version number
  const uint8_t data[] = {
    0x76, 0x2f, 0x31, 0x01,  // Correct magic
    0x03,                     // Wrong version (3 instead of 2)
    0x00, 0x00, 0x00
  };

  tinyexr::v2::Reader reader(data, sizeof(data));
  auto result = tinyexr::v2::ParseVersion(reader);

  if (!result.success) {
    std::cout << "✓ EXPECTED FAILURE: Correctly detected invalid version\n\n";
    std::cout << "Error details:\n";
    std::cout << result.error_string();
  } else {
    std::cout << "✗ TEST FAILED: Should have rejected invalid version\n";
  }
}

void test_truncated_file() {
  std::cout << "TEST 4: Truncated file (too short, should fail)\n";
  print_separator();

  // Too short
  const uint8_t data[] = {0x76, 0x2f, 0x31};  // Only 3 bytes

  tinyexr::v2::Reader reader(data, sizeof(data));
  auto result = tinyexr::v2::ParseVersion(reader);

  if (!result.success) {
    std::cout << "✓ EXPECTED FAILURE: Correctly detected truncated file\n\n";
    std::cout << "Error details:\n";
    std::cout << result.error_string();
  } else {
    std::cout << "✗ TEST FAILED: Should have rejected truncated file\n";
  }
}

void test_tiled_format() {
  std::cout << "TEST 5: Tiled format with warnings\n";
  print_separator();

  // Tiled format
  const uint8_t data[] = {
    0x76, 0x2f, 0x31, 0x01,  // Magic
    0x02,                     // Version 2
    0x02,                     // Tiled flag set (bit 1)
    0x00, 0x00
  };

  tinyexr::v2::Reader reader(data, sizeof(data));
  auto result = tinyexr::v2::ParseVersion(reader);

  if (result.success) {
    std::cout << "✓ SUCCESS: Parsed tiled format\n";
    std::cout << "  Tiled: " << (result.value.tiled ? "yes" : "no") << "\n";

    if (!result.warnings.empty()) {
      std::cout << "\nWarnings:\n";
      std::cout << result.warnings_string() << "\n";
    }
  } else {
    std::cout << "✗ FAILED\n";
    std::cout << result.error_string();
  }
}

void test_reader_error_accumulation() {
  std::cout << "TEST 6: Reader error accumulation\n";
  print_separator();

  const uint8_t data[4] = {0x01, 0x02, 0x03, 0x04};

  tinyexr::v2::Reader reader(data, sizeof(data));
  reader.set_context("Test context");

  std::cout << "Attempting to read beyond buffer...\n";

  // Try to read more than available
  uint8_t buf[10];
  bool ok1 = reader.read(5, buf);  // Should fail (only 4 bytes)
  bool ok2 = reader.read(1, buf);  // Should also fail

  std::cout << "  First read (5 bytes): " << (ok1 ? "OK" : "FAILED") << "\n";
  std::cout << "  Second read (1 byte): " << (ok2 ? "OK" : "FAILED") << "\n";
  std::cout << "\nAccumulated errors:\n";
  std::cout << reader.error_string() << "\n";
}

int main() {
  std::cout << "\n";
  std::cout << "╔════════════════════════════════════════════════════════════════════╗\n";
  std::cout << "║         TinyEXR V2 API Test Suite - Enhanced Error Reporting      ║\n";
  std::cout << "╚════════════════════════════════════════════════════════════════════╝\n";

  test_valid_version();
  print_separator();

  test_invalid_magic();
  print_separator();

  test_invalid_version();
  print_separator();

  test_truncated_file();
  print_separator();

  test_tiled_format();
  print_separator();

  test_reader_error_accumulation();
  print_separator();

  std::cout << "\n✓ All tests completed\n\n";

  return 0;
}
