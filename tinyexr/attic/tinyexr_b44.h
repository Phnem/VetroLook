/*
 * TinyEXR V3 - Pure C11 B44/B44A Decompression
 *
 * Ported from tinyexr_v2_impl.hh (C++ DecompressB44V2).
 * Provides B44 and B44A decompression for OpenEXR files.
 *
 * Copyright (c) 2024-2026 TinyEXR authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef TINYEXR_B44_H_
#define TINYEXR_B44_H_

#include <stdint.h>
#include <stddef.h>

#ifndef TINYEXR_CRT_H_
#include "tinyexr_crt.h"
#endif

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * B44 Lookup Tables
 * ============================================================================ */

/* Half-float to 14-bit log table (generated at runtime, cached) */
static uint16_t g_tinyexr_b44_exp_table[65536];
/* 14-bit log to half-float table */
static uint16_t g_tinyexr_b44_log_table[16384];
/* Thread-safe init flag. Requires ATOMIC_INT/ATOMIC_LOAD/ATOMIC_STORE
 * macros from tinyexr_c_impl.c (defined before this header is included). */
static ATOMIC_INT g_tinyexr_b44_tables_initialized;

static void tinyexr_b44_init_tables(void) {
    if (ATOMIC_LOAD(g_tinyexr_b44_tables_initialized)) return;

    /* Generate expTable: half-float -> 14-bit log */
    for (int i = 0; i < 65536; i++) {
        uint16_t h = (uint16_t)i;

        if ((h & 0x7FFF) == 0) {
            /* +0 or -0 -> center point */
            g_tinyexr_b44_exp_table[i] = (h & 0x8000) ? 8191 : 8192;
        } else if ((h & 0x7C00) == 0x7C00) {
            /* Inf or NaN - clamp to max/min */
            g_tinyexr_b44_exp_table[i] = (h & 0x8000) ? 0 : 16383;
        } else {
            uint16_t magnitude = h & 0x7FFF;
            uint16_t log_offset = magnitude >> 2;

            if (h & 0x8000) {
                int log_val = 8191 - (int)log_offset;
                if (log_val < 0) log_val = 0;
                g_tinyexr_b44_exp_table[i] = (uint16_t)log_val;
            } else {
                int log_val = 8192 + (int)log_offset;
                if (log_val > 16383) log_val = 16383;
                g_tinyexr_b44_exp_table[i] = (uint16_t)log_val;
            }
        }
    }

    /* Generate logTable: 14-bit log -> half-float */
    for (int i = 0; i < 16384; i++) {
        if (i == 8192) {
            g_tinyexr_b44_log_table[i] = 0x0000;
        } else if (i == 8191) {
            g_tinyexr_b44_log_table[i] = 0x8000;
        } else if (i > 8192) {
            uint16_t log_offset = (uint16_t)(i - 8192);
            uint16_t magnitude = log_offset << 2;
            if (magnitude > 0x7BFF) magnitude = 0x7BFF;
            g_tinyexr_b44_log_table[i] = magnitude;
        } else {
            uint16_t log_offset = (uint16_t)(8191 - i);
            uint16_t magnitude = log_offset << 2;
            if (magnitude > 0x7BFF) magnitude = 0x7BFF;
            g_tinyexr_b44_log_table[i] = 0x8000 | magnitude;
        }
    }

    ATOMIC_STORE(g_tinyexr_b44_tables_initialized, 1);
}

/* ============================================================================
 * B44 Block Unpacking
 * ============================================================================ */

