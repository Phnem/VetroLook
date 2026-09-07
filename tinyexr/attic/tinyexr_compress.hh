// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025, Syoyo Fujita and many contributors.
// All rights reserved.
//
// TinyEXR Compression Encoding Module
//
// Part of TinyEXR V2 API (EXPERIMENTAL)
//
// Provides SIMD-optimized compression encoding for EXR files:
// - Byte reordering for compression
// - Delta predictor application
// - RLE compression encoding
// - ZIP/ZIPS compression encoding (using miniz/zlib)
// - PIZ compression encoding with Huffman coding
//
// Usage:
//   #define TINYEXR_ENABLE_SIMD 1
//   #include "tinyexr_compress.hh"

#ifndef TINYEXR_COMPRESS_HH_
#define TINYEXR_COMPRESS_HH_

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>
#include <algorithm>

// Include SIMD header for optimizations
#if defined(TINYEXR_ENABLE_SIMD) && TINYEXR_ENABLE_SIMD
#include "tinyexr_simd.hh"
#endif

// Include miniz or zlib for deflate compression
#if defined(TINYEXR_USE_MINIZ) && TINYEXR_USE_MINIZ
#include "miniz.h"
#define TINYEXR_COMPRESS_HAS_ZLIB 1
#elif defined(TINYEXR_USE_ZLIB) && TINYEXR_USE_ZLIB
#include <zlib.h>
#define TINYEXR_COMPRESS_HAS_ZLIB 1
#else
// Default to miniz if available
#if defined(__has_include)
#if __has_include("miniz.h")
#include "miniz.h"
#define TINYEXR_COMPRESS_HAS_ZLIB 1
#define TINYEXR_USE_MINIZ 1
#elif __has_include(<zlib.h>)
#include <zlib.h>
#define TINYEXR_COMPRESS_HAS_ZLIB 1
#define TINYEXR_USE_ZLIB 1
#endif
#endif
#endif

#ifndef TINYEXR_COMPRESS_HAS_ZLIB
#define TINYEXR_COMPRESS_HAS_ZLIB 0
#endif

// ============================================================================
// Compiler Hints
// ============================================================================

#ifndef TINYEXR_COMPRESS_LIKELY
#if defined(__GNUC__) || defined(__clang__)
#define TINYEXR_COMPRESS_LIKELY(x) __builtin_expect(!!(x), 1)
#define TINYEXR_COMPRESS_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define TINYEXR_COMPRESS_ALWAYS_INLINE __attribute__((always_inline)) inline
#define TINYEXR_COMPRESS_PREFETCH(addr) __builtin_prefetch(addr)
#elif defined(_MSC_VER)
#include <intrin.h>
#define TINYEXR_COMPRESS_LIKELY(x) (x)
#define TINYEXR_COMPRESS_UNLIKELY(x) (x)
#define TINYEXR_COMPRESS_ALWAYS_INLINE __forceinline
#define TINYEXR_COMPRESS_PREFETCH(addr) _mm_prefetch((const char*)(addr), _MM_HINT_T0)
#else
#define TINYEXR_COMPRESS_LIKELY(x) (x)
#define TINYEXR_COMPRESS_UNLIKELY(x) (x)
#define TINYEXR_COMPRESS_ALWAYS_INLINE inline
#define TINYEXR_COMPRESS_PREFETCH(addr) ((void)0)
#endif
#endif

namespace tinyexr {
namespace compress {

// ============================================================================
// Compression Result Structure
// ============================================================================

struct CompressResult {
  bool success;
  size_t compressed_size;
  std::string error_message;

  CompressResult() : success(false), compressed_size(0) {}

  static CompressResult ok(size_t size) {
    CompressResult r;
    r.success = true;
    r.compressed_size = size;
    return r;
  }

