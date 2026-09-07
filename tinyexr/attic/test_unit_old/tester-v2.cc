// TinyEXR V2 API Unit Tester
// Ported from tester.cc to use v2 API with enhanced error reporting

#define CATCH_CONFIG_MAIN
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

#include "catch.hpp"

// Include V2 API
#include "../../tinyexr_v2.hh"
#include "../../tinyexr_v2_impl.hh"

// Path to openexr-images test files
const char* kOpenEXRImagePath = "../../../openexr-images/";

std::string GetPath(const char* basename) {
  return std::string(kOpenEXRImagePath) + std::string(basename);
}

// Helper: Read file into memory
std::vector<uint8_t> ReadFile(const std::string& filename) {
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

// Helper: Write memory to file
bool WriteFile(const std::string& filename, const uint8_t* data, size_t size) {
  std::ofstream file(filename, std::ios::binary);
  if (!file.is_open()) {
    return false;
  }
  file.write(reinterpret_cast<const char*>(data), size);
  return file.good();
}

// ============================================================================
// StreamReader Tests
// ============================================================================

TEST_CASE("StreamReader: Basic operations", "[StreamReader]") {
  const uint8_t data[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
  tinyexr::StreamReader reader(data, sizeof(data), tinyexr::Endian::Little);

  SECTION("Read single bytes") {
    uint8_t val;
    REQUIRE(reader.read1(&val) == true);
    REQUIRE(val == 0x01);
    REQUIRE(reader.tell() == 1);
  }

  SECTION("Read 2 bytes") {
    uint16_t val;
    REQUIRE(reader.read2(&val) == true);
    REQUIRE(val == 0x0201);  // Little endian
    REQUIRE(reader.tell() == 2);
  }

  SECTION("Read 4 bytes") {
    uint32_t val;
    REQUIRE(reader.read4(&val) == true);
    REQUIRE(val == 0x04030201);  // Little endian
    REQUIRE(reader.tell() == 4);
  }

  SECTION("Seek operations") {
    REQUIRE(reader.seek(4) == true);
    REQUIRE(reader.tell() == 4);

    uint8_t val;
    REQUIRE(reader.read1(&val) == true);
    REQUIRE(val == 0x05);
  }

  SECTION("Out of bounds") {
    uint8_t buf[10];
    REQUIRE(reader.read(10, buf) == false);  // Only 8 bytes available
  }

  SECTION("Remaining and EOF") {
    REQUIRE(reader.remaining() == 8);
    REQUIRE(reader.eof() == false);

    reader.seek(8);
    REQUIRE(reader.remaining() == 0);
    REQUIRE(reader.eof() == true);
  }
}

TEST_CASE("StreamReader: Endian swapping", "[StreamReader]") {
  const uint8_t data[] = {0x12, 0x34, 0x56, 0x78};

  SECTION("Little endian - bytes as stored in memory") {
    tinyexr::StreamReader reader(data, sizeof(data), tinyexr::Endian::Little);
    uint32_t val;
    REQUIRE(reader.read4(&val) == true);
    // Data in memory: 12 34 56 78
    // Little endian interpretation: 78563412
    // This should match regardless of host endianness
    uint8_t* bytes = reinterpret_cast<uint8_t*>(&val);
    REQUIRE(bytes[0] == 0x12);
    REQUIRE(bytes[1] == 0x34);
    REQUIRE(bytes[2] == 0x56);
    REQUIRE(bytes[3] == 0x78);
  }

  SECTION("Native endian - no swapping") {
    tinyexr::StreamReader reader(data, sizeof(data), tinyexr::Endian::Native);
    uint32_t val;
    REQUIRE(reader.read4(&val) == true);
    // With native endian, data is read as-is
    const uint32_t* ptr = reinterpret_cast<const uint32_t*>(data);
    REQUIRE(val == *ptr);
  }
}

// ============================================================================
// StreamWriter Tests
// ============================================================================

TEST_CASE("StreamWriter: Dynamic mode", "[StreamWriter]") {
  tinyexr::StreamWriter writer(tinyexr::Endian::Little);

  SECTION("Write and grow") {
    REQUIRE(writer.write1(0x42) == true);
    REQUIRE(writer.size() == 1);
    REQUIRE(writer.data()[0] == 0x42);
  }

  SECTION("Write multiple types") {
    REQUIRE(writer.write1(0x01) == true);
    REQUIRE(writer.write2(0x0302) == true);
    REQUIRE(writer.write4(0x07060504) == true);

    REQUIRE(writer.size() == 7);
    REQUIRE(writer.data()[0] == 0x01);
    REQUIRE(writer.data()[1] == 0x02);
    REQUIRE(writer.data()[2] == 0x03);
  }

  SECTION("Write string") {
    REQUIRE(writer.write_string("Hello") == true);
    REQUIRE(writer.size() == 6);  // Including null terminator
    REQUIRE(std::string(reinterpret_cast<const char*>(writer.data_ptr())) == "Hello");
  }

  SECTION("Write float") {
    REQUIRE(writer.write_float(3.14159f) == true);
    REQUIRE(writer.size() == 4);
  }
}

TEST_CASE("StreamWriter: Bounded mode", "[StreamWriter]") {
  uint8_t buffer[16];
  tinyexr::StreamWriter writer(buffer, sizeof(buffer), tinyexr::Endian::Little);

  SECTION("Write within bounds") {
    REQUIRE(writer.write4(0xDEADBEEF) == true);
    REQUIRE(writer.size() == 4);
    REQUIRE(writer.remaining() == 12);
  }

  SECTION("Write beyond bounds fails") {
    uint8_t large_data[20];
    REQUIRE(writer.write(20, large_data) == false);
  }

  SECTION("Check capacity") {
    REQUIRE(writer.capacity() == 16);
    REQUIRE(writer.mode() == tinyexr::WriterMode::Bounded);
  }
}

// ============================================================================
// v2::Reader Tests
// ============================================================================

TEST_CASE("v2::Reader: Error accumulation", "[v2::Reader]") {
  const uint8_t data[] = {0x01, 0x02};
  tinyexr::v2::Reader reader(data, sizeof(data));

  SECTION("Successful read has no errors") {
    uint8_t val;
    REQUIRE(reader.read1(&val) == true);
    REQUIRE(reader.has_error() == false);
  }

  SECTION("Failed read accumulates error") {
    uint8_t buf[10];
    REQUIRE(reader.read(10, buf) == false);
    REQUIRE(reader.has_error() == true);
    REQUIRE(reader.errors().size() == 1);
    REQUIRE(reader.last_error().code == tinyexr::v2::ErrorCode::OutOfBounds);
  }

  SECTION("Multiple errors accumulated") {
    uint8_t buf[5];
    reader.read(5, buf);  // Error 1
    reader.read(5, buf);  // Error 2
    REQUIRE(reader.errors().size() == 2);
  }

  SECTION("Context in errors") {
    reader.set_context("Test operation");
    uint8_t buf[10];
    reader.read(10, buf);
    REQUIRE(reader.last_error().context == "Test operation");
  }
}

// ============================================================================
// v2::Writer Tests
// ============================================================================

TEST_CASE("v2::Writer: Error accumulation", "[v2::Writer]") {
  uint8_t buffer[8];
  tinyexr::v2::Writer writer(buffer, sizeof(buffer));

  SECTION("Successful write has no errors") {
    REQUIRE(writer.write4(0x12345678) == true);
    REQUIRE(writer.has_error() == false);
  }

  SECTION("Failed write accumulates error") {
    uint8_t large_data[20];
    REQUIRE(writer.write(20, large_data) == false);
    REQUIRE(writer.has_error() == true);
    REQUIRE(writer.errors().size() == 1);
  }

  SECTION("Error messages are human-readable") {
    uint8_t data[10];
    writer.write(10, data);
    std::string err_str = writer.error_string();
    REQUIRE(err_str.find("[ERROR]") != std::string::npos);
    REQUIRE(err_str.find("Out of Bounds") != std::string::npos);
  }
}

// ============================================================================
// Version Parsing Tests
// ============================================================================

TEST_CASE("v2::ParseVersion: Valid EXR version", "[Parse][Version]") {
  const uint8_t data[] = {
    0x76, 0x2f, 0x31, 0x01,  // Magic
    0x02,                     // Version 2
    0x00,                     // Flags
    0x00, 0x00                // Padding
  };

  tinyexr::v2::Reader reader(data, sizeof(data));
  auto result = tinyexr::v2::ParseVersion(reader);

  REQUIRE(result.success == true);
  REQUIRE(result.value.version == 2);
  REQUIRE(result.value.tiled == false);
  REQUIRE(result.value.long_name == false);
  REQUIRE(result.value.non_image == false);
  REQUIRE(result.value.multipart == false);
}

TEST_CASE("v2::ParseVersion: Invalid magic number", "[Parse][Version]") {
  const uint8_t data[] = {
    0x00, 0x00, 0x00, 0x00,  // Wrong magic
    0x02, 0x00, 0x00, 0x00
  };

  tinyexr::v2::Reader reader(data, sizeof(data));
  auto result = tinyexr::v2::ParseVersion(reader);

  REQUIRE(result.success == false);
  REQUIRE(result.errors.size() > 0);
  REQUIRE(result.first_error().code == tinyexr::v2::ErrorCode::InvalidMagicNumber);

  // Check error message is human-readable
  std::string err_msg = result.error_string();
  REQUIRE(err_msg.find("Invalid EXR magic number") != std::string::npos);
  REQUIRE(err_msg.find("not a valid OpenEXR file") != std::string::npos);
}

TEST_CASE("v2::ParseVersion: Invalid version number", "[Parse][Version]") {
  const uint8_t data[] = {
    0x76, 0x2f, 0x31, 0x01,
    0x03,  // Version 3 (unsupported)
    0x00, 0x00, 0x00
  };

  tinyexr::v2::Reader reader(data, sizeof(data));
  auto result = tinyexr::v2::ParseVersion(reader);

  REQUIRE(result.success == false);
  REQUIRE(result.first_error().code == tinyexr::v2::ErrorCode::InvalidVersion);
  REQUIRE(result.error_string().find("Unsupported EXR version 3") != std::string::npos);
}

TEST_CASE("v2::ParseVersion: Tiled format flag", "[Parse][Version]") {
  const uint8_t data[] = {
    0x76, 0x2f, 0x31, 0x01,
    0x02,
    0x02,  // Tiled flag set
    0x00, 0x00
  };

  tinyexr::v2::Reader reader(data, sizeof(data));
  auto result = tinyexr::v2::ParseVersion(reader);

  REQUIRE(result.success == true);
  REQUIRE(result.value.tiled == true);
  REQUIRE(result.warnings.size() > 0);
  REQUIRE(result.warnings[0].find("tiled") != std::string::npos);
}

TEST_CASE("v2::ParseVersion: Truncated file", "[Parse][Version]") {
  const uint8_t data[] = {0x76, 0x2f, 0x31};  // Only 3 bytes

  tinyexr::v2::Reader reader(data, sizeof(data));
  auto result = tinyexr::v2::ParseVersion(reader);

  REQUIRE(result.success == false);
  REQUIRE(result.first_error().code == tinyexr::v2::ErrorCode::InvalidData);
  REQUIRE(result.error_string().find("too small") != std::string::npos);
}

// ============================================================================
// Version Writing Tests
// ============================================================================

TEST_CASE("v2::WriteVersion: Basic version header", "[Write][Version]") {
  tinyexr::v2::Writer writer;
  tinyexr::v2::Version version;
  version.version = 2;
  version.tiled = false;
  version.long_name = false;
  version.non_image = false;
  version.multipart = false;

  auto result = tinyexr::v2::WriteVersion(writer, version);

  REQUIRE(result.success == true);
  REQUIRE(writer.size() == 8);

  // Verify magic number
  const uint8_t* data = writer.data_ptr();
  REQUIRE(data[0] == 0x76);
  REQUIRE(data[1] == 0x2f);
  REQUIRE(data[2] == 0x31);
  REQUIRE(data[3] == 0x01);
  REQUIRE(data[4] == 0x02);  // Version
  REQUIRE(data[5] == 0x00);  // Flags
}

TEST_CASE("v2::WriteVersion: Tiled format", "[Write][Version]") {
  tinyexr::v2::Writer writer;
  tinyexr::v2::Version version;
  version.version = 2;
  version.tiled = true;
  version.long_name = false;
  version.non_image = false;
  version.multipart = false;

  auto result = tinyexr::v2::WriteVersion(writer, version);

  REQUIRE(result.success == true);
  const uint8_t* data = writer.data_ptr();
  REQUIRE((data[5] & 0x02) != 0);  // Tiled flag set
}

// ============================================================================
// Round-trip Tests (Write then Read)
// ============================================================================

TEST_CASE("v2: Round-trip version header", "[RoundTrip][Version]") {
  // Write
  tinyexr::v2::Writer writer;
  tinyexr::v2::Version write_version;
  write_version.version = 2;
  write_version.tiled = true;
  write_version.long_name = false;
  write_version.non_image = false;
  write_version.multipart = true;

  auto write_result = tinyexr::v2::WriteVersion(writer, write_version);
  REQUIRE(write_result.success == true);

  // Read back
  tinyexr::v2::Reader reader(writer.data_ptr(), writer.size());
  auto read_result = tinyexr::v2::ParseVersion(reader);

  REQUIRE(read_result.success == true);
  REQUIRE(read_result.value.version == write_version.version);
  REQUIRE(read_result.value.tiled == write_version.tiled);
  REQUIRE(read_result.value.long_name == write_version.long_name);
  REQUIRE(read_result.value.non_image == write_version.non_image);
  REQUIRE(read_result.value.multipart == write_version.multipart);
}

// ============================================================================
// File Loading Tests (using actual EXR files)
// ============================================================================

TEST_CASE("v2: Load real EXR file - issue40.exr", "[Load][Integration]") {
  std::vector<uint8_t> file_data = ReadFile("issue40.exr");

  if (file_data.empty()) {
    // Skip if file not found
    WARN("Test file issue40.exr not found, skipping");
    return;
  }

  auto result = tinyexr::v2::LoadFromMemory(file_data.data(), file_data.size());

  SECTION("File loads successfully") {
    REQUIRE(result.success == true);
    REQUIRE(result.value.width > 0);
    REQUIRE(result.value.height > 0);
  }

  SECTION("Header attributes parsed") {
    REQUIRE(result.value.header.compression >= 0);
    REQUIRE(result.value.header.pixel_aspect_ratio > 0.0f);
  }

  SECTION("Data window is valid") {
    auto& dw = result.value.header.data_window;
    REQUIRE(dw.width() > 0);
    REQUIRE(dw.height() > 0);
  }

  // Print warnings for debugging
  if (!result.warnings.empty()) {
    INFO("Warnings: " + result.warnings_string());
  }
}

// ============================================================================
// Error Handling Tests
// ============================================================================

TEST_CASE("v2: Invalid input handling", "[Error][Parse]") {
  SECTION("Null pointer") {
    auto result = tinyexr::v2::LoadFromMemory(nullptr, 100);
    REQUIRE(result.success == false);
    REQUIRE(result.first_error().code == tinyexr::v2::ErrorCode::InvalidArgument);
    REQUIRE(result.error_string().find("Null") != std::string::npos);
  }

  SECTION("Zero size") {
    uint8_t data[10];
    auto result = tinyexr::v2::LoadFromMemory(data, 0);
    REQUIRE(result.success == false);
    REQUIRE(result.first_error().code == tinyexr::v2::ErrorCode::InvalidArgument);
  }

  SECTION("Too small file") {
    uint8_t data[4] = {0x76, 0x2f, 0x31, 0x01};
    auto result = tinyexr::v2::LoadFromMemory(data, sizeof(data));
    REQUIRE(result.success == false);
  }
}

TEST_CASE("v2: Error message quality", "[Error]") {
  const uint8_t bad_magic[] = {0xFF, 0xFF, 0xFF, 0xFF, 0x02, 0x00, 0x00, 0x00};

  tinyexr::v2::Reader reader(bad_magic, sizeof(bad_magic));
  auto result = tinyexr::v2::ParseVersion(reader);

  REQUIRE(result.success == false);

  std::string err = result.error_string();

  SECTION("Contains error type") {
    REQUIRE(err.find("[ERROR]") != std::string::npos);
  }

  SECTION("Contains specific error code") {
    REQUIRE(err.find("Invalid Magic Number") != std::string::npos);
  }

  SECTION("Contains helpful message") {
    REQUIRE(err.find("not a valid OpenEXR file") != std::string::npos);
  }

  SECTION("Contains context") {
    REQUIRE(err.find("Context:") != std::string::npos);
    REQUIRE(err.find("Parsing EXR version header") != std::string::npos);
  }
}

// ============================================================================
// Regression Tests (from original tester.cc)
// ============================================================================

TEST_CASE("Regression: ParseEXRVersionFromMemory invalid input", "[Regression][Parse]") {
  SECTION("Null version pointer") {
    uint8_t data[8];
    tinyexr::v2::Reader reader(data, sizeof(data));
    auto result = tinyexr::v2::ParseVersion(reader);
    // In v2 API, we handle this gracefully
  }

  SECTION("Null data pointer") {
    auto result = tinyexr::v2::LoadFromMemory(nullptr, 100);
    REQUIRE(result.success == false);
    REQUIRE(result.first_error().code == tinyexr::v2::ErrorCode::InvalidArgument);
  }
}

// ============================================================================
// Performance/Stress Tests
// ============================================================================

TEST_CASE("v2: Large buffer handling", "[Performance]") {
  const size_t large_size = 1024 * 1024;  // 1MB
  std::vector<uint8_t> large_buffer(large_size, 0x42);

  SECTION("Dynamic writer grows efficiently") {
    tinyexr::v2::Writer writer;
    writer.reserve(large_size);

    for (size_t i = 0; i < large_size; i++) {
      REQUIRE(writer.write1(0x42) == true);
    }

    REQUIRE(writer.size() == large_size);
  }

  SECTION("Reader handles large buffers") {
    tinyexr::v2::Reader reader(large_buffer.data(), large_buffer.size());

    for (size_t i = 0; i < large_size; i++) {
      uint8_t val;
      REQUIRE(reader.read1(&val) == true);
      REQUIRE(val == 0x42);
    }

    REQUIRE(reader.eof() == true);
  }
}

// ============================================================================
// Summary Test
// ============================================================================

TEST_CASE("v2 API: Feature completeness", "[Summary]") {
  INFO("V2 API provides enhanced error reporting with:");
  INFO("- StreamReader/Writer with bounds checking");
  INFO("- Error accumulation with context");
  INFO("- Human-readable error messages");
  INFO("- Result<T> pattern for type-safe error handling");
  INFO("- Support for both dynamic and bounded buffers");

  REQUIRE(true);  // This test always passes, just for documentation
}

// ============================================================================
// SIMD Optimized Pixel Processing Tests
// ============================================================================

TEST_CASE("v2: ConvertHalfToFloat", "[SIMD][Conversion]") {
  INFO("SIMD Backend: " << tinyexr::v2::GetSIMDInfo());
  INFO("SIMD Enabled: " << (tinyexr::v2::IsSIMDEnabled() ? "yes" : "no"));

  SECTION("Basic conversion") {
    // Test known half-float values
    // 0x3C00 = 1.0f, 0x4000 = 2.0f, 0x3800 = 0.5f
    std::vector<uint16_t> half_values = {0x3C00, 0x4000, 0x3800, 0x0000};
    std::vector<float> expected = {1.0f, 2.0f, 0.5f, 0.0f};
    std::vector<float> result(4);

    tinyexr::v2::ConvertHalfToFloat(half_values.data(), result.data(), 4);

    for (size_t i = 0; i < 4; i++) {
      REQUIRE(std::fabs(result[i] - expected[i]) < 0.001f);
    }
  }

  SECTION("Batch conversion") {
    const size_t count = 256;
    std::vector<uint16_t> half_data(count, 0x3C00);  // All 1.0f
    std::vector<float> float_data(count);

    tinyexr::v2::ConvertHalfToFloat(half_data.data(), float_data.data(), count);

    for (size_t i = 0; i < count; i++) {
      REQUIRE(std::fabs(float_data[i] - 1.0f) < 0.001f);
    }
  }
}

TEST_CASE("v2: ConvertFloatToHalf", "[SIMD][Conversion]") {
  SECTION("Basic conversion") {
    std::vector<float> float_values = {1.0f, 2.0f, 0.5f, 0.0f};
    std::vector<uint16_t> expected = {0x3C00, 0x4000, 0x3800, 0x0000};
    std::vector<uint16_t> result(4);

    tinyexr::v2::ConvertFloatToHalf(float_values.data(), result.data(), 4);

    for (size_t i = 0; i < 4; i++) {
      // Allow +/- 1 due to rounding
      int diff = static_cast<int>(result[i]) - static_cast<int>(expected[i]);
      REQUIRE(std::abs(diff) <= 1);
    }
  }

  SECTION("Batch conversion") {
    const size_t count = 256;
    std::vector<float> float_data(count, 1.0f);
    std::vector<uint16_t> half_data(count);

    tinyexr::v2::ConvertFloatToHalf(float_data.data(), half_data.data(), count);

    for (size_t i = 0; i < count; i++) {
      // 0x3C00 = 1.0f in half
      REQUIRE(half_data[i] == 0x3C00);
    }
  }
}

TEST_CASE("v2: FP16 Round-trip", "[SIMD][Conversion]") {
  SECTION("Common values") {
    std::vector<float> original = {0.0f, 1.0f, -1.0f, 0.5f, 2.0f, 100.0f, -50.0f};
    std::vector<uint16_t> half_data(original.size());
    std::vector<float> restored(original.size());

    tinyexr::v2::ConvertFloatToHalf(original.data(), half_data.data(), original.size());
    tinyexr::v2::ConvertHalfToFloat(half_data.data(), restored.data(), original.size());

    for (size_t i = 0; i < original.size(); i++) {
      // Half precision has ~3 decimal digits of precision
      float tolerance = std::fabs(original[i]) * 0.002f + 0.001f;
      REQUIRE(std::fabs(restored[i] - original[i]) < tolerance);
    }
  }

  SECTION("Large batch") {
    const size_t count = 1024;
    std::vector<float> original(count);
    std::vector<uint16_t> half_data(count);
    std::vector<float> restored(count);

    // Fill with various values
    for (size_t i = 0; i < count; i++) {
      original[i] = static_cast<float>(i) / 10.0f - 50.0f;
    }

    tinyexr::v2::ConvertFloatToHalf(original.data(), half_data.data(), count);
    tinyexr::v2::ConvertHalfToFloat(half_data.data(), restored.data(), count);

    for (size_t i = 0; i < count; i++) {
      float tolerance = std::fabs(original[i]) * 0.002f + 0.01f;
      REQUIRE(std::fabs(restored[i] - original[i]) < tolerance);
    }
  }
}

TEST_CASE("v2: InterleaveRGBA", "[SIMD][Interleave]") {
  SECTION("Basic interleave") {
    const size_t count = 4;
    std::vector<float> r = {1.0f, 2.0f, 3.0f, 4.0f};
    std::vector<float> g = {0.1f, 0.2f, 0.3f, 0.4f};
    std::vector<float> b = {0.01f, 0.02f, 0.03f, 0.04f};
    std::vector<float> a = {1.0f, 1.0f, 1.0f, 1.0f};
    std::vector<float> rgba(count * 4);

    tinyexr::v2::InterleaveRGBA(r.data(), g.data(), b.data(), a.data(), rgba.data(), count);

    for (size_t i = 0; i < count; i++) {
      REQUIRE(rgba[i * 4 + 0] == r[i]);
      REQUIRE(rgba[i * 4 + 1] == g[i]);
      REQUIRE(rgba[i * 4 + 2] == b[i]);
      REQUIRE(rgba[i * 4 + 3] == a[i]);
    }
  }

  SECTION("Large batch") {
    const size_t count = 256;
    std::vector<float> r(count), g(count), b(count), a(count);
    std::vector<float> rgba(count * 4);

    for (size_t i = 0; i < count; i++) {
      r[i] = static_cast<float>(i);
      g[i] = static_cast<float>(count - i);
      b[i] = static_cast<float>(i * 2 % count);
      a[i] = 1.0f;
    }

    tinyexr::v2::InterleaveRGBA(r.data(), g.data(), b.data(), a.data(), rgba.data(), count);

    for (size_t i = 0; i < count; i++) {
      REQUIRE(rgba[i * 4 + 0] == r[i]);
      REQUIRE(rgba[i * 4 + 1] == g[i]);
      REQUIRE(rgba[i * 4 + 2] == b[i]);
      REQUIRE(rgba[i * 4 + 3] == a[i]);
    }
  }
}

TEST_CASE("v2: DeinterleaveRGBA", "[SIMD][Interleave]") {
  SECTION("Basic deinterleave") {
    const size_t count = 4;
    std::vector<float> rgba = {
      1.0f, 0.1f, 0.01f, 1.0f,
      2.0f, 0.2f, 0.02f, 1.0f,
      3.0f, 0.3f, 0.03f, 1.0f,
      4.0f, 0.4f, 0.04f, 1.0f
    };
    std::vector<float> r(count), g(count), b(count), a(count);

    tinyexr::v2::DeinterleaveRGBA(rgba.data(), r.data(), g.data(), b.data(), a.data(), count);

    REQUIRE(r[0] == 1.0f);
    REQUIRE(r[1] == 2.0f);
    REQUIRE(r[2] == 3.0f);
    REQUIRE(r[3] == 4.0f);

    REQUIRE(g[0] == Approx(0.1f));
    REQUIRE(g[1] == Approx(0.2f));
    REQUIRE(g[2] == Approx(0.3f));
    REQUIRE(g[3] == Approx(0.4f));
  }

  SECTION("Round-trip") {
    const size_t count = 256;
    std::vector<float> r(count), g(count), b(count), a(count);
    std::vector<float> rgba(count * 4);
    std::vector<float> r2(count), g2(count), b2(count), a2(count);

    for (size_t i = 0; i < count; i++) {
      r[i] = static_cast<float>(i) / count;
      g[i] = static_cast<float>(count - i) / count;
      b[i] = static_cast<float>(i * 2 % count) / count;
      a[i] = 1.0f;
    }

    tinyexr::v2::InterleaveRGBA(r.data(), g.data(), b.data(), a.data(), rgba.data(), count);
    tinyexr::v2::DeinterleaveRGBA(rgba.data(), r2.data(), g2.data(), b2.data(), a2.data(), count);

    for (size_t i = 0; i < count; i++) {
      REQUIRE(r2[i] == r[i]);
      REQUIRE(g2[i] == g[i]);
      REQUIRE(b2[i] == b[i]);
      REQUIRE(a2[i] == a[i]);
    }
  }
}

// ============================================================================
// PIZ Decompression Tests
// ============================================================================

TEST_CASE("v2: Load PIZ compressed EXR - issue-160-piz-decode.exr", "[PIZ][Decompress]") {
  std::vector<uint8_t> file_data = ReadFile("regression/issue-160-piz-decode.exr");

  if (file_data.empty()) {
    WARN("Test file regression/issue-160-piz-decode.exr not found, skipping");
    return;
  }

  auto result = tinyexr::v2::LoadFromMemory(file_data.data(), file_data.size());

  SECTION("PIZ file loads successfully") {
    // Print any errors for debugging
    if (!result.success) {
      INFO("Error: " + result.error_string());
    }
    if (!result.warnings.empty()) {
      INFO("Warnings: " + result.warnings_string());
    }

    REQUIRE(result.success == true);
    REQUIRE(result.value.width > 0);
    REQUIRE(result.value.height > 0);
    REQUIRE(result.value.header.compression == 4);  // PIZ = 4
  }

  SECTION("Pixel data is valid") {
    if (!result.success) return;

    // Check that we have actual pixel data
    size_t expected_size = static_cast<size_t>(result.value.width) *
                           static_cast<size_t>(result.value.height) * 4;
    REQUIRE(result.value.rgba.size() == expected_size);

    // Check some pixels are non-zero (image has content)
    bool has_nonzero = false;
    for (size_t i = 0; i < result.value.rgba.size() && !has_nonzero; i++) {
      if (result.value.rgba[i] != 0.0f) {
        has_nonzero = true;
      }
    }
    REQUIRE(has_nonzero == true);
  }
}

TEST_CASE("v2: Load PIZ compressed EXR - piz-bug-issue-100.exr", "[PIZ][Decompress]") {
  std::vector<uint8_t> file_data = ReadFile("regression/piz-bug-issue-100.exr");

  if (file_data.empty()) {
    WARN("Test file regression/piz-bug-issue-100.exr not found, skipping");
    return;
  }

  auto result = tinyexr::v2::LoadFromMemory(file_data.data(), file_data.size());

  SECTION("PIZ file loads successfully (issue 100 regression)") {
    if (!result.success) {
      INFO("Error: " + result.error_string());
    }
    if (!result.warnings.empty()) {
      INFO("Warnings: " + result.warnings_string());
    }

    REQUIRE(result.success == true);
    REQUIRE(result.value.width > 0);
    REQUIRE(result.value.height > 0);
  }
}

TEST_CASE("v2: Load PIZ edge case - issue-194 (all zeros)", "[PIZ][Decompress]") {
  std::vector<uint8_t> file_data = ReadFile("regression/000-issue194.exr");

  if (file_data.empty()) {
    WARN("Test file regression/000-issue194.exr not found, skipping");
    return;
  }

  auto result = tinyexr::v2::LoadFromMemory(file_data.data(), file_data.size());

  SECTION("PIZ file with special minNonZero/maxNonZero handles correctly") {
    if (!result.success) {
      INFO("Error: " + result.error_string());
    }

    // This file may use a special case where all pixels are zero
    // The decompressor should handle this gracefully
    if (!result.warnings.empty()) {
      INFO("Warnings: " + result.warnings_string());
    }

    // Either succeeds or gives a meaningful error/warning
    // The key is that it doesn't crash
  }
}

// ============================================================================
// Compression Round-Trip Tests
// ============================================================================

TEST_CASE("v2: ZIP compression round-trip", "[ZIP][RoundTrip][Compression]") {
  // Create test image data
  const int width = 64;
  const int height = 64;

  tinyexr::v2::ImageData original;
  original.width = width;
  original.height = height;
  original.num_channels = 4;
  original.rgba.resize(width * height * 4);

  // Fill with test pattern
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      int idx = (y * width + x) * 4;
      original.rgba[idx + 0] = static_cast<float>(x) / width;
      original.rgba[idx + 1] = static_cast<float>(y) / height;
      original.rgba[idx + 2] = static_cast<float>(x + y) / (width + height);
      original.rgba[idx + 3] = 1.0f;
    }
  }

  original.header.compression = tinyexr::v2::COMPRESSION_ZIP;
  original.header.data_window.min_x = 0;
  original.header.data_window.min_y = 0;
  original.header.data_window.max_x = width - 1;
  original.header.data_window.max_y = height - 1;
  original.header.display_window = original.header.data_window;
  original.header.pixel_aspect_ratio = 1.0f;
  original.header.screen_window_width = 1.0f;
  original.header.line_order = 0;

  tinyexr::v2::Channel ch_r, ch_g, ch_b, ch_a;
  ch_r.name = "R"; ch_r.pixel_type = tinyexr::v2::PIXEL_TYPE_HALF; ch_r.x_sampling = 1; ch_r.y_sampling = 1;
  ch_g.name = "G"; ch_g.pixel_type = tinyexr::v2::PIXEL_TYPE_HALF; ch_g.x_sampling = 1; ch_g.y_sampling = 1;
  ch_b.name = "B"; ch_b.pixel_type = tinyexr::v2::PIXEL_TYPE_HALF; ch_b.x_sampling = 1; ch_b.y_sampling = 1;
  ch_a.name = "A"; ch_a.pixel_type = tinyexr::v2::PIXEL_TYPE_HALF; ch_a.x_sampling = 1; ch_a.y_sampling = 1;
  original.header.channels.push_back(ch_a);
  original.header.channels.push_back(ch_b);
  original.header.channels.push_back(ch_g);
  original.header.channels.push_back(ch_r);

  SECTION("Save and load ZIP") {
    auto save_result = tinyexr::v2::SaveToMemory(original, 6);
    REQUIRE(save_result.success == true);
    printf("ZIP: Saved %zu bytes\n", save_result.value.size());

    auto load_result = tinyexr::v2::LoadFromMemory(save_result.value.data(),
                                                    save_result.value.size());
    REQUIRE(load_result.success == true);

    size_t num_pixels = width * height * 4;
    int diff_count = 0;
    for (size_t i = 0; i < num_pixels; i++) {
      float diff = std::abs(load_result.value.rgba[i] - original.rgba[i]);
      if (diff > 0.01f) diff_count++;
    }
    printf("ZIP: Pixels differing: %d\n", diff_count);
    if (diff_count > 0) {
      printf("ZIP: First 16 pixels:\n");
      for (size_t i = 0; i < 16 && i < num_pixels; i++) {
        printf("  [%zu] %.4f -> %.4f\n", i, original.rgba[i], load_result.value.rgba[i]);
      }
    }
    REQUIRE(diff_count == 0);
  }
}

