# TinyEXR SIMD Optimizations

This document describes the SIMD optimization layer for TinyEXR V2 API.

## Overview

The SIMD optimization layer (`tinyexr_simd.hh`) provides hardware-accelerated routines for:
- FP16 (half-precision) to FP32 (single-precision) conversion
- FP32 to FP16 conversion
- Memory operations (copy, interleave, deinterleave)
- Byte reordering for compression preprocessing

## Supported Architectures

| Architecture | Instruction Set | Detection Macro |
|--------------|-----------------|-----------------|
| x86/x64 | SSE2 | `TINYEXR_SIMD_SSE2` |
| x86/x64 | SSE4.1 | `TINYEXR_SIMD_SSE41` |
| x86/x64 | AVX | `TINYEXR_SIMD_AVX` |
| x86/x64 | AVX2 | `TINYEXR_SIMD_AVX2` |
| x86/x64 | F16C | `TINYEXR_SIMD_F16C` |
| x86/x64 | AVX-512F | `TINYEXR_SIMD_AVX512F` |
| ARM | NEON | `TINYEXR_SIMD_NEON` |
| ARM | NEON + FP16 | `TINYEXR_SIMD_NEON_FP16` |
| ARM | SVE | `TINYEXR_SIMD_SVE` |
| ARM | SVE2 | `TINYEXR_SIMD_SVE2` |
| ARM A64FX | SVE 512-bit | `TINYEXR_SIMD_A64FX` |

## How to Enable

Define `TINYEXR_ENABLE_SIMD=1` before including the headers:

```cpp
#define TINYEXR_ENABLE_SIMD 1
#include "tinyexr_v2.hh"
```

Or pass it as a compiler flag:

```bash
# x86_64 with AVX2 and F16C
g++ -O2 -mavx2 -mf16c -DTINYEXR_ENABLE_SIMD=1 your_code.cc

# ARM with NEON and FP16
g++ -O2 -march=armv8.2-a+fp16 -DTINYEXR_ENABLE_SIMD=1 your_code.cc

# ARM SVE
g++ -O2 -march=armv8-a+sve -DTINYEXR_ENABLE_SIMD=1 your_code.cc

# ARM A64FX (Fugaku)
g++ -O2 -march=armv8.2-a+sve -DTINYEXR_ENABLE_SIMD=1 -DTINYEXR_A64FX_OPTIMIZED=1 your_code.cc
```

## API Functions

### V2 API Integration

The V2 API automatically uses SIMD when enabled:

```cpp
#define TINYEXR_ENABLE_SIMD 1
#include "tinyexr_v2.hh"
#include "tinyexr_v2_impl.hh"

// Check SIMD status
printf("SIMD: %s\n", tinyexr::v2::GetSIMDInfo());
printf("Enabled: %s\n", tinyexr::v2::IsSIMDEnabled() ? "yes" : "no");

// Convert FP16 to FP32 (SIMD-accelerated)
std::vector<uint16_t> half_data(1024);
std::vector<float> float_data(1024);
tinyexr::v2::ConvertHalfToFloat(half_data.data(), float_data.data(), 1024);

// Convert FP32 to FP16
tinyexr::v2::ConvertFloatToHalf(float_data.data(), half_data.data(), 1024);

// Interleave separate channels into RGBA
std::vector<float> r(256), g(256), b(256), a(256);
std::vector<float> rgba(256 * 4);
tinyexr::v2::InterleaveRGBA(r.data(), g.data(), b.data(), a.data(), rgba.data(), 256);

// Deinterleave RGBA into separate channels
tinyexr::v2::DeinterleaveRGBA(rgba.data(), r.data(), g.data(), b.data(), a.data(), 256);
```

### Low-Level SIMD Functions

For advanced use cases, you can use the low-level SIMD functions directly:

```cpp
#define TINYEXR_ENABLE_SIMD 1
#include "tinyexr_simd.hh"

using namespace tinyexr::simd;

// Get capabilities
const SIMDCapabilities& caps = get_capabilities();
printf("F16C: %s\n", caps.f16c ? "yes" : "no");
printf("NEON: %s\n", caps.neon ? "yes" : "no");
printf("SVE Length: %u bits\n", caps.sve_vector_length);

// Batch FP16 <-> FP32 conversion
half_to_float_batch(half_data, float_data, count);
float_to_half_batch(float_data, half_data, count);

// SIMD memory copy
memcpy_simd(dst, src, bytes);

// Interleave/Deinterleave
interleave_rgba_float(r, g, b, a, rgba, pixel_count);
deinterleave_rgba_float(rgba, r, g, b, a, pixel_count);

// Byte reordering (for compression)
reorder_bytes_for_compression(src, dst, count);
unreorder_bytes_after_decompression(src, dst, count);
```

## Performance

Typical performance improvements with SIMD (measured on x86_64 with AVX2+F16C):

| Operation | Scalar | SIMD | Speedup |
|-----------|--------|------|---------|
| FP16->FP32 (1M values) | ~100ms | ~10ms | ~10x |
| FP32->FP16 (1M values) | ~150ms | ~11ms | ~14x |
| memcpy (16MB) | baseline | ~same | ~1x (compiler optimized) |
| Interleave RGBA | ~50ms | ~15ms | ~3x |

Note: Actual performance varies by CPU and workload.

## Architecture-Specific Notes

### x86/x64

- **F16C**: Provides native hardware FP16 conversion instructions (`vcvtph2ps`, `vcvtps2ph`)
- **AVX2**: Enables 256-bit operations for faster memory processing
- **AVX-512**: Enables 512-bit operations for maximum throughput

### ARM NEON

- **FP16 Extension** (`armv8.2-a+fp16`): Required for native FP16 conversion
- Without FP16 extension, falls back to scalar conversion
- NEON has excellent native interleave/deinterleave support (`vld4q`, `vst4q`)

### ARM SVE

- **Variable-length vectors**: SVE adapts to the hardware vector length
- **A64FX**: Fujitsu's A64FX processor has 512-bit SVE vectors
- Runtime detection of vector length via `svcntb()`

### ARM A64FX (Fugaku)

- Optimized for 512-bit SVE operations
- Processes 32 half-precision values or 16 single-precision values per vector operation
- Define `TINYEXR_A64FX_OPTIMIZED=1` for A64FX-specific optimizations

## Building Tests

```bash
# Basic (scalar fallback)
g++ -std=c++11 -O2 -o test_simd test_simd.cc

# x86_64 with AVX2+F16C
g++ -std=c++11 -O2 -mavx2 -mf16c -DTINYEXR_ENABLE_SIMD=1 -o test_simd test_simd.cc

# Run tests
./test_simd

# Run benchmarks
./test_simd --bench
```

## File Structure

```
tinyexr_simd.hh      # SIMD optimization layer (header-only)
tinyexr_v2.hh        # V2 API with SIMD integration
test_simd.cc         # SIMD test suite and benchmarks
test/unit/tester-v2.cc  # Unit tests including SIMD tests
```

## Compatibility

- **C++11 or later**: Required for SIMD intrinsics headers
- **Header-only**: No separate compilation required
- **Exception-free**: Compatible with `-fno-exceptions`
- **RTTI-free**: Compatible with `-fno-rtti`

## Fallback Behavior

When SIMD is not available (either not enabled or not supported by the CPU), all functions fall back to scalar implementations automatically. This ensures portability across all platforms.