  static CompressResult error(const std::string& msg) {
    CompressResult r;
    r.success = false;
    r.error_message = msg;
    return r;
  }
};

// ============================================================================
// Byte Reordering for Compression
// ============================================================================
// EXR uses interleaved byte ordering for better compression ratios.
// Before compression: reorder bytes so that bytes at similar positions
// are grouped together (improves entropy coding efficiency).

// Reorder bytes before compression (opposite of unreorder)
// Input: natural byte order [B0_0 B0_1 B1_0 B1_1 B2_0 B2_1 ...]
// Output: reordered [B0_0 B1_0 B2_0 ... B0_1 B1_1 B2_1 ...]
// (All first bytes together, then all second bytes, etc.)
TINYEXR_COMPRESS_ALWAYS_INLINE void reorder_bytes_for_compression(
    const uint8_t* src, uint8_t* dst, size_t size) {
  if (size == 0) return;

  const size_t half = (size + 1) / 2;

#if defined(TINYEXR_ENABLE_SIMD) && TINYEXR_ENABLE_SIMD && TINYEXR_SIMD_SSE2
  // SSE2 optimized version: deinterleave bytes
  size_t i = 0;
  uint8_t* dst1 = dst;
  uint8_t* dst2 = dst + half;

  // Process 32 bytes at a time (16 pairs)
  for (; i + 32 <= size; i += 32) {
    __m128i v0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i));
    __m128i v1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + i + 16));

    // Deinterleave: extract even and odd bytes
    // Shuffle mask for even bytes: 0, 2, 4, 6, 8, 10, 12, 14, -, -, -, -, -, -, -, -
    // Shuffle mask for odd bytes:  1, 3, 5, 7, 9, 11, 13, 15, -, -, -, -, -, -, -, -
    const __m128i shuf_even = _mm_setr_epi8(0, 2, 4, 6, 8, 10, 12, 14,
                                            -1, -1, -1, -1, -1, -1, -1, -1);
    const __m128i shuf_odd = _mm_setr_epi8(1, 3, 5, 7, 9, 11, 13, 15,
                                           -1, -1, -1, -1, -1, -1, -1, -1);

    __m128i even0 = _mm_shuffle_epi8(v0, shuf_even);
    __m128i odd0 = _mm_shuffle_epi8(v0, shuf_odd);
    __m128i even1 = _mm_shuffle_epi8(v1, shuf_even);
    __m128i odd1 = _mm_shuffle_epi8(v1, shuf_odd);

    // Combine: even0_lo | even1_lo, odd0_lo | odd1_lo
    __m128i evens = _mm_unpacklo_epi64(even0, even1);
    __m128i odds = _mm_unpacklo_epi64(odd0, odd1);

    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst1), evens);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(dst2), odds);
    dst1 += 16;
    dst2 += 16;
  }

  // Handle remaining bytes
  for (; i < size; i += 2) {
    *dst1++ = src[i];
    if (i + 1 < size) {
      *dst2++ = src[i + 1];
    }
  }

#elif defined(TINYEXR_ENABLE_SIMD) && TINYEXR_ENABLE_SIMD && TINYEXR_SIMD_NEON
  // NEON optimized version
  size_t i = 0;
  uint8_t* dst1 = dst;
  uint8_t* dst2 = dst + half;

  // Process 32 bytes at a time using vuzp
  for (; i + 32 <= size; i += 32) {
    uint8x16x2_t v0 = vld2q_u8(src + i);
    vst1q_u8(dst1, v0.val[0]);
    vst1q_u8(dst2, v0.val[1]);
    dst1 += 16;
    dst2 += 16;
  }

  // Handle remaining
  for (; i < size; i += 2) {
    *dst1++ = src[i];
    if (i + 1 < size) {
      *dst2++ = src[i + 1];
    }
  }

#else
  // Scalar fallback
  uint8_t* dst1 = dst;
  uint8_t* dst2 = dst + half;

  for (size_t i = 0; i < size; i += 2) {
    *dst1++ = src[i];
    if (i + 1 < size) {
      *dst2++ = src[i + 1];
    }
  }
#endif
}

// ============================================================================
// Delta Predictor for Compression
// ============================================================================
// Apply delta encoding: each byte becomes (current - previous + 128)
// This reduces entropy for gradual changes in data.

TINYEXR_COMPRESS_ALWAYS_INLINE void apply_delta_predictor_encode(
    uint8_t* data, size_t size) {
  if (size <= 1) return;

#if defined(TINYEXR_ENABLE_SIMD) && TINYEXR_ENABLE_SIMD && TINYEXR_SIMD_SSE2
  // Process in reverse for delta encoding (can't easily vectorize due to dependency)
  // We'll use a scalar approach but with good cache behavior
  size_t i = size - 1;

  // Prefetch
  TINYEXR_COMPRESS_PREFETCH(data + size - 64);

  while (i > 0) {
    int d = static_cast<int>(data[i]) - static_cast<int>(data[i - 1]) + 128;
    data[i] = static_cast<uint8_t>(d);
    i--;
  }
  // First byte stays the same (it's the reference)

#else
  // Scalar version - process in reverse
  for (size_t i = size - 1; i > 0; i--) {
    int d = static_cast<int>(data[i]) - static_cast<int>(data[i - 1]) + 128;
    data[i] = static_cast<uint8_t>(d);
  }
#endif
}