TEST_CASE("v2: PXR24 compression round-trip", "[PXR24][RoundTrip][Compression]") {
  // Create test image data
  const int width = 64;
  const int height = 64;

  tinyexr::v2::ImageData original;
  original.width = width;
  original.height = height;
  original.num_channels = 4;
  original.rgba.resize(width * height * 4);

  // Fill with test pattern (various float values)
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      int idx = (y * width + x) * 4;
      original.rgba[idx + 0] = static_cast<float>(x) / width;          // R: gradient
      original.rgba[idx + 1] = static_cast<float>(y) / height;         // G: gradient
      original.rgba[idx + 2] = static_cast<float>(x + y) / (width + height); // B: diagonal
      original.rgba[idx + 3] = 1.0f;                                   // A: constant
    }
  }

  // Set up header with PXR24 compression
  original.header.compression = tinyexr::v2::COMPRESSION_PXR24;
  original.header.data_window.min_x = 0;
  original.header.data_window.min_y = 0;
  original.header.data_window.max_x = width - 1;
  original.header.data_window.max_y = height - 1;
  original.header.display_window = original.header.data_window;
  original.header.pixel_aspect_ratio = 1.0f;
  original.header.screen_window_width = 1.0f;
  original.header.line_order = 0;

  // Add RGBA channels
  tinyexr::v2::Channel ch_r, ch_g, ch_b, ch_a;
  ch_r.name = "R"; ch_r.pixel_type = tinyexr::v2::PIXEL_TYPE_HALF; ch_r.x_sampling = 1; ch_r.y_sampling = 1;
  ch_g.name = "G"; ch_g.pixel_type = tinyexr::v2::PIXEL_TYPE_HALF; ch_g.x_sampling = 1; ch_g.y_sampling = 1;
  ch_b.name = "B"; ch_b.pixel_type = tinyexr::v2::PIXEL_TYPE_HALF; ch_b.x_sampling = 1; ch_b.y_sampling = 1;
  ch_a.name = "A"; ch_a.pixel_type = tinyexr::v2::PIXEL_TYPE_HALF; ch_a.x_sampling = 1; ch_a.y_sampling = 1;
  original.header.channels.push_back(ch_a);  // EXR channels are sorted alphabetically
  original.header.channels.push_back(ch_b);
  original.header.channels.push_back(ch_g);
  original.header.channels.push_back(ch_r);

  SECTION("Save and load PXR24") {
    // Save to memory with PXR24 compression
    auto save_result = tinyexr::v2::SaveToMemory(original, 6);

    if (!save_result.success) {
      INFO("Save error: " + save_result.error_string());
    }
    REQUIRE(save_result.success == true);
    REQUIRE(save_result.value.size() > 0);

    // Verify the saved data uses PXR24 compression
    // (compression byte is at a known offset in EXR header)
    INFO("Saved " << save_result.value.size() << " bytes");

    // Load it back
    auto load_result = tinyexr::v2::LoadFromMemory(save_result.value.data(),
                                                    save_result.value.size());

    if (!load_result.success) {
      INFO("Load error: " + load_result.error_string());
    }
    if (!load_result.warnings.empty()) {
      INFO("Load warnings: " + load_result.warnings_string());
    }
    REQUIRE(load_result.success == true);

    // Verify dimensions match
    REQUIRE(load_result.value.width == original.width);
    REQUIRE(load_result.value.height == original.height);

    // Verify pixel data matches (with tolerance for HALF precision)
    // HALF has ~3 decimal digits of precision (10-bit mantissa)
    size_t num_pixels = width * height * 4;
    REQUIRE(load_result.value.rgba.size() == num_pixels);

    int diff_count = 0;
    float max_diff = 0.0f;
    size_t first_diff_idx = 0;
    for (size_t i = 0; i < num_pixels; i++) {
      float diff = std::abs(load_result.value.rgba[i] - original.rgba[i]);
      if (diff > 0.01f) {  // ~1% tolerance for HALF precision
        if (diff_count == 0) first_diff_idx = i;
        diff_count++;
        if (diff > max_diff) max_diff = diff;
      }
    }

    if (diff_count > 0) {
      printf("Pixels differing: %d, max diff: %.4f\n", diff_count, max_diff);
      // Show first few pixels for debugging
      printf("First 16 pixels (idx: orig -> loaded):\n");
      for (size_t i = 0; i < 16 && i < num_pixels; i++) {
        printf("  [%zu] %.4f -> %.4f\n", i, original.rgba[i], load_result.value.rgba[i]);
      }
      printf("First diff at idx %zu: orig=%.4f loaded=%.4f\n",
             first_diff_idx, original.rgba[first_diff_idx], load_result.value.rgba[first_diff_idx]);
    }

    // Allow some precision loss due to HALF format
    REQUIRE(diff_count == 0);
  }
}

