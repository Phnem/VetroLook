# TinyEXR V3 API - Modern C/C++ Interface

## Status: BETA

**Version:** 3.0.0

The V3 API is a modern, production-quality C API with C++17 wrapper, designed as a successor to the V1 API. It follows a Vulkan-style interface with opaque handles, command buffers, and async I/O support.

## Key Features

- **Pure C11/C17 Core**: No C++ dependencies in the C API
- **C++17 Wrapper**: RAII, `Result<T>`, range-based iteration
- **Vulkan-style API**: Command buffers, fences, explicit synchronization
- **Async/WASM-friendly**: Asyncify support, streaming I/O
- **Exception-free**: Compatible with `-fno-exceptions -fno-rtti`
- **Thread-safe error reporting**: Thread-local error storage with context

## Files

| File | Description |
|------|-------------|
| `tinyexr_c.h` | Pure C API header (opaque handles, enums, functions) |
| `tinyexr_c_impl.c` | C API implementation |
| `tinyexr_v3.hh` | C++17 wrapper (RAII, Result<T>, iterators) |
| `test/unit/tester-v3.cc` | Comprehensive test suite (41 tests) |

## Quick Start

### C++ API

```cpp
#include "tinyexr_v3.hh"

using namespace tinyexr::v3;

// Load EXR file
auto ctx = Context::create().value;
auto decoder = Decoder::from_file(ctx, "input.exr").value;
auto image = decoder.parse_header().value;

// Access metadata
auto part = image.get_part(0).value;
std::cout << "Size: " << part.width() << "x" << part.height() << "\n";
std::cout << "Channels: " << part.channel_count() << "\n";

// Iterate over tiles
for (auto tile : part.tiles(0)) {
    // Load tile at (tile.tile_x, tile.tile_y)
}

// Iterate over scanlines
for (int y : part.scanlines()) {
    // Load scanline block starting at y
}
```

### C API

```c
#include "tinyexr_c.h"

// Create context
ExrContext ctx = NULL;
ExrContextCreateInfo ctx_info = {0};
ctx_info.api_version = TINYEXR_C_API_VERSION;
exr_context_create(&ctx_info, &ctx);

// Create decoder from file
ExrDataSource source;
exr_data_source_from_file("input.exr", &source);

ExrDecoderCreateInfo dec_info = {0};
dec_info.source = source;

ExrDecoder decoder = NULL;
exr_decoder_create(ctx, &dec_info, &decoder);

// Parse header
ExrImage image = NULL;
exr_decoder_parse_header(decoder, &image);

// Get part info
ExrPart part;
exr_image_get_part(image, 0, &part);

ExrPartInfo info;
exr_part_get_info(part, &info);
printf("Size: %dx%d, Channels: %d\n", info.width, info.height, info.num_channels);

// Cleanup
exr_decoder_destroy(decoder);
exr_context_destroy(ctx);
```

## Implementation Status

### Reading (Decoder)

| Feature | Status | Notes |
|---------|--------|-------|
| Scanline images | ✅ Complete | Full pixel data loading |
| Tiled images | ✅ Complete | Including mipmap/ripmap |
| Multipart files | ✅ Complete | Up to 254 parts |
| Deep scanline | ✅ Complete | Full sample counts and pixel data loading |
| Deep tiled | ✅ Complete | Full sample counts and pixel data loading |
| Async I/O | ✅ Complete | Fetch callbacks, WASM Asyncify |
| Custom attributes | ✅ Complete | Full read support |

### Writing (Encoder)

| Feature | Status | Notes |
|---------|--------|-------|
| Scanline images | ✅ Complete | All pixel types |
| Tiled images | ✅ Complete | Including mipmap/ripmap |
| Multipart files | ⚠️ Partial | Single-part multipart works, true multi-part TODO |
| Deep scanline | ✅ Complete | ZIP compression |
| Deep tiled | ✅ Complete | ZIP compression |
| Custom attributes | ✅ Complete | int/float/string and generic |

### Compression Formats