// Combined reorder + predictor for compression preparation
// This is the reverse of what decompression does
inline void prepare_for_compression(const uint8_t* src, uint8_t* dst, size_t size) {
  // Step 1: Reorder bytes
  reorder_bytes_for_compression(src, dst, size);

  // Step 2: Apply delta predictor
  apply_delta_predictor_encode(dst, size);
}

// ============================================================================
// RLE Compression Encoder
// ============================================================================
// RLE format:
// - If count < 0: literal run of (-count) bytes follows
// - If count >= 0: repeat next byte (count + 1) times

// Maximum RLE run length
static const int RLE_MAX_RUN = 127;
static const int RLE_MIN_RUN = 3;  // Minimum run to encode as repeat

// RLE compress with SIMD-accelerated run detection
inline CompressResult rle_compress(const uint8_t* src, size_t src_size,
                                    uint8_t* dst, size_t dst_capacity) {
  if (!src || !dst || src_size == 0) {
    return CompressResult::error("Invalid input parameters");
  }

  if (dst_capacity < src_size + src_size / 128 + 1) {
    // Ensure enough space for worst case
  }

  size_t src_pos = 0;
  size_t dst_pos = 0;
  size_t literal_start = 0;
  size_t literal_count = 0;

  // Helper to flush literal run
  auto flush_literals = [&]() {
    while (literal_count > 0) {
      int count = static_cast<int>(std::min(literal_count, static_cast<size_t>(RLE_MAX_RUN)));
      if (dst_pos + 1 + count > dst_capacity) {
        return false;
      }
      dst[dst_pos++] = static_cast<uint8_t>(-count);  // Negative = literal
      std::memcpy(dst + dst_pos, src + literal_start, count);
      dst_pos += count;
      literal_start += count;
      literal_count -= count;
    }
    return true;
  };

  while (src_pos < src_size) {
    // Look for a run
    uint8_t current = src[src_pos];
    size_t run_length = 1;

#if defined(TINYEXR_ENABLE_SIMD) && TINYEXR_ENABLE_SIMD && TINYEXR_SIMD_SSE2
    // SIMD run detection: compare 16 bytes at once
    if (src_pos + 16 <= src_size) {
      __m128i v_current = _mm_set1_epi8(static_cast<char>(current));
      __m128i v_data = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + src_pos));
      __m128i v_cmp = _mm_cmpeq_epi8(v_data, v_current);
      int mask = _mm_movemask_epi8(v_cmp);

      // Count trailing ones in mask (consecutive matches)
      if (mask == 0xFFFF) {
        run_length = 16;
        // Check further
        size_t check_pos = src_pos + 16;
        while (check_pos + 16 <= src_size) {
          v_data = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + check_pos));
          v_cmp = _mm_cmpeq_epi8(v_data, v_current);
          mask = _mm_movemask_epi8(v_cmp);
          if (mask == 0xFFFF) {
            run_length += 16;
            check_pos += 16;
            if (run_length >= RLE_MAX_RUN) break;
          } else {
            // Count trailing ones
            int trailing = (mask == 0) ? 0 : __builtin_ctz(~mask);
            run_length += trailing;
            break;
          }
        }
        // Check remaining bytes
        if (run_length < RLE_MAX_RUN) {
          size_t check_end = std::min(src_size, src_pos + RLE_MAX_RUN + 1);
          for (size_t i = src_pos + run_length; i < check_end; i++) {
            if (src[i] == current) {
              run_length++;
            } else {
              break;
            }
          }
        }
      } else if (mask != 0) {
        // Count leading ones
        int trailing = __builtin_ctz(~mask);
        run_length = trailing;
      }
    } else
#endif
    {
      // Scalar run detection
      size_t max_run = std::min(src_size - src_pos, static_cast<size_t>(RLE_MAX_RUN + 1));
      while (run_length < max_run && src[src_pos + run_length] == current) {
        run_length++;
      }
    }

    if (run_length >= RLE_MIN_RUN) {
      // Flush any pending literals
      if (!flush_literals()) {
        return CompressResult::error("Output buffer overflow");
      }

      // Write run
      int count = static_cast<int>(std::min(run_length, static_cast<size_t>(RLE_MAX_RUN + 1)));
      if (dst_pos + 2 > dst_capacity) {
        return CompressResult::error("Output buffer overflow");
      }
      dst[dst_pos++] = static_cast<uint8_t>(count - 1);  // Positive = run
      dst[dst_pos++] = current;
      src_pos += count;
      literal_start = src_pos;
    } else {
      // Add to literal run
      literal_count++;
      src_pos++;
    }
  }

  // Flush remaining literals
  if (!flush_literals()) {
    return CompressResult::error("Output buffer overflow");
  }

  return CompressResult::ok(dst_pos);
}

// ============================================================================
// ZIP/ZIPS Compression Encoder
// ============================================================================