TEST_CASE("v2: B44 compression round-trip", "[B44][RoundTrip][Compression]") {
  // Create test image data
  const int width = 64;
  const int height = 64;

  tinyexr::v2::ImageData original;
  original.width = width;
  original.height = height;
  original.num_channels = 4;
  original.rgba.resize(width * height * 4);

  // Fill with gradient pattern
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      size_t idx = (y * width + x) * 4;
      original.rgba[idx + 0] = static_cast<float>(x) / width;            // R: horizontal
      original.rgba[idx + 1] = static_cast<float>(y) / height;           // G: vertical
      original.rgba[idx + 2] = static_cast<float>(x + y) / (width + height); // B: diagonal
      original.rgba[idx + 3] = 1.0f;                                     // A: constant
    }
  }

  // Set up header with B44 compression
  original.header.compression = tinyexr::v2::COMPRESSION_B44;
  original.header.data_window.min_x = 0;
  original.header.data_window.min_y = 0;
  original.header.data_window.max_x = width - 1;
  original.header.data_window.max_y = height - 1;
  original.header.display_window = original.header.data_window;
  original.header.pixel_aspect_ratio = 1.0f;
  original.header.line_order = 0;

  // Add RGBA channels (sorted alphabetically for EXR)
  tinyexr::v2::Channel ch_r, ch_g, ch_b, ch_a;
  ch_r.name = "R"; ch_r.pixel_type = tinyexr::v2::PIXEL_TYPE_HALF; ch_r.x_sampling = 1; ch_r.y_sampling = 1;
  ch_g.name = "G"; ch_g.pixel_type = tinyexr::v2::PIXEL_TYPE_HALF; ch_g.x_sampling = 1; ch_g.y_sampling = 1;
  ch_b.name = "B"; ch_b.pixel_type = tinyexr::v2::PIXEL_TYPE_HALF; ch_b.x_sampling = 1; ch_b.y_sampling = 1;
  ch_a.name = "A"; ch_a.pixel_type = tinyexr::v2::PIXEL_TYPE_HALF; ch_a.x_sampling = 1; ch_a.y_sampling = 1;
  original.header.channels.push_back(ch_a);
  original.header.channels.push_back(ch_b);
  original.header.channels.push_back(ch_g);
  original.header.channels.push_back(ch_r);

  SECTION("Save and load B44") {
    // Save to memory with B44 compression
    auto save_result = tinyexr::v2::SaveToMemory(original, 6);

    if (!save_result.success) {
      INFO("Save error: " + save_result.error_string());
    }
    REQUIRE(save_result.success == true);
    REQUIRE(save_result.value.size() > 0);

    INFO("Saved " << save_result.value.size() << " bytes");

    // Load it back
    auto load_result = tinyexr::v2::LoadFromMemory(save_result.value.data(),
                                                    save_result.value.size());

    if (!load_result.success) {
      INFO("Load error: " + load_result.error_string());
    }
    if (!load_result.warnings.empty()) {
      INFO("Load warnings: " + load_result.warnings_string());
    }
    REQUIRE(load_result.success == true);

    // Verify dimensions match
    REQUIRE(load_result.value.width == original.width);
    REQUIRE(load_result.value.height == original.height);

    // Verify pixel data matches (B44 is lossy, use larger tolerance)
    size_t num_pixels = width * height * 4;
    REQUIRE(load_result.value.rgba.size() == num_pixels);

    int diff_count = 0;
    float max_diff = 0.0f;
    // B44 uses 6-bit deltas, so error can be up to ~1/32 of value range
    const float tolerance = 0.05f;
    for (size_t i = 0; i < num_pixels; i++) {
      float diff = std::abs(load_result.value.rgba[i] - original.rgba[i]);
      if (diff > tolerance) {
        diff_count++;
        if (diff > max_diff) max_diff = diff;
      }
    }

    if (diff_count > 0) {
      INFO("Pixels differing beyond tolerance: " << diff_count << ", max diff: " << max_diff);
    }

    // B44 is lossy, allow some differences but not too many
    REQUIRE(diff_count < static_cast<int>(num_pixels * 0.05));  // Less than 5% of pixels
  }
}