| Format | Read | Write | Notes |
|--------|------|-------|-------|
| NONE | ✅ | ✅ | Uncompressed |
| RLE | ✅ | ✅ | Run-length encoding |
| ZIP | ✅ | ✅ | zlib (16 scanlines) |
| ZIPS | ✅ | ✅ | zlib (single scanline) |
| PIZ | ✅ | ✅ | Wavelet + Huffman |
| PXR24 | ✅ | ✅ | 24-bit lossy |
| B44 | ✅ | ✅ | 4x4 lossy blocks |
| B44A | ✅ | ✅ | B44 with alpha |
| DWAA | ❌ | ❌ | DCT lossy - not implemented |
| DWAB | ❌ | ❌ | DCT lossy - not implemented |

### Image Type Support

| Type | Detect | Read Header | Read Pixels | Write |
|------|--------|-------------|-------------|-------|
| Scanline | ✅ | ✅ | ✅ | ✅ |
| Tiled | ✅ | ✅ | ✅ | ✅ |
| Deep Scanline | ✅ | ✅ | ✅ | ✅ |
| Deep Tiled | ✅ | ✅ | ✅ | ✅ |
| Multipart | ✅ | ✅ | ✅ | ⚠️ |
| Mipmap | ✅ | ✅ | ✅ | ✅ |
| Ripmap | ✅ | ✅ | ✅ | ✅ |

## Missing Features (TODO)

### High Priority

1. **DWAA/DWAB Compression**
   - Not implemented (returns `EXR_ERROR_UNSUPPORTED_FORMAT`)
   - Complex DCT implementation required
   - Alternative: Convert files to ZIP/PIZ format

### Medium Priority

2. **True Multipart Writing**
   - Single-part multipart writing ✅ (writes name/type attributes, multipart flag)
   - True multi-part (multiple ExrWriteImage per file) needs coordination

### Low Priority

3. **Direct Chunk Compression/Decompression API**
   - `exr_decompress_chunk()` / `exr_compress_chunk()` ✅ Implemented
   - Supports NONE, RLE, ZIP/ZIPS decompression and compression
   - PIZ decompression only (compression requires channel info)

## Error Handling

### Error Codes (22 distinct codes)

```c
// Success states
EXR_SUCCESS           // Operation completed successfully
EXR_INCOMPLETE        // Operation partially completed
EXR_WOULD_BLOCK       // Would block, try again
EXR_SUSPENDED         // Suspended for async operation

// Validation errors
EXR_ERROR_INVALID_HANDLE
EXR_ERROR_INVALID_ARGUMENT
EXR_ERROR_INVALID_MAGIC
EXR_ERROR_INVALID_VERSION
EXR_ERROR_INVALID_DATA

// Resource errors
EXR_ERROR_OUT_OF_MEMORY
EXR_ERROR_BUFFER_TOO_SMALL
EXR_ERROR_OUT_OF_BOUNDS

// Format errors
EXR_ERROR_UNSUPPORTED_FORMAT
EXR_ERROR_COMPRESSION_FAILED
EXR_ERROR_DECOMPRESSION_FAILED

// I/O errors
EXR_ERROR_IO_ERROR
EXR_ERROR_FETCH_FAILED
EXR_ERROR_TIMEOUT
EXR_ERROR_CANCELLED
```

### Error Context

```cpp
// C++ API
auto result = decoder.parse_header();
if (!result) {
    for (const auto& err : result.errors) {
        std::cerr << err.to_string() << "\n";
        // Output: "Invalid data: Unsupported compression type [parsing header] at byte 1234"
    }
}

// C API
ExrErrorInfo error;
if (exr_context_get_last_error(ctx, &error) == EXR_SUCCESS) {
    printf("Error: %s at byte %llu\n", error.message, error.byte_position);
}
```

## Building

### Requirements

- C11/C17 compiler (for C API)
- C++17 compiler (for C++ wrapper)
- miniz or zlib (for ZIP/ZIPS compression)

### Compile Test Suite