#if TINYEXR_COMPRESS_HAS_ZLIB

// Compress using zlib/miniz deflate
inline CompressResult zip_compress(const uint8_t* src, size_t src_size,
                                    uint8_t* dst, size_t dst_capacity,
                                    int level = 6) {
  if (!src || !dst || src_size == 0) {
    return CompressResult::error("Invalid input parameters");
  }

#if defined(TINYEXR_USE_MINIZ) && TINYEXR_USE_MINIZ
  mz_ulong compressed_size = static_cast<mz_ulong>(dst_capacity);
  int ret = mz_compress2(dst, &compressed_size, src, static_cast<mz_ulong>(src_size), level);
  if (ret != MZ_OK) {
    return CompressResult::error("miniz compression failed");
  }
  return CompressResult::ok(static_cast<size_t>(compressed_size));
#else
  uLongf compressed_size = static_cast<uLongf>(dst_capacity);
  int ret = compress2(dst, &compressed_size, src, static_cast<uLong>(src_size), level);
  if (ret != Z_OK) {
    return CompressResult::error("zlib compression failed");
  }
  return CompressResult::ok(static_cast<size_t>(compressed_size));
#endif
}

// Get maximum compressed size for given input
inline size_t zip_compress_bound(size_t src_size) {
#if defined(TINYEXR_USE_MINIZ) && TINYEXR_USE_MINIZ
  return static_cast<size_t>(mz_compressBound(static_cast<mz_ulong>(src_size)));
#else
  return static_cast<size_t>(compressBound(static_cast<uLong>(src_size)));
#endif
}

#else

// Stub when no zlib available
inline CompressResult zip_compress(const uint8_t*, size_t,
                                    uint8_t*, size_t, int = 6) {
  return CompressResult::error("ZIP compression not available (no zlib/miniz)");
}

inline size_t zip_compress_bound(size_t src_size) {
  return src_size + src_size / 1000 + 12 + 6;  // Approximate
}

#endif

// Full compression pipeline for ZIP: reorder + predictor + deflate
inline CompressResult compress_zip(const uint8_t* src, size_t src_size,
                                    uint8_t* dst, size_t dst_capacity,
                                    std::vector<uint8_t>& work_buffer,
                                    int level = 6) {
  // Prepare work buffer
  if (work_buffer.size() < src_size) {
    work_buffer.resize(src_size);
  }

  // Apply reorder and predictor
  prepare_for_compression(src, work_buffer.data(), src_size);

  // Compress
  return zip_compress(work_buffer.data(), src_size, dst, dst_capacity, level);
}

// Full compression pipeline for RLE: reorder + predictor + RLE
inline CompressResult compress_rle(const uint8_t* src, size_t src_size,
                                    uint8_t* dst, size_t dst_capacity,
                                    std::vector<uint8_t>& work_buffer) {
  // Prepare work buffer
  if (work_buffer.size() < src_size) {
    work_buffer.resize(src_size);
  }

  // Apply reorder and predictor
  prepare_for_compression(src, work_buffer.data(), src_size);

  // RLE compress
  return rle_compress(work_buffer.data(), src_size, dst, dst_capacity);
}

// ============================================================================
// PIZ Compression Encoder
// ============================================================================

// PIZ constants
static const int PIZ_BITMAP_SIZE = 8192;
static const int PIZ_USHORT_RANGE = 65536;
static const int HUF_ENCBITS = 16;
static const int HUF_ENCSIZE = (1 << HUF_ENCBITS) + 1;

// Wavelet encoding functions (14-bit and 16-bit)
TINYEXR_COMPRESS_ALWAYS_INLINE void wenc14(uint16_t a, uint16_t b,
                                            uint16_t& l, uint16_t& h) {
  int16_t as = static_cast<int16_t>(a);
  int16_t bs = static_cast<int16_t>(b);
  int16_t ms = (as + bs) >> 1;
  int16_t ds = as - bs;
  l = static_cast<uint16_t>(ms);
  h = static_cast<uint16_t>(ds);
}

static const int NBITS = 16;
static const int A_OFFSET = 1 << (NBITS - 1);
static const int M_OFFSET = 1 << (NBITS - 1);
static const int MOD_MASK = (1 << NBITS) - 1;

TINYEXR_COMPRESS_ALWAYS_INLINE void wenc16(uint16_t a, uint16_t b,
                                            uint16_t& l, uint16_t& h) {
  int ao = (a + A_OFFSET) & MOD_MASK;
  int m = ((ao + b) >> 1);
  int d = ao - b;
  if (d < 0) m = (m + M_OFFSET) & MOD_MASK;
  d &= MOD_MASK;
  l = static_cast<uint16_t>(m);
  h = static_cast<uint16_t>(d);
}

