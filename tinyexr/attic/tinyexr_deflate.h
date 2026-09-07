/*
 * TinyEXR V3 - Pure C11 Deflate/Inflate Implementation
 *
 * Ported from tinyexr_huffman.hh (C++ FastDeflateDecoder).
 * Provides zlib/deflate decompression for ZIP/ZIPS EXR compression.
 *
 * This is a header-only C11 implementation. Include in exactly one .c file
 * with TINYEXR_DEFLATE_IMPLEMENTATION defined, or include as-is (all static).
 *
 * Copyright (c) 2024-2026 TinyEXR authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef TINYEXR_DEFLATE_H_
#define TINYEXR_DEFLATE_H_

#include <stdint.h>
#include <stddef.h>

/* Use tinyexr_crt.h for exr_memcpy/exr_memset when available, fall back to string.h */
#ifdef TINYEXR_CRT_H_
/* Already included — exr_exr_memcpy, exr_exr_memset available */
#else
#include "tinyexr_crt.h"
#endif

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Compiler Hints
 * ============================================================================ */

#ifndef TINYEXR_LIKELY
#if defined(__GNUC__) || defined(__clang__)
#define TINYEXR_LIKELY(x)   __builtin_expect(!!(x), 1)
#define TINYEXR_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define TINYEXR_DFL_INLINE  __attribute__((always_inline)) static inline
#elif defined(_MSC_VER)
#define TINYEXR_LIKELY(x)   (x)
#define TINYEXR_UNLIKELY(x) (x)
#define TINYEXR_DFL_INLINE  __forceinline static
#else
#define TINYEXR_LIKELY(x)   (x)
#define TINYEXR_UNLIKELY(x) (x)
#define TINYEXR_DFL_INLINE  static inline
#endif
#endif

/* ============================================================================
 * Deflate Constants
 * ============================================================================ */

#define TINYEXR_DFL_MAX_BITS       15
#define TINYEXR_DFL_LITLEN_CODES   288
#define TINYEXR_DFL_DIST_CODES     32
#define TINYEXR_DFL_CODELEN_CODES  19

static const uint8_t tinyexr_dfl_codelen_order[TINYEXR_DFL_CODELEN_CODES] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

static const uint16_t tinyexr_dfl_length_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const uint8_t tinyexr_dfl_length_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};

static const uint16_t tinyexr_dfl_dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const uint8_t tinyexr_dfl_dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

/* ============================================================================
 * Bit Reader (LSB first, for deflate)
 * ============================================================================ */

typedef struct {
    uint64_t bits;
    int count;
    const uint8_t* ptr;
    const uint8_t* end;
} TinyExrDflBitReader;

TINYEXR_DFL_INLINE void tinyexr_dfl_br_init(TinyExrDflBitReader* br,
                                              const uint8_t* data, size_t size) {
    br->bits = 0;
    br->count = 0;
    br->ptr = data;
    br->end = data + size;
}

TINYEXR_DFL_INLINE void tinyexr_dfl_br_refill(TinyExrDflBitReader* br) {
    while (br->count <= 56 && br->ptr < br->end) {
        br->bits |= (uint64_t)(*br->ptr++) << br->count;
        br->count += 8;
    }
}

TINYEXR_DFL_INLINE void tinyexr_dfl_br_refill_fast(TinyExrDflBitReader* br) {
    if (TINYEXR_LIKELY(br->ptr + 8 <= br->end)) {
        uint64_t new_bits;
        exr_memcpy(&new_bits, br->ptr, 8);
        br->bits |= new_bits << br->count;
        int bytes_to_advance = (64 - br->count) / 8;
        br->ptr += bytes_to_advance;
        br->count += bytes_to_advance * 8;
    } else {
        tinyexr_dfl_br_refill(br);
    }
}

TINYEXR_DFL_INLINE uint32_t tinyexr_dfl_br_peek(const TinyExrDflBitReader* br, int n) {
    return (uint32_t)(br->bits & ((1ULL << n) - 1));
}

TINYEXR_DFL_INLINE void tinyexr_dfl_br_consume(TinyExrDflBitReader* br, int n) {
    br->bits >>= n;
    br->count -= n;
}