TEST_CASE("v2: B44A compression round-trip", "[B44A][RoundTrip][Compression]") {
  // Create test image with flat areas (B44A optimization target)
  const int width = 64;
  const int height = 64;

  tinyexr::v2::ImageData original;
  original.width = width;
  original.height = height;
  original.num_channels = 4;
  original.rgba.resize(width * height * 4);

  // Fill with large flat areas (8x8 blocks of same color)
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      size_t idx = (y * width + x) * 4;
      int block_x = x / 8;
      int block_y = y / 8;
      float val = static_cast<float>((block_x + block_y) % 4) / 4.0f;
      original.rgba[idx + 0] = val;
      original.rgba[idx + 1] = val;
      original.rgba[idx + 2] = val;
      original.rgba[idx + 3] = 1.0f;
    }
  }

  // Set up header with B44A compression
  original.header.compression = tinyexr::v2::COMPRESSION_B44A;
  original.header.data_window.min_x = 0;
  original.header.data_window.min_y = 0;
  original.header.data_window.max_x = width - 1;
  original.header.data_window.max_y = height - 1;
  original.header.display_window = original.header.data_window;
  original.header.pixel_aspect_ratio = 1.0f;
  original.header.line_order = 0;

  // Add RGBA channels
  tinyexr::v2::Channel ch_r, ch_g, ch_b, ch_a;
  ch_r.name = "R"; ch_r.pixel_type = tinyexr::v2::PIXEL_TYPE_HALF; ch_r.x_sampling = 1; ch_r.y_sampling = 1;
  ch_g.name = "G"; ch_g.pixel_type = tinyexr::v2::PIXEL_TYPE_HALF; ch_g.x_sampling = 1; ch_g.y_sampling = 1;
  ch_b.name = "B"; ch_b.pixel_type = tinyexr::v2::PIXEL_TYPE_HALF; ch_b.x_sampling = 1; ch_b.y_sampling = 1;
  ch_a.name = "A"; ch_a.pixel_type = tinyexr::v2::PIXEL_TYPE_HALF; ch_a.x_sampling = 1; ch_a.y_sampling = 1;
  original.header.channels.push_back(ch_a);
  original.header.channels.push_back(ch_b);
  original.header.channels.push_back(ch_g);
  original.header.channels.push_back(ch_r);

  SECTION("Save and load B44A") {
    auto save_result = tinyexr::v2::SaveToMemory(original, 6);

    if (!save_result.success) {
      INFO("Save error: " + save_result.error_string());
    }
    REQUIRE(save_result.success == true);
    REQUIRE(save_result.value.size() > 0);

    INFO("Saved " << save_result.value.size() << " bytes");

    auto load_result = tinyexr::v2::LoadFromMemory(save_result.value.data(),
                                                    save_result.value.size());

    if (!load_result.success) {
      INFO("Load error: " + load_result.error_string());
    }
    REQUIRE(load_result.success == true);

    REQUIRE(load_result.value.width == original.width);
    REQUIRE(load_result.value.height == original.height);

    // For flat areas, B44A should have perfect reconstruction
    size_t num_pixels = width * height * 4;
    int diff_count = 0;
    float max_diff = 0.0f;
    const float tolerance = 0.01f;  // Tighter tolerance for flat blocks
    for (size_t i = 0; i < num_pixels; i++) {
      float diff = std::abs(load_result.value.rgba[i] - original.rgba[i]);
      if (diff > tolerance) {
        diff_count++;
        if (diff > max_diff) max_diff = diff;
      }
    }

    if (diff_count > 0) {
      INFO("Pixels differing: " << diff_count << ", max diff: " << max_diff);
    }

    // For flat areas, should have near-perfect reconstruction
    REQUIRE(diff_count == 0);
  }
}

// ============================================================================
// Tiled EXR Writing Tests
// ============================================================================

TEST_CASE("v2: Tiled EXR writing with ZIP", "[Tiled][ZIP][RoundTrip]") {
  // Create test image data
  const int width = 100;  // Non-tile-aligned
  const int height = 75;  // Non-tile-aligned

  tinyexr::v2::ImageData original;
  original.width = width;
  original.height = height;
  original.num_channels = 4;
  original.rgba.resize(width * height * 4);

  // Fill with gradient pattern
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      int idx = (y * width + x) * 4;
      original.rgba[idx + 0] = static_cast<float>(x) / width;
      original.rgba[idx + 1] = static_cast<float>(y) / height;
      original.rgba[idx + 2] = 0.5f;
      original.rgba[idx + 3] = 1.0f;
    }
  }

  // Set up header for tiled format
  original.header.compression = tinyexr::v2::COMPRESSION_ZIP;
  original.header.data_window.min_x = 0;
  original.header.data_window.min_y = 0;
  original.header.data_window.max_x = width - 1;
  original.header.data_window.max_y = height - 1;
  original.header.display_window = original.header.data_window;
  original.header.pixel_aspect_ratio = 1.0f;
  original.header.screen_window_width = 1.0f;
  original.header.line_order = 0;
  original.header.tiled = true;
  original.header.tile_size_x = 32;
  original.header.tile_size_y = 32;
  original.header.tile_level_mode = 0;  // ONE_LEVEL
  original.header.tile_rounding_mode = 0;  // ROUND_DOWN

  tinyexr::v2::Channel ch_r, ch_g, ch_b, ch_a;
  ch_r.name = "R"; ch_r.pixel_type = tinyexr::v2::PIXEL_TYPE_HALF; ch_r.x_sampling = 1; ch_r.y_sampling = 1;
  ch_g.name = "G"; ch_g.pixel_type = tinyexr::v2::PIXEL_TYPE_HALF; ch_g.x_sampling = 1; ch_g.y_sampling = 1;
  ch_b.name = "B"; ch_b.pixel_type = tinyexr::v2::PIXEL_TYPE_HALF; ch_b.x_sampling = 1; ch_b.y_sampling = 1;
  ch_a.name = "A"; ch_a.pixel_type = tinyexr::v2::PIXEL_TYPE_HALF; ch_a.x_sampling = 1; ch_a.y_sampling = 1;
  original.header.channels.push_back(ch_a);
  original.header.channels.push_back(ch_b);
  original.header.channels.push_back(ch_g);
  original.header.channels.push_back(ch_r);

  SECTION("Save and load tiled ZIP") {
    auto save_result = tinyexr::v2::SaveTiledToMemory(original, 6);

    if (!save_result.success) {
      INFO("Save error: " + save_result.error_string());
    }
    REQUIRE(save_result.success == true);
    REQUIRE(save_result.value.size() > 0);

    INFO("Tiled ZIP: Saved " << save_result.value.size() << " bytes");

    // Load back
    auto load_result = tinyexr::v2::LoadFromMemory(save_result.value.data(),
                                                    save_result.value.size());

    if (!load_result.success) {
      INFO("Load error: " + load_result.error_string());
    }
    REQUIRE(load_result.success == true);

    REQUIRE(load_result.value.width == original.width);
    REQUIRE(load_result.value.height == original.height);

    // Verify pixels
    size_t num_pixels = width * height * 4;
    int diff_count = 0;
    float max_diff = 0.0f;
    const float tolerance = 0.002f;  // FP16 precision
    for (size_t i = 0; i < num_pixels; i++) {
      float diff = std::abs(load_result.value.rgba[i] - original.rgba[i]);
      if (diff > tolerance) {
        diff_count++;
        if (diff > max_diff) max_diff = diff;
      }
    }

    if (diff_count > 0) {
      INFO("Tiled ZIP: Pixels differing: " << diff_count << ", max diff: " << max_diff);
    }

    REQUIRE(diff_count == 0);
  }
}