// 2D Wavelet encoding
inline void wav2Encode(uint16_t* in, int nx, int ox, int ny, int oy, uint16_t mx) {
  bool w14 = (mx < (1 << 14));
  int n = (nx > ny) ? ny : nx;
  int p = 1;
  int p2 = 2;

  while (p2 <= n) {
    uint16_t* py = in;
    uint16_t* ey = in + oy * (ny - p2);
    int oy1 = oy * p;
    int oy2 = oy * p2;
    int ox1 = ox * p;
    int ox2 = ox * p2;
    uint16_t i00, i01, i10, i11;

    for (; py <= ey; py += oy2) {
      uint16_t* px = py;
      uint16_t* ex = py + ox * (nx - p2);

      for (; px <= ex; px += ox2) {
        uint16_t* p01 = px + ox1;
        uint16_t* p10 = px + oy1;
        uint16_t* p11 = p10 + ox1;

        if (w14) {
          wenc14(*px, *p01, i00, i01);
          wenc14(*p10, *p11, i10, i11);
          wenc14(i00, i10, *px, *p10);
          wenc14(i01, i11, *p01, *p11);
        } else {
          wenc16(*px, *p01, i00, i01);
          wenc16(*p10, *p11, i10, i11);
          wenc16(i00, i10, *px, *p10);
          wenc16(i01, i11, *p01, *p11);
        }
      }

      if (nx & p) {
        uint16_t* p10 = px + oy1;
        if (w14)
          wenc14(*px, *p10, i00, *p10);
        else
          wenc16(*px, *p10, i00, *p10);
        *px = i00;
      }
    }

    if (ny & p) {
      uint16_t* px = py;
      uint16_t* ex = py + ox * (nx - p2);

      for (; px <= ex; px += ox2) {
        uint16_t* p01 = px + ox1;
        if (w14)
          wenc14(*px, *p01, i00, *p01);
        else
          wenc16(*px, *p01, i00, *p01);
        *px = i00;
      }
    }

    p = p2;
    p2 <<= 1;
  }
}

// Build bitmap from data
inline void bitmapFromData(const uint16_t* data, int nData,
                           uint8_t* bitmap,
                           uint16_t& minNonZero, uint16_t& maxNonZero) {
  std::memset(bitmap, 0, PIZ_BITMAP_SIZE);

  for (int i = 0; i < nData; ++i) {
    bitmap[data[i] >> 3] |= (1 << (data[i] & 7));
  }

  bitmap[0] &= ~1;

  minNonZero = PIZ_BITMAP_SIZE - 1;
  maxNonZero = 0;

  for (int i = 0; i < PIZ_BITMAP_SIZE; ++i) {
    if (bitmap[i]) {
      if (minNonZero > i) minNonZero = static_cast<uint16_t>(i);
      if (maxNonZero < i) maxNonZero = static_cast<uint16_t>(i);
    }
  }
}

// Build forward LUT for PIZ
inline uint16_t forwardLutFromBitmap(const uint8_t* bitmap, uint16_t* lut) {
  int k = 0;
  for (int i = 0; i < PIZ_USHORT_RANGE; ++i) {
    if ((i == 0) || (bitmap[i >> 3] & (1 << (i & 7))))
      lut[i] = static_cast<uint16_t>(k++);
    else
      lut[i] = 0;
  }
  return static_cast<uint16_t>(k - 1);
}

// Apply LUT to data
inline void applyLut(const uint16_t* lut, uint16_t* data, int nData) {
  for (int i = 0; i < nData; ++i) {
    data[i] = lut[data[i]];
  }
}

// ============================================================================
// Huffman Encoding for PIZ
// ============================================================================

// Count code frequencies
inline void countFrequencies(const uint16_t* data, int nData, int64_t* freq) {
  std::memset(freq, 0, sizeof(int64_t) * HUF_ENCSIZE);
  for (int i = 0; i < nData; ++i) {
    freq[data[i]]++;
  }
}

// Build heap for Huffman tree construction
struct HufHeapEntry {
  int64_t freq;
  int node;
};

inline void heapPush(std::vector<HufHeapEntry>& heap, int64_t freq, int node) {
  HufHeapEntry e = {freq, node};
  heap.push_back(e);
  size_t i = heap.size() - 1;
  while (i > 0) {
    size_t parent = (i - 1) / 2;
    if (heap[parent].freq <= heap[i].freq) break;
    std::swap(heap[parent], heap[i]);
    i = parent;
  }
}