TINYEXR_DFL_INLINE uint32_t tinyexr_dfl_br_read(TinyExrDflBitReader* br, int n) {
    uint32_t result = tinyexr_dfl_br_peek(br, n);
    tinyexr_dfl_br_consume(br, n);
    return result;
}

TINYEXR_DFL_INLINE void tinyexr_dfl_br_align(TinyExrDflBitReader* br) {
    int skip = br->count & 7;
    tinyexr_dfl_br_consume(br, skip);
}

/* ============================================================================
 * Huffman Table
 * ============================================================================ */

/* 12-bit fast lookup covers the vast majority of deflate codes */
#define TINYEXR_DFL_FAST_BITS 12
#define TINYEXR_DFL_FAST_SIZE (1 << TINYEXR_DFL_FAST_BITS)

typedef struct {
    /* Fast lookup: 12-bit index -> (symbol << 4) | length | 0x8000 */
    uint16_t fast_table[TINYEXR_DFL_FAST_SIZE];
    /* Slow table for codes > 12 bits */
    uint16_t slow_table[640];
    int slow_count;
    int max_bits;
} TinyExrDflHuffTable;

static void tinyexr_dfl_ht_build_fixed_litlen(TinyExrDflHuffTable* t) {
    t->max_bits = 9;
    t->slow_count = 0;
    exr_memset(t->fast_table, 0, sizeof(t->fast_table));
    exr_memset(t->slow_table, 0, sizeof(t->slow_table));

    for (int sym = 0; sym <= 287; sym++) {
        int len, code;
        if (sym <= 143) {
            len = 8; code = 0x30 + sym;
        } else if (sym <= 255) {
            len = 9; code = 0x190 + (sym - 144);
        } else if (sym <= 279) {
            len = 7; code = sym - 256;
        } else {
            len = 8; code = 0xC0 + (sym - 280);
        }

        int rev_code = 0;
        for (int i = 0; i < len; i++) {
            rev_code = (rev_code << 1) | ((code >> i) & 1);
        }

        int fill = 1 << (TINYEXR_DFL_FAST_BITS - len);
        for (int i = 0; i < fill; i++) {
            int idx = rev_code | (i << len);
            if (idx < TINYEXR_DFL_FAST_SIZE) {
                t->fast_table[idx] = (uint16_t)((sym << 4) | len | 0x8000);
            }
        }
    }
}

static void tinyexr_dfl_ht_build_fixed_dist(TinyExrDflHuffTable* t) {
    t->max_bits = 5;
    t->slow_count = 0;
    exr_memset(t->fast_table, 0, sizeof(t->fast_table));
    exr_memset(t->slow_table, 0, sizeof(t->slow_table));

    for (int sym = 0; sym < 32; sym++) {
        int rev_code = 0;
        for (int i = 0; i < 5; i++) {
            rev_code = (rev_code << 1) | ((sym >> i) & 1);
        }

        int fill = 1 << (TINYEXR_DFL_FAST_BITS - 5);
        for (int i = 0; i < fill; i++) {
            int idx = rev_code | (i << 5);
            if (idx < TINYEXR_DFL_FAST_SIZE) {
                t->fast_table[idx] = (uint16_t)((sym << 4) | 5 | 0x8000);
            }
        }
    }
}

/* Reverse `len` bits of `code` (deflate uses LSB-first codes) */
static inline int tinyexr_dfl_reverse_bits(int code, int len) {
    int rev = 0;
    for (int i = 0; i < len; i++) {
        rev = (rev << 1) | ((code >> i) & 1);
    }
    return rev;
}

