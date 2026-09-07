/*
 * TinyEXR V3 - Freestanding CRT Replacement
 *
 * Provides libc-free implementations of memory, string, and formatting
 * functions used by the TinyEXR V3 core. All functions are static to
 * avoid symbol collisions.
 *
 * Design goals:
 * - Zero libc dependency (only freestanding C headers: stdint.h, stddef.h)
 * - Secure by default (bounded copies, no buffer overflows)
 * - Minimal footprint
 *
 * To supply your own malloc/free, define before including this header:
 *   #define EXR_CRT_MALLOC(sz)       my_malloc(sz)
 *   #define EXR_CRT_FREE(ptr)        my_free(ptr)
 *   #define EXR_CRT_REALLOC(p,sz)    my_realloc(p,sz)
 *
 * If not defined, the default allocator uses compiler built-in __builtin_malloc
 * (GCC/Clang) or falls back to the platform allocator.
 *
 * Copyright (c) 2024-2026 TinyEXR authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef TINYEXR_CRT_H_
#define TINYEXR_CRT_H_

/* Freestanding headers only — no libc required */
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Pluggable Allocator Backends
 *
 * Users can override these before including this header.
 * The defaults use compiler builtins (GCC/Clang) which do NOT require
 * linking libc when using -ffreestanding, or fall back to platform alloc.
 * ============================================================================ */

#if !defined(EXR_CRT_MALLOC)
#  if defined(__GNUC__) || defined(__clang__)
#    define EXR_CRT_MALLOC(sz)       __builtin_malloc(sz)
#    define EXR_CRT_FREE(ptr)        __builtin_free(ptr)
#    define EXR_CRT_REALLOC(p, sz)   __builtin_realloc(p, sz)
#  elif defined(_MSC_VER)
     /* MSVC: must link a CRT or provide overrides */
     void* __cdecl malloc(size_t);
     void  __cdecl free(void*);
     void* __cdecl realloc(void*, size_t);
#    define EXR_CRT_MALLOC(sz)       malloc(sz)
#    define EXR_CRT_FREE(ptr)        free(ptr)
#    define EXR_CRT_REALLOC(p, sz)   realloc(p, sz)
#  else
     /* Unknown compiler: user must define EXR_CRT_MALLOC etc. */
#    error "Define EXR_CRT_MALLOC, EXR_CRT_FREE, EXR_CRT_REALLOC for your platform"
#  endif
#endif

/* ============================================================================
 * Memory Functions
 * ============================================================================ */

/*
 * Use compiler builtins for memcpy/memset/memmove when available.
 * These generate optimal code (single instructions for small sizes)
 * and are freestanding-safe on GCC/Clang — no libc link required.
 * Fall back to byte loops only on unknown compilers.
 */
#if defined(__GNUC__) || defined(__clang__)
#define exr_memcpy(d,s,n)  __builtin_memcpy((d),(s),(n))
#define exr_memset(d,c,n)  __builtin_memset((d),(c),(n))
#define exr_memmove(d,s,n) __builtin_memmove((d),(s),(n))
#define exr_memcmp(a,b,n)  __builtin_memcmp((a),(b),(n))
#define EXR_CRT_HAS_BUILTIN_MEM 1
#elif defined(_MSC_VER)
  void* __cdecl memcpy(void*, const void*, size_t);
  void* __cdecl memset(void*, int, size_t);
  void* __cdecl memmove(void*, const void*, size_t);
  int   __cdecl memcmp(const void*, const void*, size_t);
#define exr_memcpy(d,s,n)  memcpy((d),(s),(n))
#define exr_memset(d,c,n)  memset((d),(c),(n))
#define exr_memmove(d,s,n) memmove((d),(s),(n))
#define exr_memcmp(a,b,n)  memcmp((a),(b),(n))
#define EXR_CRT_HAS_BUILTIN_MEM 1
#else
/* Fallback: portable byte-at-a-time implementations */
static void* exr_memcpy(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    while (n--) *d++ = *s++;
    return dst;
}
static void* exr_memset(void* dst, int c, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    while (n--) *d++ = (uint8_t)c;
    return dst;
}
static void* exr_memmove(void* dst, const void* src, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    if (d == s || n == 0) return dst;
    if (d < s || d >= s + n) {
        return exr_memcpy(dst, src, n);
    }
    d += n;
    s += n;
    while (n--) { *--d = *--s; }
    return dst;
}
static int exr_memcmp(const void* a, const void* b, size_t n) {
    const uint8_t* pa = (const uint8_t*)a;
    const uint8_t* pb = (const uint8_t*)b;
    while (n--) {
        if (*pa != *pb) return (int)*pa - (int)*pb;
        pa++; pb++;
    }
    return 0;
}
#endif /* fallback mem functions */