inline HufHeapEntry heapPop(std::vector<HufHeapEntry>& heap) {
  HufHeapEntry result = heap[0];
  heap[0] = heap.back();
  heap.pop_back();
  if (!heap.empty()) {
    size_t i = 0;
    while (true) {
      size_t left = 2 * i + 1;
      size_t right = 2 * i + 2;
      size_t smallest = i;
      if (left < heap.size() && heap[left].freq < heap[smallest].freq)
        smallest = left;
      if (right < heap.size() && heap[right].freq < heap[smallest].freq)
        smallest = right;
      if (smallest == i) break;
      std::swap(heap[i], heap[smallest]);
      i = smallest;
    }
  }
  return result;
}

// Build Huffman encoding table
inline void hufBuildEncTable(const int64_t* freq, int* im, int* iM, int64_t* hcode) {
  // Find min/max symbols with non-zero frequency
  *im = 0;
  while (*im < HUF_ENCSIZE && freq[*im] == 0) (*im)++;

  *iM = HUF_ENCSIZE - 1;
  while (*iM >= 0 && freq[*iM] == 0) (*iM)--;

  if (*im > *iM) {
    *im = 0;
    *iM = 0;
    return;
  }

  // Initialize code lengths to 0
  std::memset(hcode, 0, sizeof(int64_t) * HUF_ENCSIZE);

  // Count non-zero frequencies
  int nSymbols = 0;
  for (int i = *im; i <= *iM; ++i) {
    if (freq[i] > 0) nSymbols++;
  }

  if (nSymbols == 1) {
    // Single symbol - use length 1
    for (int i = *im; i <= *iM; ++i) {
      if (freq[i] > 0) {
        hcode[i] = 1;
        break;
      }
    }
    return;
  }

  // Build Huffman tree using heap
  std::vector<HufHeapEntry> heap;
  std::vector<int> parent(HUF_ENCSIZE * 2, -1);
  int nextNode = HUF_ENCSIZE;

  for (int i = *im; i <= *iM; ++i) {
    if (freq[i] > 0) {
      heapPush(heap, freq[i], i);
    }
  }

  while (heap.size() > 1) {
    HufHeapEntry e1 = heapPop(heap);
    HufHeapEntry e2 = heapPop(heap);
    parent[e1.node] = nextNode;
    parent[e2.node] = nextNode;
    heapPush(heap, e1.freq + e2.freq, nextNode);
    nextNode++;
  }

  // Calculate code lengths by traversing tree
  for (int i = *im; i <= *iM; ++i) {
    if (freq[i] > 0) {
      int len = 0;
      int node = i;
      while (parent[node] >= 0) {
        len++;
        node = parent[node];
      }
      // Limit length to 58 bits
      if (len > 58) len = 58;
      hcode[i] = len;
    }
  }
}

// Build canonical code table
inline void hufCanonicalCodeTable(int64_t* hcode) {
  int64_t n[59] = {0};

  for (int i = 0; i < HUF_ENCSIZE; ++i) {
    if (hcode[i] >= 0 && hcode[i] <= 58) {
      n[hcode[i]]++;
    }
  }

  int64_t c = 0;
  for (int i = 58; i > 0; --i) {
    int64_t nc = ((c + n[i]) >> 1);
    n[i] = c;
    c = nc;
  }

  for (int i = 0; i < HUF_ENCSIZE; ++i) {
    int l = static_cast<int>(hcode[i]);
    if (l > 0 && l <= 58) {
      hcode[i] = l | (n[l] << 6);
      n[l]++;
    }
  }
}

// Pack encoding table into compressed format
inline size_t hufPackEncTable(const int64_t* hcode, int im, int iM,
                               uint8_t* out, size_t outSize) {
  // Output buffer
  uint8_t* outStart = out;
  uint8_t* outEnd = out + outSize;

  int64_t c = 0;
  int lc = 0;

  auto putBits = [&](int nbits, int64_t bits) {
    c = (c << nbits) | bits;
    lc += nbits;
    while (lc >= 8 && out < outEnd) {
      lc -= 8;
      *out++ = static_cast<uint8_t>(c >> lc);
    }
  };

  for (int i = im; i <= iM; ++i) {
    int l = static_cast<int>(hcode[i] & 63);

    if (l == 0) {
      // Count run of zeros
      int zerun = 1;
      while (i + zerun <= iM && (hcode[i + zerun] & 63) == 0) {
        zerun++;
      }

      if (zerun >= 2 + 63 - 59) {
        // Long zero run
        putBits(6, 63);
        putBits(8, zerun - (2 + 63 - 59));
        i += zerun - 1;
      } else if (zerun >= 2) {
        // Short zero run
        putBits(6, 59 + zerun - 2);
        i += zerun - 1;
      } else {
        putBits(6, 0);
      }
    } else {
      putBits(6, l);
    }
  }

  // Flush remaining bits
  if (lc > 0 && out < outEnd) {
    *out++ = static_cast<uint8_t>(c << (8 - lc));
  }

  return out - outStart;
}

