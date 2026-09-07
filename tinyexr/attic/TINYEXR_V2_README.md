# TinyEXR V2 API - Enhanced Error Reporting and Safer Memory Access

## Deprecated - Reference Only

**Status:** The experimental V2 API has been retired to `attic/`. It is no
longer built, tested, or maintained in the active TinyEXR tree. For production
use, use the stable V1 API in `tinyexr.h`; for the current rewrite, use the
pure-C11 v3 API under `include/exr.h` + `src/`.

## Overview

The V2 API is a refactored version of TinyEXR with improved error handling, safer memory access, and human-readable error messages. It introduces breaking changes from the original API to provide better safety guarantees and developer experience.

This experimental API is provided for:
- Testing new error handling approaches
- Evaluating safer memory access patterns
- Gathering community feedback
- Exploring potential future stable release

## Key Features

### 1. **StreamReader Class** (`streamreader.hh`)
- Header-only, exception-free, RTTI-free
- Endian-aware reading (Little, Big, Native)
- Automatic endian swapping
- Bounds checking on all operations
- Methods: `read()`, `read1()`, `read2()`, `read4()`, `read8()`, `seek()`, `rewind()`

### 2. **Enhanced Reader Class** (`tinyexr_v2.hh`)
- Wraps StreamReader with error accumulation
- Context-aware error messages
- Error stack with detailed position information
- Methods include all StreamReader methods plus `read_string()`, `read_fixed_string()`

### 3. **Result<T> Type**
- Modern error handling without exceptions
- Contains success/failure status
- Accumulates multiple errors and warnings
- Human-readable error formatting

### 4. **Structured Error Information**
```cpp
struct ErrorInfo {
  ErrorCode code;           // Enumerated error type
  std::string message;      // Human-readable description
  std::string context;      // Where the error occurred
  size_t byte_position;     // Position in stream
  int line_number;          // Optional line number
};
```

### 5. **Error Codes**
```cpp
enum class ErrorCode {
  Success,
  InvalidArgument,
  InvalidMagicNumber,
  InvalidVersion,
  InvalidData,
  OutOfBounds,
  UnsupportedFormat,
  CompressionError,
  MissingRequiredAttribute,
  AllocationError,
  IOError
};
```

## API Comparison

### V1 API (Original)
```cpp
const char* err = nullptr;
float* rgba = nullptr;
int width, height;

int ret = LoadEXRFromMemory(&rgba, &width, &height, memory, size, &err);
if (ret != TINYEXR_SUCCESS) {
  if (err) {
    fprintf(stderr, "Error: %s\n", err);
    FreeEXRErrorMessage(err);
  }
  return -1;
}
```

Problems with V1:
- Manual memory management for error strings
- Single error message (no error stack)
- No context about where error occurred
- Requires checking return codes and null pointers
- Error position information lost

### V2 API (New)
```cpp
auto result = tinyexr::v2::LoadFromMemory(data, size);

if (!result.success) {
  // Human-readable error with full context
  std::cerr << result.error_string() << "\n";

  // Access structured error information
  for (const auto& err : result.errors) {
    std::cerr << "Error at byte " << err.byte_position
              << ": " << err.message << "\n";
  }
  return -1;
}

// Access image data
const auto& img = result.value;
std::cout << "Loaded " << img.width << "x" << img.height << " image\n";
```

Benefits of V2:
- ✓ Automatic memory management
- ✓ Error stack with multiple errors
- ✓ Context and position information
- ✓ Type-safe Result<T> pattern
- ✓ Human-readable error formatting
- ✓ Warnings separate from errors

## Usage Examples

### Example 1: Parse Version Header
```cpp
#include "tinyexr_v2.hh"
#include "tinyexr_v2_impl.hh"

tinyexr::v2::Reader reader(data, size);
auto result = tinyexr::v2::ParseVersion(reader);

if (!result.success) {
  std::cerr << result.error_string();
  return;
}

const auto& version = result.value;
std::cout << "EXR Version: " << version.version << "\n";
std::cout << "Tiled: " << version.tiled << "\n";
```

### Example 2: Load Complete EXR File
```cpp
auto result = tinyexr::v2::LoadFromMemory(file_data.data(), file_data.size());

if (!result.success) {
  // Detailed error information
  std::cerr << "Failed to load EXR:\n";
  std::cerr << result.error_string();
  return;
}

// Display warnings (non-fatal)
if (!result.warnings.empty()) {
  std::cout << "Warnings:\n" << result.warnings_string();
}

// Access image data
const auto& img = result.value;
std::cout << "Dimensions: " << img.width << "x" << img.height << "\n";
std::cout << "Compression: " << img.header.compression << "\n";
```