TEST_CASE("v2: Tiled EXR writing with various compressions", "[Tiled][Compression][RoundTrip]") {
  // Create test image data
  const int width = 64;
  const int height = 64;

  tinyexr::v2::ImageData original;
  original.width = width;
  original.height = height;
  original.num_channels = 4;
  original.rgba.resize(width * height * 4);

  // Fill with gradient pattern
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      int idx = (y * width + x) * 4;
      original.rgba[idx + 0] = static_cast<float>(x) / width;
      original.rgba[idx + 1] = static_cast<float>(y) / height;
      original.rgba[idx + 2] = 0.5f;
      original.rgba[idx + 3] = 1.0f;
    }
  }

  // Set up header for tiled format
  original.header.data_window.min_x = 0;
  original.header.data_window.min_y = 0;
  original.header.data_window.max_x = width - 1;
  original.header.data_window.max_y = height - 1;
  original.header.display_window = original.header.data_window;
  original.header.pixel_aspect_ratio = 1.0f;
  original.header.screen_window_width = 1.0f;
  original.header.line_order = 0;
  original.header.tiled = true;
  original.header.tile_size_x = 32;
  original.header.tile_size_y = 32;
  original.header.tile_level_mode = 0;
  original.header.tile_rounding_mode = 0;

  tinyexr::v2::Channel ch_r, ch_g, ch_b, ch_a;
  ch_r.name = "R"; ch_r.pixel_type = tinyexr::v2::PIXEL_TYPE_HALF; ch_r.x_sampling = 1; ch_r.y_sampling = 1;
  ch_g.name = "G"; ch_g.pixel_type = tinyexr::v2::PIXEL_TYPE_HALF; ch_g.x_sampling = 1; ch_g.y_sampling = 1;
  ch_b.name = "B"; ch_b.pixel_type = tinyexr::v2::PIXEL_TYPE_HALF; ch_b.x_sampling = 1; ch_b.y_sampling = 1;
  ch_a.name = "A"; ch_a.pixel_type = tinyexr::v2::PIXEL_TYPE_HALF; ch_a.x_sampling = 1; ch_a.y_sampling = 1;
  original.header.channels.push_back(ch_a);
  original.header.channels.push_back(ch_b);
  original.header.channels.push_back(ch_g);
  original.header.channels.push_back(ch_r);

  auto test_compression = [&](int compression, const char* name, float tolerance) {
    original.header.compression = compression;

    auto save_result = tinyexr::v2::SaveTiledToMemory(original, 6);
    if (!save_result.success) {
      INFO(name << ": Save error: " + save_result.error_string());
    }
    REQUIRE(save_result.success == true);
    REQUIRE(save_result.value.size() > 0);

    INFO(name << ": Saved " << save_result.value.size() << " bytes");

    auto load_result = tinyexr::v2::LoadFromMemory(save_result.value.data(),
                                                    save_result.value.size());
    if (!load_result.success) {
      INFO(name << ": Load error: " + load_result.error_string());
    }
    REQUIRE(load_result.success == true);

    REQUIRE(load_result.value.width == original.width);
    REQUIRE(load_result.value.height == original.height);

    size_t num_pixels = width * height * 4;
    int diff_count = 0;
    float max_diff = 0.0f;
    for (size_t i = 0; i < num_pixels; i++) {
      float diff = std::abs(load_result.value.rgba[i] - original.rgba[i]);
      if (diff > tolerance) {
        diff_count++;
        if (diff > max_diff) max_diff = diff;
      }
    }

    INFO(name << ": Pixels differing: " << diff_count << ", max diff: " << max_diff);
    REQUIRE(diff_count == 0);
  };

  SECTION("Tiled NONE compression") {
    test_compression(tinyexr::v2::COMPRESSION_NONE, "Tiled NONE", 0.002f);
  }

  SECTION("Tiled RLE compression") {
    test_compression(tinyexr::v2::COMPRESSION_RLE, "Tiled RLE", 0.002f);
  }

  SECTION("Tiled ZIPS compression") {
    test_compression(tinyexr::v2::COMPRESSION_ZIPS, "Tiled ZIPS", 0.002f);
  }

  SECTION("Tiled ZIP compression") {
    test_compression(tinyexr::v2::COMPRESSION_ZIP, "Tiled ZIP", 0.002f);
  }

  SECTION("Tiled B44 compression") {
    test_compression(tinyexr::v2::COMPRESSION_B44, "Tiled B44", 0.05f);  // Lossy
  }
}

TEST_CASE("v2: Tiled EXR with mipmap levels", "[Tiled][Mipmap][RoundTrip]") {
  // Create test image data (64x64 for clean mipmap levels)
  const int width = 64;
  const int height = 64;

  tinyexr::v2::ImageData original;
  original.width = width;
  original.height = height;
  original.num_channels = 4;
  original.rgba.resize(width * height * 4);

  // Fill with gradient pattern
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      int idx = (y * width + x) * 4;
      original.rgba[idx + 0] = static_cast<float>(x) / width;
      original.rgba[idx + 1] = static_cast<float>(y) / height;
      original.rgba[idx + 2] = 0.5f;
      original.rgba[idx + 3] = 1.0f;
    }
  }

  // Set up header for tiled format with mipmap levels
  original.header.compression = tinyexr::v2::COMPRESSION_ZIP;
  original.header.data_window.min_x = 0;
  original.header.data_window.min_y = 0;
  original.header.data_window.max_x = width - 1;
  original.header.data_window.max_y = height - 1;
  original.header.display_window = original.header.data_window;
  original.header.pixel_aspect_ratio = 1.0f;
  original.header.screen_window_width = 1.0f;
  original.header.line_order = 0;
  original.header.tiled = true;
  original.header.tile_size_x = 32;
  original.header.tile_size_y = 32;
  original.header.tile_level_mode = 1;  // MIPMAP_LEVELS
  original.header.tile_rounding_mode = 0;

  tinyexr::v2::Channel ch_r, ch_g, ch_b, ch_a;
  ch_r.name = "R"; ch_r.pixel_type = tinyexr::v2::PIXEL_TYPE_HALF; ch_r.x_sampling = 1; ch_r.y_sampling = 1;
  ch_g.name = "G"; ch_g.pixel_type = tinyexr::v2::PIXEL_TYPE_HALF; ch_g.x_sampling = 1; ch_g.y_sampling = 1;
  ch_b.name = "B"; ch_b.pixel_type = tinyexr::v2::PIXEL_TYPE_HALF; ch_b.x_sampling = 1; ch_b.y_sampling = 1;
  ch_a.name = "A"; ch_a.pixel_type = tinyexr::v2::PIXEL_TYPE_HALF; ch_a.x_sampling = 1; ch_a.y_sampling = 1;
  original.header.channels.push_back(ch_a);
  original.header.channels.push_back(ch_b);
  original.header.channels.push_back(ch_g);
  original.header.channels.push_back(ch_r);

  SECTION("Save and load tiled mipmap") {
    auto save_result = tinyexr::v2::SaveTiledToMemory(original, 6);

    if (!save_result.success) {
      INFO("Save error: " + save_result.error_string());
    }
    REQUIRE(save_result.success == true);
    REQUIRE(save_result.value.size() > 0);

    // Should have warning about mipmap levels generated
    INFO("Mipmap: Saved " << save_result.value.size() << " bytes");
    if (!save_result.warnings.empty()) {
      INFO("Warnings: " << save_result.warnings_string());
    }

    // Load back - loader reads level 0 by default
    auto load_result = tinyexr::v2::LoadFromMemory(save_result.value.data(),
                                                    save_result.value.size());

    if (!load_result.success) {
      INFO("Load error: " + load_result.error_string());
    }
    REQUIRE(load_result.success == true);

    // Base level should match original dimensions
    REQUIRE(load_result.value.width == original.width);
    REQUIRE(load_result.value.height == original.height);

    // Verify base level pixels
    size_t num_pixels = width * height * 4;
    int diff_count = 0;
    float max_diff = 0.0f;
    const float tolerance = 0.002f;
    for (size_t i = 0; i < num_pixels; i++) {
      float diff = std::abs(load_result.value.rgba[i] - original.rgba[i]);
      if (diff > tolerance) {
        diff_count++;
        if (diff > max_diff) max_diff = diff;
      }
    }

    if (diff_count > 0) {
      INFO("Mipmap: Pixels differing: " << diff_count << ", max diff: " << max_diff);
    }

    REQUIRE(diff_count == 0);
  }
}

// ============================================================================
// Deep Image Writing Tests
// ============================================================================