// Encode data using Huffman codes
inline size_t hufEncode(const int64_t* hcode, const uint16_t* data, int nData,
                         uint8_t* out, size_t outSize) {
  uint8_t* outStart = out;
  uint8_t* outEnd = out + outSize;

  int64_t c = 0;
  int lc = 0;

  for (int i = 0; i < nData; ++i) {
    int64_t code = hcode[data[i]];
    int len = static_cast<int>(code & 63);
    int64_t bits = code >> 6;

    if (len == 0) continue;  // Skip symbols with no code

    c = (c << len) | bits;
    lc += len;

    while (lc >= 8 && out < outEnd) {
      lc -= 8;
      *out++ = static_cast<uint8_t>(c >> lc);
    }
  }

  // Flush remaining bits
  if (lc > 0 && out < outEnd) {
    *out++ = static_cast<uint8_t>(c << (8 - lc));
  }

  return out - outStart;
}

// Full PIZ compression
inline CompressResult compress_piz(const uint16_t* data, int numPixels,
                                    int width, int height, int numChannels,
                                    const int* channelSizes,
                                    uint8_t* out, size_t outCapacity) {
  if (!data || !out || numPixels <= 0 || width <= 0 || height <= 0) {
    return CompressResult::error("Invalid input parameters");
  }

  // Work buffers
  std::vector<uint16_t> tmpData(numPixels);
  std::vector<uint8_t> bitmap(PIZ_BITMAP_SIZE);
  std::vector<uint16_t> lut(PIZ_USHORT_RANGE);
  std::vector<int64_t> freq(HUF_ENCSIZE);
  std::vector<int64_t> hcode(HUF_ENCSIZE);

  // Copy data to work buffer
  std::memcpy(tmpData.data(), data, numPixels * sizeof(uint16_t));

  // Apply wavelet transform per channel
  uint16_t maxValue = 0;
  uint16_t* channelData = tmpData.data();
  for (int ch = 0; ch < numChannels; ++ch) {
    int size = channelSizes ? channelSizes[ch] : 1;
    int chWidth = width;
    int chHeight = height;
    int chPixels = chWidth * chHeight;

    for (int s = 0; s < size; ++s) {
      // Find max value for this channel
      uint16_t chMax = 0;
      for (int i = 0; i < chPixels; ++i) {
        if (channelData[i] > chMax) chMax = channelData[i];
      }
      if (chMax > maxValue) maxValue = chMax;

      // Apply 2D wavelet
      wav2Encode(channelData, chWidth, 1, chHeight, chWidth, chMax);
      channelData += chPixels;
    }
  }

  // Build bitmap and forward LUT
  uint16_t minNonZero, maxNonZero;
  bitmapFromData(tmpData.data(), numPixels, bitmap.data(), minNonZero, maxNonZero);
  uint16_t lutMax = forwardLutFromBitmap(bitmap.data(), lut.data());

  // Apply LUT
  applyLut(lut.data(), tmpData.data(), numPixels);

  // Build Huffman encoding table
  countFrequencies(tmpData.data(), numPixels, freq.data());
  int im, iM;
  hufBuildEncTable(freq.data(), &im, &iM, hcode.data());
  hufCanonicalCodeTable(hcode.data());

  // Output format:
  // - minNonZero (2 bytes)
  // - maxNonZero (2 bytes)
  // - bitmap (maxNonZero - minNonZero + 1 bytes)
  // - im (4 bytes)
  // - iM (4 bytes)
  // - table size (4 bytes)
  // - huffman table
  // - data size (4 bytes)
  // - huffman encoded data

  uint8_t* outPtr = out;
  uint8_t* outEnd = out + outCapacity;

  // Write bitmap range
  if (outPtr + 4 > outEnd) {
    return CompressResult::error("Output buffer too small");
  }
  *outPtr++ = static_cast<uint8_t>(minNonZero >> 8);
  *outPtr++ = static_cast<uint8_t>(minNonZero);
  *outPtr++ = static_cast<uint8_t>(maxNonZero >> 8);
  *outPtr++ = static_cast<uint8_t>(maxNonZero);

  // Write bitmap
  int bitmapLen = (minNonZero <= maxNonZero) ? (maxNonZero - minNonZero + 1) : 0;
  if (outPtr + bitmapLen > outEnd) {
    return CompressResult::error("Output buffer too small for bitmap");
  }
  std::memcpy(outPtr, bitmap.data() + minNonZero, bitmapLen);
  outPtr += bitmapLen;

  // Write im, iM
  if (outPtr + 8 > outEnd) {
    return CompressResult::error("Output buffer too small");
  }
  *outPtr++ = static_cast<uint8_t>(im >> 24);
  *outPtr++ = static_cast<uint8_t>(im >> 16);
  *outPtr++ = static_cast<uint8_t>(im >> 8);
  *outPtr++ = static_cast<uint8_t>(im);
  *outPtr++ = static_cast<uint8_t>(iM >> 24);
  *outPtr++ = static_cast<uint8_t>(iM >> 16);
  *outPtr++ = static_cast<uint8_t>(iM >> 8);
  *outPtr++ = static_cast<uint8_t>(iM);

  // Pack and write Huffman table
  std::vector<uint8_t> tableData(HUF_ENCSIZE);
  size_t tableSize = hufPackEncTable(hcode.data(), im, iM, tableData.data(), tableData.size());

  if (outPtr + 4 + tableSize > outEnd) {
    return CompressResult::error("Output buffer too small for table");
  }
  *outPtr++ = static_cast<uint8_t>(tableSize >> 24);
  *outPtr++ = static_cast<uint8_t>(tableSize >> 16);
  *outPtr++ = static_cast<uint8_t>(tableSize >> 8);
  *outPtr++ = static_cast<uint8_t>(tableSize);
  std::memcpy(outPtr, tableData.data(), tableSize);
  outPtr += tableSize;

  // Encode and write data
  size_t maxDataSize = outEnd - outPtr - 4;
  std::vector<uint8_t> encodedData(numPixels * 2 + 1024);
  size_t dataSize = hufEncode(hcode.data(), tmpData.data(), numPixels,
                               encodedData.data(), encodedData.size());

  if (outPtr + 4 + dataSize > outEnd) {
    return CompressResult::error("Output buffer too small for data");
  }
  *outPtr++ = static_cast<uint8_t>(dataSize >> 24);
  *outPtr++ = static_cast<uint8_t>(dataSize >> 16);
  *outPtr++ = static_cast<uint8_t>(dataSize >> 8);
  *outPtr++ = static_cast<uint8_t>(dataSize);
  std::memcpy(outPtr, encodedData.data(), dataSize);
  outPtr += dataSize;

  return CompressResult::ok(outPtr - out);
}