### Example 3: Custom Error Handling with Reader
```cpp
tinyexr::v2::Reader reader(data, size);
reader.set_context("Parsing custom data");

uint32_t value;
if (!reader.read4(&value)) {
  // Error automatically accumulated with context
  std::cerr << reader.error_string();
  return;
}

// Check for accumulated errors
if (reader.has_error()) {
  const auto& err = reader.last_error();
  std::cerr << "Error code: " << static_cast<int>(err.code) << "\n";
  std::cerr << "Position: " << err.byte_position << "\n";
  std::cerr << "Message: " << err.message << "\n";
}
```

## Error Message Examples

### Invalid Magic Number
```
[ERROR] Invalid Magic Number
  Message: Invalid EXR magic number. Expected [0x76 0x2f 0x31 0x01], got [0x00 0x00 0x00 0x00]. This is not a valid OpenEXR file.
  Context: Parsing EXR version header
```

### Invalid Version
```
[ERROR] Invalid Version
  Message: Unsupported EXR version 3. Only version 2 is supported.
  Context: Parsing EXR version header
  Position: byte 4 (0x4)
```

### Out of Bounds Read
```
[ERROR] Out of Bounds
  Message: Failed to read 5 bytes
  Context: Reading pixel data
  Position: byte 1024 (0x400)
```

### Missing Required Attribute
```
[ERROR] Missing Required Attribute
  Message: Required attribute 'dataWindow' not found
  Context: Parsing EXR header attributes
  Position: byte 56 (0x38)
```

## Building and Testing

### Compile Test Suite
```bash
g++ -std=c++11 -Wall -Wextra -I. test_v2_api.cc -o test_v2_api
./test_v2_api
```

### Compile Example Program
```bash
g++ -std=c++11 -Wall -Wextra -I. example_v2_usage.cc -o example_v2_usage
./example_v2_usage input.exr
```

## Design Principles

1. **No Exceptions, No RTTI**: Compatible with embedded systems and strict environments
2. **Bounds Checking**: All memory accesses are validated
3. **Error Accumulation**: Multiple errors collected, not just first failure
4. **Human-Readable**: Error messages designed for developers, not machines
5. **Context Preservation**: Know exactly where and why errors occurred
6. **Type Safety**: Result<T> prevents forgetting to check errors
7. **Zero-Cost Abstractions**: Minimal overhead compared to manual checks

## Files

- `streamreader.hh` - Low-level safe memory reader with endian support
- `exr_reader.hh` - Reader wrapper with error accumulation (V1 compatible)
- `tinyexr_v2.hh` - V2 API declarations with Result<T> and ErrorInfo
- `tinyexr_v2_impl.hh` - V2 API implementation
- `test_v2_api.cc` - Comprehensive test suite
- `example_v2_usage.cc` - Example usage program

## Migration Guide: V1 to V2

### Error Handling
**V1:**
```cpp
const char* err = nullptr;
int ret = SomeFunction(&err);
if (ret != TINYEXR_SUCCESS) {
  fprintf(stderr, "%s\n", err);
  FreeEXRErrorMessage(err);
}
```

**V2:**
```cpp
auto result = SomeFunction();
if (!result.success) {
  std::cerr << result.error_string();
}
```

### Return Values
**V1:** Integer error codes, output parameters
**V2:** Result<T> containing value or errors

### Memory Management
**V1:** Manual allocation/deallocation
**V2:** Automatic with RAII and std::vector

## Compatibility

- **C++ Standard**: C++11 or later
- **Platforms**: Linux, macOS, Windows
- **Compilers**: GCC 4.8+, Clang 3.4+, MSVC 2015+
- **Dependencies**: None (header-only)

## Current Status

✓ StreamReader implementation complete
✓ Reader class with error stack complete
✓ Result<T> and ErrorInfo complete
✓ ParseVersion() implemented and tested
✓ ParseHeader() partially implemented
⚠ LoadFromMemory() header parsing only (pixel data TODO)
⚠ Compression/decompression not yet ported to V2
⚠ Multipart files support incomplete

## Future Enhancements

- [ ] Complete pixel data loading
- [ ] Implement all compression formats
- [ ] Add write/save functionality
- [ ] Multipart file support
- [ ] Deep image support
- [ ] Async loading API
- [ ] Streaming reader for large files

## License

Same as TinyEXR: BSD-3-Clause

Copyright (c) 2025, Syoyo Fujita and many contributors.