/* Unpack one 4x4 block from B44 compressed 14 bytes */
static void tinyexr_b44_unpack_block(uint16_t dst[16], const uint8_t src[14]) {
    uint16_t s0 = ((uint16_t)src[0] << 8) | src[1];
    uint16_t shift = src[2] >> 2;
    uint16_t bias = (uint16_t)(0x20u << shift);

    uint16_t s4 = (uint16_t)(
        (uint32_t)s0 +
        (uint32_t)((((uint32_t)src[2] << 4) |
                     ((uint32_t)src[3] >> 4)) & 0x3fu) * (1u << shift) - bias);

    uint16_t s8 = (uint16_t)(
        (uint32_t)s4 +
        (uint32_t)((((uint32_t)src[3] << 2) |
                     ((uint32_t)src[4] >> 6)) & 0x3fu) * (1u << shift) - bias);

    uint16_t s12 = (uint16_t)(
        (uint32_t)s8 +
        (uint32_t)(src[4] & 0x3fu) * (1u << shift) - bias);

    uint16_t s1 = (uint16_t)(
        (uint32_t)s0 +
        (uint32_t)(src[5] >> 2) * (1u << shift) - bias);

    uint16_t s5 = (uint16_t)(
        (uint32_t)s4 +
        (uint32_t)((((uint32_t)src[5] << 4) |
                     ((uint32_t)src[6] >> 4)) & 0x3fu) * (1u << shift) - bias);

    uint16_t s9 = (uint16_t)(
        (uint32_t)s8 +
        (uint32_t)((((uint32_t)src[6] << 2) |
                     ((uint32_t)src[7] >> 6)) & 0x3fu) * (1u << shift) - bias);

    uint16_t s13 = (uint16_t)(
        (uint32_t)s12 +
        (uint32_t)(src[7] & 0x3fu) * (1u << shift) - bias);

    uint16_t s2 = (uint16_t)(
        (uint32_t)s1 +
        (uint32_t)(src[8] >> 2) * (1u << shift) - bias);

    uint16_t s6 = (uint16_t)(
        (uint32_t)s5 +
        (uint32_t)((((uint32_t)src[8] << 4) |
                     ((uint32_t)src[9] >> 4)) & 0x3fu) * (1u << shift) - bias);

    uint16_t s10 = (uint16_t)(
        (uint32_t)s9 +
        (uint32_t)((((uint32_t)src[9] << 2) |
                     ((uint32_t)src[10] >> 6)) & 0x3fu) * (1u << shift) - bias);

    uint16_t s14 = (uint16_t)(
        (uint32_t)s13 +
        (uint32_t)(src[10] & 0x3fu) * (1u << shift) - bias);

    uint16_t s3 = (uint16_t)(
        (uint32_t)s2 +
        (uint32_t)(src[11] >> 2) * (1u << shift) - bias);

    uint16_t s7 = (uint16_t)(
        (uint32_t)s6 +
        (uint32_t)((((uint32_t)src[11] << 4) |
                     ((uint32_t)src[12] >> 4)) & 0x3fu) * (1u << shift) - bias);

    uint16_t s11 = (uint16_t)(
        (uint32_t)s10 +
        (uint32_t)((((uint32_t)src[12] << 2) |
                     ((uint32_t)src[13] >> 6)) & 0x3fu) * (1u << shift) - bias);

    uint16_t s15 = (uint16_t)(
        (uint32_t)s14 +
        (uint32_t)(src[13] & 0x3fu) * (1u << shift) - bias);

    dst[0] = s0;   dst[1] = s1;   dst[2] = s2;   dst[3] = s3;
    dst[4] = s4;   dst[5] = s5;   dst[6] = s6;   dst[7] = s7;
    dst[8] = s8;   dst[9] = s9;   dst[10] = s10; dst[11] = s11;
    dst[12] = s12; dst[13] = s13; dst[14] = s14; dst[15] = s15;

    /* Convert from ordered-magnitude to half-float */
    for (int i = 0; i < 16; i++) {
        if (dst[i] & 0x8000) {
            dst[i] &= 0x7fff;
        } else {
            dst[i] = (uint16_t)(~dst[i]);
        }
    }
}

/* Unpack a 3-byte flat block (all pixels same value) */
static void tinyexr_b44_unpack_flat_block(uint16_t dst[16], const uint8_t src[3]) {
    uint16_t t = ((uint16_t)src[0] << 8) | src[1];
    uint16_t h;
    if (t & 0x8000) {
        h = t & 0x7fff;
    } else {
        h = (uint16_t)(~t);
    }
    for (int i = 0; i < 16; i++) {
        dst[i] = h;
    }
}

/*
 * Channel info for B44 decompression.
 * Layout must match ExrChannelData in tinyexr_c_impl.c exactly.
 * Uses EXR_PIXEL_* constants from tinyexr_c.h.
 */
#include "tinyexr_c.h"

typedef struct {
    char name[64];
    uint32_t pixel_type;   /* EXR_PIXEL_UINT/HALF/FLOAT */
    int32_t x_sampling;
    int32_t y_sampling;
    uint8_t p_linear;
} TinyExrB44Channel;

/* ============================================================================
 * B44/B44A Decompression
 * ============================================================================ */