// ============================================================================
// High-Level Compression Interface
// ============================================================================

enum CompressionType {
  COMPRESSION_NONE = 0,
  COMPRESSION_RLE = 1,
  COMPRESSION_ZIPS = 2,
  COMPRESSION_ZIP = 3,
  COMPRESSION_PIZ = 4
};

// Get scanlines per block for compression type
inline int getScanLinesPerBlock(CompressionType type) {
  switch (type) {
    case COMPRESSION_NONE:
    case COMPRESSION_RLE:
    case COMPRESSION_ZIPS:
      return 1;
    case COMPRESSION_ZIP:
      return 16;
    case COMPRESSION_PIZ:
      return 32;
    default:
      return 1;
  }
}

// Compress a scanline block
inline CompressResult compressScanlineBlock(
    CompressionType type,
    const uint8_t* src, size_t srcSize,
    uint8_t* dst, size_t dstCapacity,
    std::vector<uint8_t>& workBuffer,
    int compressionLevel = 6) {

  switch (type) {
    case COMPRESSION_NONE:
      if (dstCapacity < srcSize) {
        return CompressResult::error("Output buffer too small");
      }
      std::memcpy(dst, src, srcSize);
      return CompressResult::ok(srcSize);

    case COMPRESSION_RLE:
      return compress_rle(src, srcSize, dst, dstCapacity, workBuffer);

    case COMPRESSION_ZIPS:
    case COMPRESSION_ZIP:
      return compress_zip(src, srcSize, dst, dstCapacity, workBuffer, compressionLevel);

    case COMPRESSION_PIZ:
      // PIZ uses 16-bit data, needs different interface
      return CompressResult::error("Use compress_piz for PIZ compression");

    default:
      return CompressResult::error("Unknown compression type");
  }
}

// Get compression backend info
inline const char* get_compress_info() {
#if TINYEXR_COMPRESS_HAS_ZLIB
#if defined(TINYEXR_USE_MINIZ) && TINYEXR_USE_MINIZ
  return "miniz (bundled)";
#else
  return "zlib (system)";
#endif
#else
  return "No deflate backend (RLE only)";
#endif
}

}  // namespace compress
}  // namespace tinyexr

#endif  // TINYEXR_COMPRESS_HH_