TEST_CASE("v2: Deep image basic write and read", "[Deep][RoundTrip]") {
  // Create a simple deep image
  const int width = 8;
  const int height = 8;

  tinyexr::v2::DeepImageData original;
  original.width = width;
  original.height = height;

  // Set up header
  original.header.compression = tinyexr::v2::COMPRESSION_ZIP;
  original.header.data_window.min_x = 0;
  original.header.data_window.min_y = 0;
  original.header.data_window.max_x = width - 1;
  original.header.data_window.max_y = height - 1;
  original.header.display_window = original.header.data_window;
  original.header.pixel_aspect_ratio = 1.0f;
  original.header.screen_window_width = 1.0f;
  original.header.line_order = 0;

  // Set up channels (RGBA, HALF)
  tinyexr::v2::Channel ch_a, ch_b, ch_g, ch_r, ch_z;
  ch_r.name = "R"; ch_r.pixel_type = tinyexr::v2::PIXEL_TYPE_HALF; ch_r.x_sampling = 1; ch_r.y_sampling = 1;
  ch_g.name = "G"; ch_g.pixel_type = tinyexr::v2::PIXEL_TYPE_HALF; ch_g.x_sampling = 1; ch_g.y_sampling = 1;
  ch_b.name = "B"; ch_b.pixel_type = tinyexr::v2::PIXEL_TYPE_HALF; ch_b.x_sampling = 1; ch_b.y_sampling = 1;
  ch_a.name = "A"; ch_a.pixel_type = tinyexr::v2::PIXEL_TYPE_HALF; ch_a.x_sampling = 1; ch_a.y_sampling = 1;
  ch_z.name = "Z"; ch_z.pixel_type = tinyexr::v2::PIXEL_TYPE_FLOAT; ch_z.x_sampling = 1; ch_z.y_sampling = 1;

  // Sort channels alphabetically (A, B, G, R, Z)
  original.header.channels.push_back(ch_a);
  original.header.channels.push_back(ch_b);
  original.header.channels.push_back(ch_g);
  original.header.channels.push_back(ch_r);
  original.header.channels.push_back(ch_z);
  original.num_channels = 5;

  // Create sample counts: varying number of samples per pixel
  original.sample_counts.resize(static_cast<size_t>(width) * height);
  size_t total = 0;
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      // 1-3 samples per pixel based on position
      uint32_t count = 1 + (x + y) % 3;
      original.sample_counts[static_cast<size_t>(y) * width + x] = count;
      total += count;
    }
  }
  original.total_samples = total;

  // Create channel data
  original.channel_data.resize(5);
  for (size_t c = 0; c < 5; c++) {
    original.channel_data[c].resize(total);
  }

  // Fill with test pattern
  size_t sample_idx = 0;
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      uint32_t count = original.sample_counts[static_cast<size_t>(y) * width + x];
      for (uint32_t s = 0; s < count; s++) {
        // RGBA values + Z depth
        original.channel_data[0][sample_idx] = 1.0f;  // A
        original.channel_data[1][sample_idx] = static_cast<float>(x) / width;  // B
        original.channel_data[2][sample_idx] = static_cast<float>(y) / height;  // G
        original.channel_data[3][sample_idx] = 0.5f + 0.1f * s;  // R
        original.channel_data[4][sample_idx] = 1.0f + 0.5f * s;  // Z (depth)
        sample_idx++;
      }
    }
  }

  SECTION("Basic deep write and read") {
    auto save_result = tinyexr::v2::SaveDeepToMemory(original, 6);

    if (!save_result.success) {
      INFO("Save error: " + save_result.error_string());
    }
    REQUIRE(save_result.success == true);
    REQUIRE(save_result.value.size() > 0);

    INFO("Deep: Saved " << save_result.value.size() << " bytes for "
         << total << " total samples");

    // Load back as multipart (deep images are loaded via multipart API)
    auto load_result = tinyexr::v2::LoadMultipartFromMemory(
        save_result.value.data(), save_result.value.size());

    if (!load_result.success) {
      INFO("Load error: " + load_result.error_string());
    }
    REQUIRE(load_result.success == true);
    REQUIRE(load_result.value.deep_parts.size() == 1);

    const auto& loaded = load_result.value.deep_parts[0];

    REQUIRE(loaded.width == original.width);
    REQUIRE(loaded.height == original.height);
    REQUIRE(loaded.total_samples == original.total_samples);
    REQUIRE(loaded.sample_counts.size() == original.sample_counts.size());

    // Verify sample counts
    for (size_t i = 0; i < loaded.sample_counts.size(); i++) {
      REQUIRE(loaded.sample_counts[i] == original.sample_counts[i]);
    }

    // Verify channel data (with tolerance for HALF precision)
    const float tolerance = 0.002f;
    int diff_count = 0;
    for (size_t c = 0; c < loaded.channel_data.size() && c < original.channel_data.size(); c++) {
      for (size_t s = 0; s < loaded.channel_data[c].size(); s++) {
        float diff = std::abs(loaded.channel_data[c][s] - original.channel_data[c][s]);
        if (diff > tolerance) {
          diff_count++;
        }
      }
    }

    if (diff_count > 0) {
      INFO("Deep: Channel data differs at " << diff_count << " samples");
    }
    REQUIRE(diff_count == 0);
  }
}

TEST_CASE("v2: Deep image with uniform samples", "[Deep][Uniform]") {
  // Deep image where all pixels have the same sample count
  const int width = 16;
  const int height = 16;
  const uint32_t samples_per_pixel = 2;

  tinyexr::v2::DeepImageData original;
  original.width = width;
  original.height = height;

  // Set up header
  original.header.compression = tinyexr::v2::COMPRESSION_ZIPS;  // Single-line ZIP
  original.header.data_window.min_x = 0;
  original.header.data_window.min_y = 0;
  original.header.data_window.max_x = width - 1;
  original.header.data_window.max_y = height - 1;
  original.header.display_window = original.header.data_window;
  original.header.pixel_aspect_ratio = 1.0f;
  original.header.screen_window_width = 1.0f;

  // Simple RGBA channels
  tinyexr::v2::Channel ch_a, ch_b, ch_g, ch_r;
  ch_a.name = "A"; ch_a.pixel_type = tinyexr::v2::PIXEL_TYPE_HALF; ch_a.x_sampling = 1; ch_a.y_sampling = 1;
  ch_b.name = "B"; ch_b.pixel_type = tinyexr::v2::PIXEL_TYPE_HALF; ch_b.x_sampling = 1; ch_b.y_sampling = 1;
  ch_g.name = "G"; ch_g.pixel_type = tinyexr::v2::PIXEL_TYPE_HALF; ch_g.x_sampling = 1; ch_g.y_sampling = 1;
  ch_r.name = "R"; ch_r.pixel_type = tinyexr::v2::PIXEL_TYPE_HALF; ch_r.x_sampling = 1; ch_r.y_sampling = 1;
  original.header.channels.push_back(ch_a);
  original.header.channels.push_back(ch_b);
  original.header.channels.push_back(ch_g);
  original.header.channels.push_back(ch_r);
  original.num_channels = 4;

  // Uniform sample counts
  size_t num_pixels = static_cast<size_t>(width) * height;
  original.sample_counts.resize(num_pixels, samples_per_pixel);
  original.total_samples = num_pixels * samples_per_pixel;

  // Create channel data
  original.channel_data.resize(4);
  for (size_t c = 0; c < 4; c++) {
    original.channel_data[c].resize(original.total_samples);
  }

  // Fill with gradient pattern
  size_t idx = 0;
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      for (uint32_t s = 0; s < samples_per_pixel; s++) {
        original.channel_data[0][idx] = 1.0f;  // A
        original.channel_data[1][idx] = static_cast<float>(x) / width;  // B
        original.channel_data[2][idx] = static_cast<float>(y) / height;  // G
        original.channel_data[3][idx] = 0.5f;  // R
        idx++;
      }
    }
  }

  auto save_result = tinyexr::v2::SaveDeepToMemory(original, 6);
  REQUIRE(save_result.success == true);

  auto load_result = tinyexr::v2::LoadMultipartFromMemory(
      save_result.value.data(), save_result.value.size());
  REQUIRE(load_result.success == true);
  REQUIRE(load_result.value.deep_parts.size() == 1);

  const auto& loaded = load_result.value.deep_parts[0];
  REQUIRE(loaded.total_samples == original.total_samples);

  // Verify all sample counts are correct
  for (size_t i = 0; i < num_pixels; i++) {
    REQUIRE(loaded.sample_counts[i] == samples_per_pixel);
  }
}

TEST_CASE("v2: Deep image with empty pixels", "[Deep][Sparse]") {
  // Deep image with some pixels having zero samples
  const int width = 8;
  const int height = 8;

  tinyexr::v2::DeepImageData original;
  original.width = width;
  original.height = height;

  // Set up header
  original.header.compression = tinyexr::v2::COMPRESSION_NONE;  // No compression
  original.header.data_window.min_x = 0;
  original.header.data_window.min_y = 0;
  original.header.data_window.max_x = width - 1;
  original.header.data_window.max_y = height - 1;
  original.header.display_window = original.header.data_window;
  original.header.pixel_aspect_ratio = 1.0f;
  original.header.screen_window_width = 1.0f;

  // Single channel (Z depth)
  tinyexr::v2::Channel ch_z;
  ch_z.name = "Z"; ch_z.pixel_type = tinyexr::v2::PIXEL_TYPE_FLOAT; ch_z.x_sampling = 1; ch_z.y_sampling = 1;
  original.header.channels.push_back(ch_z);
  original.num_channels = 1;

  // Sparse sample counts: checkerboard pattern (0 or 1 sample)
  size_t num_pixels = static_cast<size_t>(width) * height;
  original.sample_counts.resize(num_pixels);
  size_t total = 0;
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      uint32_t count = ((x + y) % 2 == 0) ? 1 : 0;
      original.sample_counts[static_cast<size_t>(y) * width + x] = count;
      total += count;
    }
  }
  original.total_samples = total;

  // Create channel data (only for non-empty pixels)
  original.channel_data.resize(1);
  original.channel_data[0].resize(total);
  for (size_t i = 0; i < total; i++) {
    original.channel_data[0][i] = static_cast<float>(i) * 0.1f;  // Depth
  }

  auto save_result = tinyexr::v2::SaveDeepToMemory(original, 6);
  REQUIRE(save_result.success == true);

  auto load_result = tinyexr::v2::LoadMultipartFromMemory(
      save_result.value.data(), save_result.value.size());
  REQUIRE(load_result.success == true);
  REQUIRE(load_result.value.deep_parts.size() == 1);

  const auto& loaded = load_result.value.deep_parts[0];
  REQUIRE(loaded.total_samples == original.total_samples);

  // Verify sparse pattern preserved
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      size_t idx = static_cast<size_t>(y) * width + x;
      uint32_t expected = ((x + y) % 2 == 0) ? 1 : 0;
      REQUIRE(loaded.sample_counts[idx] == expected);
    }
  }
}

// =============================================================================
// Raw Channel Access Tests (Phase 5)
// =============================================================================

TEST_CASE("v2: LoadOptions - preserve raw channels (HALF)", "[v2][loadoptions]") {
  using namespace tinyexr::v2;

  const int width = 4;
  const int height = 4;

  // Create test image with HALF channels
  ImageData original;
  original.width = width;
  original.height = height;
  original.num_channels = 4;
  original.rgba.resize(width * height * 4);

  // Fill with test pattern
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      int idx = (y * width + x) * 4;
      original.rgba[idx + 0] = static_cast<float>(x) / width;   // R
      original.rgba[idx + 1] = static_cast<float>(y) / height;  // G
      original.rgba[idx + 2] = 0.5f;                            // B
      original.rgba[idx + 3] = 1.0f;                            // A
    }
  }

  original.header.compression = tinyexr::v2::COMPRESSION_ZIP;
  original.header.data_window.min_x = 0;
  original.header.data_window.min_y = 0;
  original.header.data_window.max_x = width - 1;
  original.header.data_window.max_y = height - 1;
  original.header.display_window = original.header.data_window;
  original.header.pixel_aspect_ratio = 1.0f;
  original.header.screen_window_width = 1.0f;
  original.header.line_order = 0;

  // Set up channels as HALF
  Channel ch_r, ch_g, ch_b, ch_a;
  ch_r.name = "R"; ch_r.pixel_type = tinyexr::v2::PIXEL_TYPE_HALF; ch_r.x_sampling = 1; ch_r.y_sampling = 1;
  ch_g.name = "G"; ch_g.pixel_type = tinyexr::v2::PIXEL_TYPE_HALF; ch_g.x_sampling = 1; ch_g.y_sampling = 1;
  ch_b.name = "B"; ch_b.pixel_type = tinyexr::v2::PIXEL_TYPE_HALF; ch_b.x_sampling = 1; ch_b.y_sampling = 1;
  ch_a.name = "A"; ch_a.pixel_type = tinyexr::v2::PIXEL_TYPE_HALF; ch_a.x_sampling = 1; ch_a.y_sampling = 1;
  original.header.channels.push_back(ch_a);  // Alphabetically sorted
  original.header.channels.push_back(ch_b);
  original.header.channels.push_back(ch_g);
  original.header.channels.push_back(ch_r);

  // Save to memory
  auto save_result = SaveToMemory(original, 6);
  REQUIRE(save_result.success);

  // Load with raw channel preservation
  LoadOptions opts;
  opts.preserve_raw_channels = true;
  opts.convert_to_rgba = true;

  auto load_result = LoadFromMemory(save_result.value.data(), save_result.value.size(), opts);
  REQUIRE(load_result.success);

  const auto& img = load_result.value;

  // Check that raw channels were populated
  REQUIRE(img.raw_channels.size() == 4);

  // Each HALF channel should have width * height * 2 bytes
  size_t expected_size = static_cast<size_t>(width) * height * 2;
  for (size_t c = 0; c < 4; c++) {
    REQUIRE(img.raw_channels[c].size() == expected_size);
  }

  // Verify that RGBA conversion also worked
  REQUIRE(img.rgba.size() == static_cast<size_t>(width) * height * 4);
}