static bool tinyexr_decompress_b44(uint8_t* dst, size_t expected_size,
                                    const uint8_t* src, size_t src_size,
                                    int width, int num_lines,
                                    int num_channels,
                                    const TinyExrB44Channel* channels) {
    /* B44A flat-block detection is done per-block via shift value (>= 13<<2) */

    /* Eagerly init log tables if any channel uses p_linear */
    for (int c = 0; c < num_channels; c++) {
        if (channels[c].p_linear) { tinyexr_b44_init_tables(); break; }
    }

    const uint8_t* in_ptr = src;
    const uint8_t* in_end = src + src_size;

    /* Calculate bytes per scanline */
    size_t bytes_per_scanline = 0;
    for (int c = 0; c < num_channels; c++) {
        int ch_width = width / channels[c].x_sampling;
        size_t pixel_size = (channels[c].pixel_type == EXR_PIXEL_HALF) ? 2 : 4;
        bytes_per_scanline += (size_t)ch_width * pixel_size;
    }

    exr_memset(dst, 0, expected_size);

    for (int c = 0; c < num_channels; c++) {
        if (channels[c].x_sampling <= 0 || channels[c].y_sampling <= 0) {
            return false;
        }
        int ch_width = width / channels[c].x_sampling;
        int ch_height = num_lines / channels[c].y_sampling;

        /* Calculate channel offset within each scanline */
        size_t ch_offset = 0;
        for (int i = 0; i < c; i++) {
            int ch_w = width / channels[i].x_sampling;
            size_t ps = (channels[i].pixel_type == EXR_PIXEL_HALF) ? 2 : 4;
            ch_offset += (size_t)ch_w * ps;
        }

        if (channels[c].pixel_type != EXR_PIXEL_HALF) {
            /* Non-HALF channels are stored uncompressed */
            size_t pixel_size = 4; /* UINT and FLOAT are both 4 bytes */
            for (int line = 0; line < num_lines; line++) {
                if ((line % channels[c].y_sampling) != 0) continue;
                size_t line_bytes = (size_t)ch_width * pixel_size;
                if (in_ptr + line_bytes > in_end) return false;

                uint8_t* line_ptr = dst + (size_t)line * bytes_per_scanline + ch_offset;
                exr_memcpy(line_ptr, in_ptr, line_bytes);
                in_ptr += line_bytes;
            }
            continue;
        }

        /* Process HALF channel in 4x4 blocks */
        int num_blocks_x = (ch_width + 3) / 4;
        int num_blocks_y = (ch_height + 3) / 4;

        /* Overflow-safe allocation for temporary channel buffer */
        size_t ch_alloc = (size_t)ch_width * (size_t)ch_height;
        if (ch_width > 0 && ch_alloc / (size_t)ch_width != (size_t)ch_height) {
            return false; /* overflow */
        }
        if (ch_alloc > SIZE_MAX / sizeof(uint16_t)) {
            return false; /* overflow */
        }
        ch_alloc *= sizeof(uint16_t);

        uint16_t* ch_data = (uint16_t*)EXR_CRT_MALLOC(ch_alloc);
        if (!ch_data) return false;
        exr_memset(ch_data, 0, ch_alloc);

        for (int by = 0; by < num_blocks_y; by++) {
            for (int bx = 0; bx < num_blocks_x; bx++) {
                uint16_t block[16];

                if (in_ptr + 3 > in_end) {
                    EXR_CRT_FREE(ch_data);
                    return false;
                }

                if (in_ptr[2] >= (13 << 2)) {
                    /* 3-byte flat block */
                    tinyexr_b44_unpack_flat_block(block, in_ptr);
                    in_ptr += 3;
                } else {
                    /* Regular 14-byte block */
                    if (in_ptr + 14 > in_end) {
                        EXR_CRT_FREE(ch_data);
                        return false;
                    }
                    tinyexr_b44_unpack_block(block, in_ptr);
                    in_ptr += 14;
                }

                /* Apply p_linear conversion (log table) if needed */
                if (channels[c].p_linear) {
                    for (int i = 0; i < 16; i++) {
                        block[i] = g_tinyexr_b44_log_table[block[i]];
                    }
                }

                /* Copy block to temp buffer with bounds checking */
                for (int py = 0; py < 4; py++) {
                    int y = by * 4 + py;
                    if (y >= ch_height) break;
                    for (int px = 0; px < 4; px++) {
                        int x = bx * 4 + px;
                        if (x >= ch_width) break;
                        ch_data[y * ch_width + x] = block[py * 4 + px];
                    }
                }
            }
        }

        /* Copy channel data to output in per-scanline layout */
        for (int line = 0; line < num_lines; line++) {
            if ((line % channels[c].y_sampling) != 0) continue;
            int ch_line = line / channels[c].y_sampling;
            uint8_t* line_ptr = dst + (size_t)line * bytes_per_scanline + ch_offset;

            for (int x = 0; x < ch_width; x++) {
                uint16_t val = ch_data[ch_line * ch_width + x];
                line_ptr[x * 2] = (uint8_t)(val & 0xFF);
                line_ptr[x * 2 + 1] = (uint8_t)(val >> 8);
            }
        }

        EXR_CRT_FREE(ch_data);
    }

    return true;
}

#ifdef __cplusplus
}
#endif

#endif /* TINYEXR_B44_H_ */