static bool tinyexr_dfl_ht_build(TinyExrDflHuffTable* table,
                                  const uint8_t* lens, int count) {
    int bl_count[TINYEXR_DFL_MAX_BITS + 1] = {0};
    int max_len = 0;
    for (int i = 0; i < count; i++) {
        if (lens[i] > 0) {
            bl_count[lens[i]]++;
            if (lens[i] > max_len) max_len = lens[i];
        }
    }

    table->max_bits = max_len;
    table->slow_count = 0;
    exr_memset(table->fast_table, 0, sizeof(table->fast_table));

    /* Compute first code for each length */
    int next_code[TINYEXR_DFL_MAX_BITS + 1] = {0};
    int code = 0;
    for (int bits = 1; bits <= max_len; bits++) {
        code = (code + bl_count[bits - 1]) << 1;
        next_code[bits] = code;
    }

    /* Single-pass: build both fast and slow tables simultaneously.
     * Collect slow entries (codes > FAST_BITS) into a temporary buffer
     * with separate arrays for code, symbol, and length. */
    uint16_t slow_codes[320];
    uint16_t slow_syms[320];
    uint8_t slow_lens[320];
    int slow_total = 0;

    for (int sym = 0; sym < count; sym++) {
        int len = lens[sym];
        if (len == 0) continue;

        int code_val = next_code[len]++;
        int rev_code = tinyexr_dfl_reverse_bits(code_val, len);

        if (len <= TINYEXR_DFL_FAST_BITS) {
            /* Fast table: fill all suffix combinations */
            uint16_t entry = (uint16_t)((sym << 4) | len | 0x8000);
            int fill = 1 << (TINYEXR_DFL_FAST_BITS - len);
            for (int i = 0; i < fill; i++) {
                int idx = rev_code | (i << len);
                table->fast_table[idx] = entry;
            }
        } else if (slow_total < 320) {
            slow_codes[slow_total] = (uint16_t)rev_code;
            slow_syms[slow_total] = (uint16_t)sym;
            slow_lens[slow_total] = (uint8_t)len;
            slow_total++;
        }
    }

    /* Build slow table organized by length */
    uint16_t* slow_ptr = table->slow_table;
    for (int bits = TINYEXR_DFL_FAST_BITS + 1; bits <= max_len && bits <= 15; bits++) {
        uint16_t* count_ptr = slow_ptr++;
        uint16_t bit_count = 0;
        for (int i = 0; i < slow_total; i++) {
            if (slow_lens[i] == bits) {
                *slow_ptr++ = slow_codes[i];
                *slow_ptr++ = slow_syms[i];
                bit_count++;
                table->slow_count++;
            }
        }
        *count_ptr = bit_count;
    }

    return true;
}

/* ============================================================================
 * Deflate Decoder
 * ============================================================================ */

static int tinyexr_dfl_decode_symbol_slow(TinyExrDflBitReader* reader,
                                           const TinyExrDflHuffTable* table) {
    if (reader->count < 15) {
        tinyexr_dfl_br_refill(reader);
    }

    const uint16_t* ptr = table->slow_table;

    for (int bits = TINYEXR_DFL_FAST_BITS + 1; bits <= table->max_bits && bits <= 15; bits++) {
        uint16_t cnt = *ptr++;
        uint32_t code_mask = (1u << bits) - 1;
        uint32_t peeked = tinyexr_dfl_br_peek(reader, bits) & code_mask;

        for (uint16_t i = 0; i < cnt; i++) {
            uint16_t stored_code = ptr[i * 2];
            uint16_t symbol = ptr[i * 2 + 1];

            if (peeked == stored_code) {
                tinyexr_dfl_br_consume(reader, bits);
                return (int)symbol;
            }
        }
        ptr += cnt * 2;
    }

    return -1;
}

TINYEXR_DFL_INLINE int tinyexr_dfl_decode_symbol(TinyExrDflBitReader* reader,
                                                   const TinyExrDflHuffTable* table) {
    if (TINYEXR_UNLIKELY(reader->count < 15)) {
        tinyexr_dfl_br_refill_fast(reader);
    }

    uint32_t idx = tinyexr_dfl_br_peek(reader, TINYEXR_DFL_FAST_BITS);
    uint16_t entry = table->fast_table[idx];

    if (TINYEXR_LIKELY(entry & 0x8000)) {
        int len = entry & 0xF;
        int sym = (entry >> 4) & 0x7FF;
        tinyexr_dfl_br_consume(reader, len);
        return sym;
    }

    return tinyexr_dfl_decode_symbol_slow(reader, table);
}