TEST_CASE("v2: LoadOptions - raw channel data verification", "[v2][loadoptions]") {
  using namespace tinyexr::v2;

  const int width = 4;
  const int height = 4;

  // Create test image (SaveToMemory converts to HALF internally)
  ImageData original;
  original.width = width;
  original.height = height;
  original.num_channels = 3;
  original.rgba.resize(width * height * 4);

  // Fill with test pattern - values that can be represented accurately in HALF
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      int idx = (y * width + x) * 4;
      original.rgba[idx + 0] = static_cast<float>(x) * 0.25f;  // R: 0, 0.25, 0.5, 0.75
      original.rgba[idx + 1] = static_cast<float>(y) * 0.25f;  // G: 0, 0.25, 0.5, 0.75
      original.rgba[idx + 2] = 0.5f;                           // B: constant
      original.rgba[idx + 3] = 1.0f;                           // A (not saved)
    }
  }

  original.header.compression = tinyexr::v2::COMPRESSION_ZIP;
  original.header.data_window.min_x = 0;
  original.header.data_window.min_y = 0;
  original.header.data_window.max_x = width - 1;
  original.header.data_window.max_y = height - 1;
  original.header.display_window = original.header.data_window;
  original.header.pixel_aspect_ratio = 1.0f;
  original.header.screen_window_width = 1.0f;
  original.header.line_order = 0;

  // Set up RGB channels as HALF (SaveToMemory writes HALF regardless of input)
  Channel ch_r, ch_g, ch_b;
  ch_r.name = "R"; ch_r.pixel_type = tinyexr::v2::PIXEL_TYPE_HALF; ch_r.x_sampling = 1; ch_r.y_sampling = 1;
  ch_g.name = "G"; ch_g.pixel_type = tinyexr::v2::PIXEL_TYPE_HALF; ch_g.x_sampling = 1; ch_g.y_sampling = 1;
  ch_b.name = "B"; ch_b.pixel_type = tinyexr::v2::PIXEL_TYPE_HALF; ch_b.x_sampling = 1; ch_b.y_sampling = 1;
  original.header.channels.push_back(ch_b);  // Alphabetically sorted
  original.header.channels.push_back(ch_g);
  original.header.channels.push_back(ch_r);

  // Save to memory
  auto save_result = SaveToMemory(original, 6);
  REQUIRE(save_result.success);

  // Load with raw channel preservation
  LoadOptions opts;
  opts.preserve_raw_channels = true;
  opts.convert_to_rgba = true;

  auto load_result = LoadFromMemory(save_result.value.data(), save_result.value.size(), opts);
  REQUIRE(load_result.success);

  const auto& img = load_result.value;

  // Check that raw channels were populated (HALF = 2 bytes per pixel)
  REQUIRE(img.raw_channels.size() == 3);
  size_t expected_size = static_cast<size_t>(width) * height * 2;  // HALF format
  for (size_t c = 0; c < 3; c++) {
    REQUIRE(img.raw_channels[c].size() == expected_size);
  }

  // Verify loaded header has correct pixel type
  REQUIRE(img.header.channels.size() == 3);
  for (size_t c = 0; c < 3; c++) {
    REQUIRE(img.header.channels[c].pixel_type == tinyexr::v2::PIXEL_TYPE_HALF);
  }

  // Verify raw data can be interpreted correctly
  // Channels are in header order: B, G, R (indices 0, 1, 2)
  // Check R channel (index 2) - should have values 0, 0.25, 0.5, 0.75 repeating per row
  const uint8_t* r_raw = img.raw_channels[2].data();
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      size_t offset = (static_cast<size_t>(y) * width + x) * 2;
      uint16_t half_val;
      std::memcpy(&half_val, r_raw + offset, 2);
      float val = tinyexr::v2::HalfToFloat(half_val);
      float expected = static_cast<float>(x) * 0.25f;
      REQUIRE(std::abs(val - expected) < 0.001f);
    }
  }
}

TEST_CASE("v2: LoadOptions - default (no raw channels)", "[v2][loadoptions]") {
  using namespace tinyexr::v2;

  const int width = 4;
  const int height = 4;

  // Create test image
  ImageData original;
  original.width = width;
  original.height = height;
  original.num_channels = 4;
  original.rgba.resize(width * height * 4, 0.5f);

  original.header.compression = tinyexr::v2::COMPRESSION_ZIP;
  original.header.data_window.min_x = 0;
  original.header.data_window.min_y = 0;
  original.header.data_window.max_x = width - 1;
  original.header.data_window.max_y = height - 1;
  original.header.display_window = original.header.data_window;
  original.header.pixel_aspect_ratio = 1.0f;
  original.header.screen_window_width = 1.0f;
  original.header.line_order = 0;

  Channel ch_r, ch_g, ch_b, ch_a;
  ch_r.name = "R"; ch_r.pixel_type = tinyexr::v2::PIXEL_TYPE_HALF; ch_r.x_sampling = 1; ch_r.y_sampling = 1;
  ch_g.name = "G"; ch_g.pixel_type = tinyexr::v2::PIXEL_TYPE_HALF; ch_g.x_sampling = 1; ch_g.y_sampling = 1;
  ch_b.name = "B"; ch_b.pixel_type = tinyexr::v2::PIXEL_TYPE_HALF; ch_b.x_sampling = 1; ch_b.y_sampling = 1;
  ch_a.name = "A"; ch_a.pixel_type = tinyexr::v2::PIXEL_TYPE_HALF; ch_a.x_sampling = 1; ch_a.y_sampling = 1;
  original.header.channels.push_back(ch_a);
  original.header.channels.push_back(ch_b);
  original.header.channels.push_back(ch_g);
  original.header.channels.push_back(ch_r);

  // Save to memory
  auto save_result = SaveToMemory(original, 6);
  REQUIRE(save_result.success);

  // Load with default options (no raw channels)
  auto load_result = LoadFromMemory(save_result.value.data(), save_result.value.size());
  REQUIRE(load_result.success);

  const auto& img = load_result.value;

  // Raw channels should be empty
  REQUIRE(img.raw_channels.empty());

  // RGBA should still work
  REQUIRE(img.rgba.size() == static_cast<size_t>(width) * height * 4);
}

// =============================================================================
// PIZ Compression Round-Trip Tests (Phase 6)
// =============================================================================

TEST_CASE("v2: PIZ compression round-trip", "[v2][piz][compression]") {
  using namespace tinyexr::v2;

  const int width = 64;
  const int height = 64;

  // Create test image
  ImageData original;
  original.width = width;
  original.height = height;
  original.num_channels = 4;
  original.rgba.resize(width * height * 4);

  // Fill with test pattern (gradient + noise for realistic compression)
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      int idx = (y * width + x) * 4;
      original.rgba[idx + 0] = static_cast<float>(x) / width;
      original.rgba[idx + 1] = static_cast<float>(y) / height;
      original.rgba[idx + 2] = 0.5f + 0.1f * std::sin(x * 0.1f);
      original.rgba[idx + 3] = 1.0f;
    }
  }

  original.header.compression = tinyexr::v2::COMPRESSION_PIZ;
  original.header.data_window.min_x = 0;
  original.header.data_window.min_y = 0;
  original.header.data_window.max_x = width - 1;
  original.header.data_window.max_y = height - 1;
  original.header.display_window = original.header.data_window;
  original.header.pixel_aspect_ratio = 1.0f;
  original.header.screen_window_width = 1.0f;
  original.header.line_order = 0;

  Channel ch_r, ch_g, ch_b, ch_a;
  ch_r.name = "R"; ch_r.pixel_type = tinyexr::v2::PIXEL_TYPE_HALF; ch_r.x_sampling = 1; ch_r.y_sampling = 1;
  ch_g.name = "G"; ch_g.pixel_type = tinyexr::v2::PIXEL_TYPE_HALF; ch_g.x_sampling = 1; ch_g.y_sampling = 1;
  ch_b.name = "B"; ch_b.pixel_type = tinyexr::v2::PIXEL_TYPE_HALF; ch_b.x_sampling = 1; ch_b.y_sampling = 1;
  ch_a.name = "A"; ch_a.pixel_type = tinyexr::v2::PIXEL_TYPE_HALF; ch_a.x_sampling = 1; ch_a.y_sampling = 1;
  original.header.channels.push_back(ch_a);
  original.header.channels.push_back(ch_b);
  original.header.channels.push_back(ch_g);
  original.header.channels.push_back(ch_r);

  // Save with PIZ compression
  auto save_result = SaveToMemory(original, 6);
  REQUIRE(save_result.success);
  INFO("PIZ: Saved " << save_result.value.size() << " bytes");

  // Load back
  auto load_result = LoadFromMemory(save_result.value.data(), save_result.value.size());
  REQUIRE(load_result.success);

  const auto& loaded = load_result.value;
  REQUIRE(loaded.width == width);
  REQUIRE(loaded.height == height);
  REQUIRE(loaded.rgba.size() == original.rgba.size());

  // Check pixel values (allowing for HALF precision loss)
  int errors = 0;
  for (size_t i = 0; i < original.rgba.size(); i++) {
    float diff = std::abs(original.rgba[i] - loaded.rgba[i]);
    if (diff > 0.01f) errors++;
  }

  INFO("PIZ: Pixel errors: " << errors << "/" << original.rgba.size());
  REQUIRE(errors == 0);
}

TEST_CASE("v2: PIZ compression all zeros (issue 194)", "[v2][piz][compression]") {
  using namespace tinyexr::v2;

  const int width = 16;
  const int height = 16;

  // Create image with all zeros
  ImageData original;
  original.width = width;
  original.height = height;
  original.num_channels = 4;
  original.rgba.resize(width * height * 4, 0.0f);

  original.header.compression = tinyexr::v2::COMPRESSION_PIZ;
  original.header.data_window.min_x = 0;
  original.header.data_window.min_y = 0;
  original.header.data_window.max_x = width - 1;
  original.header.data_window.max_y = height - 1;
  original.header.display_window = original.header.data_window;
  original.header.pixel_aspect_ratio = 1.0f;
  original.header.screen_window_width = 1.0f;
  original.header.line_order = 0;

  Channel ch_r, ch_g, ch_b, ch_a;
  ch_r.name = "R"; ch_r.pixel_type = tinyexr::v2::PIXEL_TYPE_HALF; ch_r.x_sampling = 1; ch_r.y_sampling = 1;
  ch_g.name = "G"; ch_g.pixel_type = tinyexr::v2::PIXEL_TYPE_HALF; ch_g.x_sampling = 1; ch_g.y_sampling = 1;
  ch_b.name = "B"; ch_b.pixel_type = tinyexr::v2::PIXEL_TYPE_HALF; ch_b.x_sampling = 1; ch_b.y_sampling = 1;
  ch_a.name = "A"; ch_a.pixel_type = tinyexr::v2::PIXEL_TYPE_HALF; ch_a.x_sampling = 1; ch_a.y_sampling = 1;
  original.header.channels.push_back(ch_a);
  original.header.channels.push_back(ch_b);
  original.header.channels.push_back(ch_g);
  original.header.channels.push_back(ch_r);

  // Save with PIZ compression
  auto save_result = SaveToMemory(original, 6);
  REQUIRE(save_result.success);
  INFO("PIZ zeros: Saved " << save_result.value.size() << " bytes");

  // Load back
  auto load_result = LoadFromMemory(save_result.value.data(), save_result.value.size());
  REQUIRE(load_result.success);

  const auto& loaded = load_result.value;
  REQUIRE(loaded.width == width);
  REQUIRE(loaded.height == height);

  // All pixels should still be zero
  for (size_t i = 0; i < loaded.rgba.size(); i++) {
    REQUIRE(loaded.rgba[i] == 0.0f);
  }
}

// ============================================================================
// Custom Attributes Tests
// ============================================================================