```bash
# With ASAN
g++ -std=c++17 -I. -I deps/miniz -DTINYEXR_IMPLEMENTATION \
    -fsanitize=address -g -O0 \
    -o tester-v3 test/unit/tester-v3.cc tinyexr_c_impl.c deps/miniz/miniz.c

# Run tests
./tester-v3
```

### Compile Flags

| Flag | Description |
|------|-------------|
| `TINYEXR_V3_USE_MINIZ` | Use miniz for ZIP compression (default) |
| `TINYEXR_V3_HAS_DEFLATE` | Enable optimized deflate decompression |
| `TINYEXR_V3_HAS_PIZ` | Enable PIZ compression (requires V2 impl) |
| `TINYEXR_ENABLE_SIMD` | Enable SIMD optimizations |

## SIMD Optimization

The V3 API detects and uses available SIMD instructions:

```cpp
// C++ API
std::cout << tinyexr::v3::simd_info() << "\n";
// Output: "AVX2+BMI2" or "SSE4.1" or "NEON" etc.

// C API
char info[256];
exr_get_simd_info(info, sizeof(info));
```

Supported SIMD features:
- SSE2, SSE4.1, AVX, AVX2, AVX512
- NEON (ARM)
- BMI1, BMI2 (bit manipulation)

## Thread Safety

- **Context**: Thread-safe error reporting with thread-local storage
- **Decoder**: Single-threaded per instance; multiple decoders can run in parallel
- **Encoder**: Single-threaded per instance; multiple encoders can run in parallel
- **Allocator**: Custom allocator must be thread-safe if shared

## Migration from V1

### Key Differences

| Aspect | V1 API | V3 API |
|--------|--------|--------|
| Error handling | Error codes + strings | `Result<T>` with error stack |
| Memory | Manual management | RAII (C++), explicit destroy (C) |
| I/O | Synchronous only | Async with callbacks |
| Threading | OpenMP | Command buffers, fences |
| Handles | Direct pointers | Opaque handles |

### Migration Example

**V1:**
```cpp
const char* err = nullptr;
float* rgba = nullptr;
int width, height;

int ret = LoadEXR(&rgba, &width, &height, "input.exr", &err);
if (ret != TINYEXR_SUCCESS) {
    fprintf(stderr, "Error: %s\n", err);
    FreeEXRErrorMessage(err);
    return -1;
}
// Use rgba...
free(rgba);
```

**V3 (C++):**
```cpp
auto ctx = tinyexr::v3::Context::create().value;
auto decoder = tinyexr::v3::Decoder::from_file(ctx, "input.exr");
if (!decoder) {
    std::cerr << decoder.error_string() << "\n";
    return -1;
}
auto image = decoder.value.parse_header();
// Use image...
// Automatic cleanup via RAII
```

## Test Results

```
TinyEXR V3 API Tests
====================

[C API Tests]
  [TEST] c_api_version... PASSED
  [TEST] c_api_context_create_destroy... PASSED
  [TEST] c_api_decoder_create_destroy... PASSED
  [TEST] c_api_parse_real_file... [660x440, 4 ch, comp=3] PASSED
  [TEST] c_api_verify_zip_vs_v1... [660x440x4 verified] PASSED
  [TEST] c_api_verify_piz_vs_v1... [420x32x3 verified] PASSED
  [TEST] c_api_verify_pxr24_vs_v1... [660x440x4 verified] PASSED
  [TEST] c_api_verify_b44_vs_v1... [660x440x4 verified] PASSED
  ...

[C++ Wrapper Tests]
  [TEST] cpp_result_ok... PASSED
  [TEST] cpp_result_map... PASSED
  [TEST] cpp_decoder_from_memory... PASSED
  ...

====================
Results: 41 passed, 0 failed
```

## License

BSD-3-Clause (same as TinyEXR)

## Version History

- **3.0.0** (2025): Initial beta release
  - Complete scanline/tiled reading and writing
  - All compression formats except DWAA/DWAB
  - Async I/O support
  - C++17 wrapper