static void tinyexr_dfl_copy_match(uint8_t* dst, const uint8_t* src,
                                    int length, int distance) {
    if (TINYEXR_UNLIKELY(length <= 0)) return;

    if (distance == 1) {
        /* RLE: single byte repeat */
        exr_memset(dst, *src, (size_t)length);
        return;
    }

    if (distance == 2 && length >= 4) {
        uint16_t pattern;
        exr_memcpy(&pattern, src, 2);
        while (length >= 8) {
            exr_memcpy(dst + 0, &pattern, 2);
            exr_memcpy(dst + 2, &pattern, 2);
            exr_memcpy(dst + 4, &pattern, 2);
            exr_memcpy(dst + 6, &pattern, 2);
            dst += 8;
            length -= 8;
        }
        while (length >= 2) {
            exr_memcpy(dst, &pattern, 2);
            dst += 2;
            length -= 2;
        }
    } else if (distance == 4 && length >= 8) {
        uint32_t pattern;
        exr_memcpy(&pattern, src, 4);
        while (length >= 16) {
            exr_memcpy(dst + 0, &pattern, 4);
            exr_memcpy(dst + 4, &pattern, 4);
            exr_memcpy(dst + 8, &pattern, 4);
            exr_memcpy(dst + 12, &pattern, 4);
            dst += 16;
            length -= 16;
        }
        while (length >= 4) {
            exr_memcpy(dst, &pattern, 4);
            dst += 4;
            length -= 4;
        }
    }

    /* Byte-by-byte copy for trailing bytes or non-pattern distances */
    src = dst - distance;
    while (length-- > 0) {
        *dst++ = *src++;
    }
}

static bool tinyexr_dfl_decode_dynamic_tables(TinyExrDflBitReader* reader,
                                               TinyExrDflHuffTable* litlen,
                                               TinyExrDflHuffTable* dist) {
    tinyexr_dfl_br_refill(reader);

    int hlit = tinyexr_dfl_br_read(reader, 5) + 257;
    int hdist = tinyexr_dfl_br_read(reader, 5) + 1;
    int hclen = tinyexr_dfl_br_read(reader, 4) + 4;

    uint8_t codelen_lens[TINYEXR_DFL_CODELEN_CODES] = {0};
    for (int i = 0; i < hclen; i++) {
        if (reader->count < 3) tinyexr_dfl_br_refill(reader);
        codelen_lens[tinyexr_dfl_codelen_order[i]] = (uint8_t)tinyexr_dfl_br_read(reader, 3);
    }

    TinyExrDflHuffTable codelen_table;
    if (!tinyexr_dfl_ht_build(&codelen_table, codelen_lens, TINYEXR_DFL_CODELEN_CODES)) {
        return false;
    }

    uint8_t all_lens[TINYEXR_DFL_LITLEN_CODES + TINYEXR_DFL_DIST_CODES] = {0};
    int total = hlit + hdist;
    int i = 0;

    while (i < total) {
        tinyexr_dfl_br_refill(reader);
        int sym = tinyexr_dfl_decode_symbol(reader, &codelen_table);

        if (sym < 0) return false;

        if (sym < 16) {
            all_lens[i++] = (uint8_t)sym;
        } else if (sym == 16) {
            if (i == 0) return false;
            int repeat = tinyexr_dfl_br_read(reader, 2) + 3;
            uint8_t prev = all_lens[i - 1];
            while (repeat-- > 0 && i < total) {
                all_lens[i++] = prev;
            }
        } else if (sym == 17) {
            int repeat = tinyexr_dfl_br_read(reader, 3) + 3;
            while (repeat-- > 0 && i < total) {
                all_lens[i++] = 0;
            }
        } else if (sym == 18) {
            int repeat = tinyexr_dfl_br_read(reader, 7) + 11;
            while (repeat-- > 0 && i < total) {
                all_lens[i++] = 0;
            }
        }
    }

    if (!tinyexr_dfl_ht_build(litlen, all_lens, hlit)) return false;
    if (!tinyexr_dfl_ht_build(dist, all_lens + hlit, hdist)) return false;

    return true;
}