/* ============================================================================
 * String Functions
 * ============================================================================ */

static size_t exr_strlen(const char* s) {
    const char* p = s;
    while (*p) p++;
    return (size_t)(p - s);
}

static int exr_strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((unused))
#endif
static int exr_strncmp(const char* a, const char* b, size_t n) {
    if (n == 0) return 0;
    while (n > 1 && *a && *a == *b) { a++; b++; n--; }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

/*
 * exr_strncpy_s — Secure bounded string copy.
 *
 * Copies at most (dst_size - 1) chars from src to dst.
 * Always NUL-terminates if dst_size > 0.
 * Returns 0 on success, non-zero on truncation or error.
 */
static int exr_strncpy_s(char* dst, size_t dst_size, const char* src, size_t count) {
    if (!dst || dst_size == 0) return -1;
    if (!src) { dst[0] = '\0'; return -1; }

    size_t max_copy = dst_size - 1;
    if (count < max_copy) max_copy = count;

    size_t i;
    for (i = 0; i < max_copy && src[i] != '\0'; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';

    /* Non-zero if source was truncated */
    return (src[i] != '\0') ? 1 : 0;
}

/*
 * exr_strcpy_s — Secure string copy (unbounded source, bounded destination).
 *
 * Copies src to dst, up to (dst_size - 1) chars. Always NUL-terminates.
 * Returns 0 on success, non-zero on truncation.
 */
static int exr_strcpy_s(char* dst, size_t dst_size, const char* src) {
    return exr_strncpy_s(dst, dst_size, src, dst_size);
}

/* ============================================================================
 * Integer-to-String Conversion Helpers
 * ============================================================================ */

/* Write unsigned decimal to buffer. Returns number of chars written. */
static int exr_utoa(char* buf, size_t buf_size, uint64_t val) {
    if (buf_size == 0) return 0;

    char tmp[20]; /* max uint64 is 20 digits */
    int len = 0;

    if (val == 0) {
        tmp[len++] = '0';
    } else {
        while (val > 0) {
            tmp[len++] = '0' + (char)(val % 10);
            val /= 10;
        }
    }

    int written = 0;
    for (int i = len - 1; i >= 0 && (size_t)written < buf_size - 1; i--) {
        buf[written++] = tmp[i];
    }
    buf[written] = '\0';
    return written;
}

/* Write signed decimal to buffer. Returns number of chars written. */
static int exr_itoa(char* buf, size_t buf_size, int64_t val) {
    if (buf_size == 0) return 0;
    if (val < 0 && buf_size > 1) {
        buf[0] = '-';
        return 1 + exr_utoa(buf + 1, buf_size - 1, (uint64_t)(-val));
    }
    return exr_utoa(buf, buf_size, (uint64_t)val);
}

/* Write unsigned hex to buffer. Returns number of chars written. */
static int exr_utoa_hex(char* buf, size_t buf_size, uint64_t val) {
    if (buf_size == 0) return 0;
    static const char hex[] = "0123456789abcdef";

    char tmp[16];
    int len = 0;

    if (val == 0) {
        tmp[len++] = '0';
    } else {
        while (val > 0) {
            tmp[len++] = hex[val & 0xF];
            val >>= 4;
        }
    }

    int written = 0;
    for (int i = len - 1; i >= 0 && (size_t)written < buf_size - 1; i--) {
        buf[written++] = tmp[i];
    }
    buf[written] = '\0';
    return written;
}

/* ============================================================================
 * Simplified snprintf Replacement
 *
 * Supports: %s, %d, %i, %u, %x, %p, %c, %lu, %llu, %ld, %lld, %%
 * Supports: field width for %s (e.g., %s only, no padding)
 * Does NOT support: floating point, padding, precision, flags
 * Always NUL-terminates. Returns chars that would have been written.
 * ============================================================================ */

/* stdarg.h is a freestanding header per the C standard */
#include <stdarg.h>

static int exr_vsnprintf_va(char* buf, size_t buf_size, const char* fmt, va_list args) {
    if (!buf || buf_size == 0 || !fmt) return 0;

    char* out = buf;
    char* end = buf + buf_size - 1; /* reserve space for NUL */
    const char* p = fmt;
    int total = 0; /* total chars that would be written */

    while (*p) {
        if (*p != '%') {
            if (out < end) *out++ = *p;
            total++;
            p++;
            continue;
        }

        p++; /* skip '%' */

        /* Handle %% */
        if (*p == '%') {
            if (out < end) *out++ = '%';
            total++;
            p++;
            continue;
        }

        /* Parse length modifier */
        int is_long = 0;
        int is_longlong = 0;
        if (*p == 'l') {
            p++;
            is_long = 1;
            if (*p == 'l') { p++; is_longlong = 1; }
        }

        /* Conversion */
        char tmp_buf[24]; /* large enough for any 64-bit number */
        const char* str = NULL;
        int str_len = 0;

        switch (*p) {
            case 's': {
                str = va_arg(args, const char*);
                if (!str) str = "(null)";
                str_len = (int)exr_strlen(str);
                p++;
                break;
            }
            case 'd':
            case 'i': {
                int64_t val;
                if (is_longlong) val = va_arg(args, long long);
                else if (is_long) val = va_arg(args, long);
                else val = va_arg(args, int);
                str_len = exr_itoa(tmp_buf, sizeof(tmp_buf), val);
                str = tmp_buf;
                p++;
                break;
            }
            case 'u': {
                uint64_t val;
                if (is_longlong) val = va_arg(args, unsigned long long);
                else if (is_long) val = va_arg(args, unsigned long);
                else val = va_arg(args, unsigned int);
                str_len = exr_utoa(tmp_buf, sizeof(tmp_buf), val);
                str = tmp_buf;
                p++;
                break;
            }
            case 'x': {
                uint64_t val;
                if (is_longlong) val = va_arg(args, unsigned long long);
                else if (is_long) val = va_arg(args, unsigned long);
                else val = va_arg(args, unsigned int);
                str_len = exr_utoa_hex(tmp_buf, sizeof(tmp_buf), val);
                str = tmp_buf;
                p++;
                break;
            }
            case 'p': {
                uintptr_t val = (uintptr_t)va_arg(args, void*);
                tmp_buf[0] = '0';
                tmp_buf[1] = 'x';
                int hex_len = exr_utoa_hex(tmp_buf + 2, sizeof(tmp_buf) - 2, (uint64_t)val);
                str_len = 2 + hex_len;
                str = tmp_buf;
                p++;
                break;
            }
            case 'c': {
                tmp_buf[0] = (char)va_arg(args, int);
                tmp_buf[1] = '\0';
                str = tmp_buf;
                str_len = 1;
                p++;
                break;
            }
            default: {
                /* Unknown format: output literally */
                if (out < end) *out++ = '%';
                total++;
                if (out < end) *out++ = *p;
                total++;
                p++;
                continue;
            }
        }

        /* Copy formatted string to output */
        for (int i = 0; i < str_len; i++) {
            if (out < end) *out++ = str[i];
            total++;
        }
    }

    *out = '\0';
    return total;
}

static int exr_snprintf(char* buf, size_t buf_size, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int ret = exr_vsnprintf_va(buf, buf_size, fmt, args);
    va_end(args);
    return ret;
}

/* ============================================================================
 * Boolean type (freestanding)
 * ============================================================================ */

#ifndef __bool_true_false_are_defined
#  if !defined(__cplusplus) && !defined(bool)
#    if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L
       /* C23: bool, true, false are keywords */
#    elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L
#      include <stdbool.h>
#    else
       typedef int bool;
#      define true 1
#      define false 0
#    endif
#  endif
#endif

#ifdef __cplusplus
}
#endif

#endif /* TINYEXR_CRT_H_ */