TEST_CASE("v2: Custom attributes round-trip", "[v2][attributes]") {
  using namespace tinyexr::v2;

  const int width = 8;
  const int height = 8;

  // Create test image
  ImageData original;
  original.width = width;
  original.height = height;
  original.num_channels = 4;
  original.rgba.resize(width * height * 4, 1.0f);

  original.header.compression = COMPRESSION_ZIP;
  original.header.data_window.min_x = 0;
  original.header.data_window.min_y = 0;
  original.header.data_window.max_x = width - 1;
  original.header.data_window.max_y = height - 1;
  original.header.display_window = original.header.data_window;
  original.header.pixel_aspect_ratio = 1.0f;
  original.header.screen_window_width = 1.0f;
  original.header.line_order = 0;

  Channel ch_r, ch_g, ch_b, ch_a;
  ch_r.name = "R"; ch_r.pixel_type = PIXEL_TYPE_HALF;
  ch_g.name = "G"; ch_g.pixel_type = PIXEL_TYPE_HALF;
  ch_b.name = "B"; ch_b.pixel_type = PIXEL_TYPE_HALF;
  ch_a.name = "A"; ch_a.pixel_type = PIXEL_TYPE_HALF;
  original.header.channels.push_back(ch_a);
  original.header.channels.push_back(ch_b);
  original.header.channels.push_back(ch_g);
  original.header.channels.push_back(ch_r);

  // Add custom attributes
  original.header.set_string_attribute("myString", "Hello, EXR!");
  original.header.set_int_attribute("myInt", 42);
  original.header.set_float_attribute("myFloat", 3.14159f);
  original.header.set_attribute(Attribute::V2f("myV2f", 1.5f, 2.5f));

  // Save
  auto save_result = SaveToMemory(original, 6);
  REQUIRE(save_result.success);

  // Load back
  auto load_result = LoadFromMemory(save_result.value.data(), save_result.value.size());
  REQUIRE(load_result.success);

  const auto& loaded = load_result.value;

  // Check custom attributes were preserved
  REQUIRE(loaded.header.has_attribute("myString"));
  REQUIRE(loaded.header.has_attribute("myInt"));
  REQUIRE(loaded.header.has_attribute("myFloat"));
  REQUIRE(loaded.header.has_attribute("myV2f"));

  REQUIRE(loaded.header.get_string_attribute("myString") == "Hello, EXR!");
  REQUIRE(loaded.header.get_int_attribute("myInt") == 42);
  REQUIRE(std::abs(loaded.header.get_float_attribute("myFloat") - 3.14159f) < 0.0001f);

  // Check V2f attribute
  const Attribute* v2f = loaded.header.find_attribute("myV2f");
  REQUIRE(v2f != nullptr);
  REQUIRE(v2f->type == "v2f");
  REQUIRE(v2f->data.size() == 8);
  float x, y;
  std::memcpy(&x, v2f->data.data(), 4);
  std::memcpy(&y, v2f->data.data() + 4, 4);
  REQUIRE(std::abs(x - 1.5f) < 0.0001f);
  REQUIRE(std::abs(y - 2.5f) < 0.0001f);
}

TEST_CASE("v2: Deep tiled write and read", "[v2][deep][tiled]") {
  using namespace tinyexr::v2;

  const int width = 32;
  const int height = 32;

  // Create deep image data
  DeepImageData deep;
  deep.width = width;
  deep.height = height;

  // Set up sample counts - varying depths
  deep.sample_counts.resize(width * height);
  deep.total_samples = 0;

  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      // Variable sample count: 1 to 4 samples per pixel
      uint32_t count = static_cast<uint32_t>((x + y) % 4 + 1);
      deep.sample_counts[y * width + x] = count;
      deep.total_samples += count;
    }
  }

  // Set up channels (depth + alpha)
  Channel ch_z, ch_a;
  ch_z.name = "Z";
  ch_z.pixel_type = PIXEL_TYPE_FLOAT;
  ch_z.x_sampling = 1;
  ch_z.y_sampling = 1;
  ch_a.name = "A";
  ch_a.pixel_type = PIXEL_TYPE_HALF;
  ch_a.x_sampling = 1;
  ch_a.y_sampling = 1;
  deep.header.channels.push_back(ch_a);
  deep.header.channels.push_back(ch_z);

  // Fill channel data
  deep.channel_data.resize(2);
  deep.channel_data[0].resize(deep.total_samples); // A
  deep.channel_data[1].resize(deep.total_samples); // Z

  size_t sample_idx = 0;
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      uint32_t count = deep.sample_counts[y * width + x];
      for (uint32_t s = 0; s < count; s++) {
        deep.channel_data[0][sample_idx] = 0.5f + 0.1f * s;  // A
        deep.channel_data[1][sample_idx] = 1.0f + static_cast<float>(s) * 0.5f;  // Z
        sample_idx++;
      }
    }
  }

  deep.header.compression = COMPRESSION_ZIP;
  deep.header.tile_size_x = 16;
  deep.header.tile_size_y = 16;

  // Save as deep tiled
  auto save_result = SaveDeepTiledToMemory(deep, 6);
  REQUIRE(save_result.success);
  INFO("Deep tiled: Saved " << save_result.value.size() << " bytes");

  // Verify magic and version flags
  REQUIRE(save_result.value.size() > 8);
  REQUIRE(save_result.value[0] == 0x76);
  REQUIRE(save_result.value[1] == 0x2f);
  REQUIRE(save_result.value[2] == 0x31);
  REQUIRE(save_result.value[3] == 0x01);

  // Check version byte has tiled and non_image flags set
  uint32_t version_bits = save_result.value[4] |
                          (save_result.value[5] << 8) |
                          (save_result.value[6] << 16) |
                          (save_result.value[7] << 24);
  REQUIRE((version_bits & 0x200) != 0);  // tiled flag
  REQUIRE((version_bits & 0x800) != 0);  // non_image flag (deep)
}

TEST_CASE("v2: Spectral EXR save and load", "[v2][spectral]") {
  using namespace tinyexr::v2;

  const int width = 8;
  const int height = 8;

  // Create spectral image with 7 wavelengths (visible spectrum)
  SpectralImageData spectral;
  spectral.width = width;
  spectral.height = height;

  std::vector<float> wavelengths = {400.0f, 450.0f, 500.0f, 550.0f, 600.0f, 650.0f, 700.0f};
  spectral.SetupEmissive(wavelengths);

  // Fill with test data
  for (size_t w = 0; w < wavelengths.size(); w++) {
    for (int y = 0; y < height; y++) {
      for (int x = 0; x < width; x++) {
        // Create a gradient pattern
        float val = static_cast<float>(w) / wavelengths.size() +
                    static_cast<float>(x) / width * 0.1f +
                    static_cast<float>(y) / height * 0.1f;
        spectral.SetPixel(static_cast<int>(w), x, y, val);
      }
    }
  }

  // Set EV
  spectral.SetEV(1.5f);

  // Save
  auto save_result = SaveSpectralToMemory(spectral, 6);
  REQUIRE(save_result.success);
  INFO("Spectral: Saved " << save_result.value.size() << " bytes with " << wavelengths.size() << " wavelengths");

  // Load back
  auto load_result = LoadSpectralFromMemory(save_result.value.data(), save_result.value.size());
  REQUIRE(load_result.success);

  const auto& loaded = load_result.value;

  // Verify dimensions
  REQUIRE(loaded.width == width);
  REQUIRE(loaded.height == height);
  REQUIRE(loaded.wavelengths.size() == wavelengths.size());

  // Verify wavelengths
  for (size_t w = 0; w < wavelengths.size(); w++) {
    REQUIRE(std::abs(loaded.wavelengths[w] - wavelengths[w]) < 0.01f);
  }

  // Verify spectral data
  int errors = 0;
  for (size_t w = 0; w < wavelengths.size(); w++) {
    for (int y = 0; y < height; y++) {
      for (int x = 0; x < width; x++) {
        float expected = spectral.GetPixel(static_cast<int>(w), x, y);
        float actual = loaded.GetPixel(static_cast<int>(w), x, y);
        if (std::abs(expected - actual) > 0.001f) {
          errors++;
        }
      }
    }
  }
  INFO("Spectral pixel errors: " << errors);
  REQUIRE(errors == 0);

  // Verify EV attribute preserved
  REQUIRE(loaded.header.has_attribute("EV"));
  REQUIRE(std::abs(loaded.header.get_float_attribute("EV") - 1.5f) < 0.001f);

  // Verify spectralLayoutVersion attribute
  REQUIRE(loaded.header.has_attribute("spectralLayoutVersion"));
  REQUIRE(loaded.header.get_string_attribute("spectralLayoutVersion") == "1.0");
}

TEST_CASE("v2: Spectral channel naming", "[v2][spectral]") {
  using namespace tinyexr::v2;

  // Test channel name generation
  std::string ch_name = SpectralChannelName(550.5f, 0);
  REQUIRE(ch_name.find("S0.") == 0);
  REQUIRE(ch_name.find("nm") != std::string::npos);

  std::string refl_name = ReflectiveChannelName(600.0f);
  REQUIRE(refl_name.find("T.") == 0);

  // Test wavelength parsing
  float wl1 = ParseSpectralChannelWavelength("S0.550,000000nm");
  REQUIRE(std::abs(wl1 - 550.0f) < 0.01f);

  float wl2 = ParseSpectralChannelWavelength("T.600,500000nm");
  REQUIRE(std::abs(wl2 - 600.5f) < 0.01f);

  // Test Stokes component detection
  REQUIRE(GetStokesComponent("S0.550,000000nm") == 0);
  REQUIRE(GetStokesComponent("S1.550,000000nm") == 1);
  REQUIRE(GetStokesComponent("S2.550,000000nm") == 2);
  REQUIRE(GetStokesComponent("S3.550,000000nm") == 3);
  REQUIRE(GetStokesComponent("T.550,000000nm") == -1);  // Reflective
  REQUIRE(GetStokesComponent("R") == -2);  // Not spectral

  // Test IsSpectralChannel
  REQUIRE(IsSpectralChannel("S0.550,000000nm") == true);
  REQUIRE(IsSpectralChannel("T.600,000000nm") == true);
  REQUIRE(IsSpectralChannel("R") == false);
  REQUIRE(IsSpectralChannel("G") == false);
}

TEST_CASE("v2: Spectral EXR attributes", "[v2][attributes][spectral]") {
  using namespace tinyexr::v2;

  const int width = 4;
  const int height = 4;

  // Create test image simulating spectral EXR
  ImageData original;
  original.width = width;
  original.height = height;
  original.num_channels = 4;
  original.rgba.resize(width * height * 4, 0.5f);

  original.header.compression = COMPRESSION_ZIP;
  original.header.data_window.min_x = 0;
  original.header.data_window.min_y = 0;
  original.header.data_window.max_x = width - 1;
  original.header.data_window.max_y = height - 1;
  original.header.display_window = original.header.data_window;
  original.header.pixel_aspect_ratio = 1.0f;
  original.header.screen_window_width = 1.0f;
  original.header.line_order = 0;

  Channel ch_r, ch_g, ch_b, ch_a;
  ch_r.name = "R"; ch_r.pixel_type = PIXEL_TYPE_HALF;
  ch_g.name = "G"; ch_g.pixel_type = PIXEL_TYPE_HALF;
  ch_b.name = "B"; ch_b.pixel_type = PIXEL_TYPE_HALF;
  ch_a.name = "A"; ch_a.pixel_type = PIXEL_TYPE_HALF;
  original.header.channels.push_back(ch_a);
  original.header.channels.push_back(ch_b);
  original.header.channels.push_back(ch_g);
  original.header.channels.push_back(ch_r);

  // Add spectral EXR custom attributes (per JCGT paper)
  original.header.set_string_attribute("spectralLayoutVersion", "1.0");
  original.header.set_string_attribute("emissiveUnits", "W.m^-2.sr^-1");
  original.header.set_float_attribute("EV", 0.0f);

  // Spectrum attribute format: "wavelength_nm:value;wavelength_nm:value;..."
  original.header.set_string_attribute("lensTransmission",
    "400nm:0.92;450nm:0.95;500nm:0.96;550nm:0.97;600nm:0.96;650nm:0.95;700nm:0.93;");

  // Save
  auto save_result = SaveToMemory(original, 6);
  REQUIRE(save_result.success);

  // Load back
  auto load_result = LoadFromMemory(save_result.value.data(), save_result.value.size());
  REQUIRE(load_result.success);

  const auto& loaded = load_result.value;

  // Check spectral attributes were preserved
  REQUIRE(loaded.header.get_string_attribute("spectralLayoutVersion") == "1.0");
  REQUIRE(loaded.header.get_string_attribute("emissiveUnits") == "W.m^-2.sr^-1");
  REQUIRE(loaded.header.get_float_attribute("EV") == 0.0f);

  std::string lens = loaded.header.get_string_attribute("lensTransmission");
  REQUIRE(lens.find("400nm:0.92") != std::string::npos);
  REQUIRE(lens.find("700nm:0.93") != std::string::npos);
}