static bool tinyexr_dfl_decode_block(TinyExrDflBitReader* reader,
                                      const TinyExrDflHuffTable* litlen_t,
                                      const TinyExrDflHuffTable* dist_t,
                                      uint8_t** out,
                                      uint8_t* out_start,
                                      uint8_t* out_end) {
    for (;;) {
        /* Bulk refill: ensures enough bits for symbol + extra bits in one go.
         * This reduces refill calls from ~2 per symbol to ~1 per 4-5 symbols. */
        tinyexr_dfl_br_refill_fast(reader);

        /* Fast literal loop: decode consecutive literals without extra refills.
         * Most deflate data is dominated by literals, so optimizing this inner
         * loop has a big impact. */
        while (TINYEXR_LIKELY(reader->count >= 15)) {
            uint32_t idx = tinyexr_dfl_br_peek(reader, TINYEXR_DFL_FAST_BITS);
            uint16_t entry = litlen_t->fast_table[idx];

            if (TINYEXR_UNLIKELY(!(entry & 0x8000))) break;  /* slow path needed */

            int sym = (entry >> 4) & 0x7FF;
            int len = entry & 0xF;
            tinyexr_dfl_br_consume(reader, len);

            if (TINYEXR_LIKELY(sym < 256)) {
                if (TINYEXR_UNLIKELY(*out >= out_end)) return false;
                *(*out)++ = (uint8_t)sym;
                continue;
            }
            if (sym == 256) return true;

            /* Length/distance pair */
            int length_sym = sym - 257;
            if (TINYEXR_UNLIKELY(length_sym >= 29)) return false;

            int length = tinyexr_dfl_length_base[length_sym];
            int extra_bits = tinyexr_dfl_length_extra[length_sym];
            if (extra_bits > 0) {
                if (TINYEXR_UNLIKELY(reader->count < extra_bits))
                    tinyexr_dfl_br_refill_fast(reader);
                length += (int)tinyexr_dfl_br_read(reader, extra_bits);
            }

            /* Decode distance inline for fast path */
            if (TINYEXR_UNLIKELY(reader->count < 15))
                tinyexr_dfl_br_refill_fast(reader);

            uint32_t didx = tinyexr_dfl_br_peek(reader, TINYEXR_DFL_FAST_BITS);
            uint16_t dentry = dist_t->fast_table[didx];
            int dist_sym;

            if (TINYEXR_LIKELY(dentry & 0x8000)) {
                dist_sym = (dentry >> 4) & 0x7FF;
                tinyexr_dfl_br_consume(reader, dentry & 0xF);
            } else {
                dist_sym = tinyexr_dfl_decode_symbol_slow(reader, dist_t);
            }
            if (TINYEXR_UNLIKELY(dist_sym < 0 || dist_sym >= 30)) return false;

            int distance = tinyexr_dfl_dist_base[dist_sym];
            extra_bits = tinyexr_dfl_dist_extra[dist_sym];
            if (extra_bits > 0) {
                if (TINYEXR_UNLIKELY(reader->count < extra_bits))
                    tinyexr_dfl_br_refill_fast(reader);
                distance += (int)tinyexr_dfl_br_read(reader, extra_bits);
            }

            if (TINYEXR_UNLIKELY(*out + length > out_end)) return false;
            if (TINYEXR_UNLIKELY(*out - out_start < distance)) return false;

            const uint8_t* match = *out - distance;
            tinyexr_dfl_copy_match(*out, match, length, distance);
            *out += length;
        }

        /* Slow fallback for symbols not in the fast table */
        int sym = tinyexr_dfl_decode_symbol(reader, litlen_t);
        if (sym < 0) return false;

        if (sym < 256) {
            if (TINYEXR_UNLIKELY(*out >= out_end)) return false;
            *(*out)++ = (uint8_t)sym;
        } else if (sym == 256) {
            return true;
        } else {
            int length_sym = sym - 257;
            if (length_sym >= 29) return false;

            int length = tinyexr_dfl_length_base[length_sym];
            int extra_bits = tinyexr_dfl_length_extra[length_sym];
            if (extra_bits > 0) {
                if (reader->count < extra_bits)
                    tinyexr_dfl_br_refill_fast(reader);
                length += (int)tinyexr_dfl_br_read(reader, extra_bits);
            }

            int dist_sym = tinyexr_dfl_decode_symbol(reader, dist_t);
            if (dist_sym < 0 || dist_sym >= 30) return false;

            int distance = tinyexr_dfl_dist_base[dist_sym];
            extra_bits = tinyexr_dfl_dist_extra[dist_sym];
            if (extra_bits > 0) {
                if (reader->count < extra_bits)
                    tinyexr_dfl_br_refill_fast(reader);
                distance += (int)tinyexr_dfl_br_read(reader, extra_bits);
            }

            if (TINYEXR_UNLIKELY(*out + length > out_end)) return false;
            if (TINYEXR_UNLIKELY(*out - out_start < distance)) return false;

            const uint8_t* match = *out - distance;
            tinyexr_dfl_copy_match(*out, match, length, distance);
            *out += length;
        }
    }
}

/* ============================================================================
 * Public API
 * ============================================================================ */

/* Decompress raw deflate stream (no zlib header) */
static bool tinyexr_inflate(const uint8_t* src, size_t src_len,
                             uint8_t* dst, size_t* dst_len) {
    TinyExrDflBitReader reader;
    tinyexr_dfl_br_init(&reader, src, src_len);

    /* Build fixed Huffman tables */
    TinyExrDflHuffTable fixed_litlen, fixed_dist;
    tinyexr_dfl_ht_build_fixed_litlen(&fixed_litlen);
    tinyexr_dfl_ht_build_fixed_dist(&fixed_dist);

    uint8_t* out = dst;
    uint8_t* out_end = dst + *dst_len;
    bool final_block = false;

    while (!final_block) {
        tinyexr_dfl_br_refill(&reader);

        final_block = tinyexr_dfl_br_read(&reader, 1) != 0;
        int block_type = tinyexr_dfl_br_read(&reader, 2);

        if (block_type == 0) {
            /* Stored block */
            tinyexr_dfl_br_align(&reader);
            if (reader.count < 32) tinyexr_dfl_br_refill(&reader);

            uint16_t len = (uint16_t)tinyexr_dfl_br_read(&reader, 16);
            uint16_t nlen = (uint16_t)tinyexr_dfl_br_read(&reader, 16);

            if ((len ^ nlen) != 0xFFFF) return false;
            if (out + len > out_end) return false;

            /* Flush remaining bits and copy directly from input */
            {
                /* Skip any bits remaining in the bit buffer (already aligned) */
                size_t bytes_avail = (size_t)(reader.end - reader.ptr);
                /* Account for unread bytes still in the bit buffer */
                size_t buffered_bytes = (reader.count > 0) ? (size_t)(reader.count / 8) : 0;
                if ((size_t)len > bytes_avail + buffered_bytes) return false;

                /* Read through bit reader for correctness */
                for (int i = 0; i < len; i++) {
                    if (reader.count < 8) tinyexr_dfl_br_refill(&reader);
                    if (reader.count < 8) return false; /* truncated input */
                    *out++ = (uint8_t)tinyexr_dfl_br_read(&reader, 8);
                }
            }
        } else if (block_type == 1) {
            /* Fixed Huffman */
            if (!tinyexr_dfl_decode_block(&reader, &fixed_litlen, &fixed_dist,
                                           &out, dst, out_end)) {
                return false;
            }
        } else if (block_type == 2) {
            /* Dynamic Huffman */
            TinyExrDflHuffTable dyn_litlen, dyn_dist;
            if (!tinyexr_dfl_decode_dynamic_tables(&reader, &dyn_litlen, &dyn_dist)) {
                return false;
            }
            if (!tinyexr_dfl_decode_block(&reader, &dyn_litlen, &dyn_dist,
                                           &out, dst, out_end)) {
                return false;
            }
        } else {
            return false; /* Invalid block type */
        }
    }

    *dst_len = (size_t)(out - dst);
    return true;
}

/* Decompress zlib data (with 2-byte header + 4-byte Adler-32 trailer) */
static bool tinyexr_inflate_zlib(const uint8_t* src, size_t src_len,
                                  uint8_t* dst, size_t* dst_len) {
    if (src_len < 2) return false;

    uint8_t cmf = src[0];
    uint8_t flg = src[1];

    if ((cmf & 0x0F) != 8) return false;           /* Must be deflate */
    if (((cmf << 8) | flg) % 31 != 0) return false; /* Header checksum */

    size_t offset = 2;
    if (flg & 0x20) {
        /* Dictionary present - skip 4 bytes */
        if (src_len < 6) return false;
        offset += 4;
    }

    /* Skip trailing 4-byte Adler-32 checksum */
    if (src_len - offset < 4) return false;
    return tinyexr_inflate(src + offset, src_len - offset - 4, dst, dst_len);
}

#ifdef __cplusplus
}
#endif

#endif /* TINYEXR_DEFLATE_H_ */
