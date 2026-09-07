/*
 * tocio - freestanding smoke test. Links the freestanding-compiled core objects
 * (no -lm) and drives them through a custom static-arena allocator, proving the
 * engine has no hidden libc/libm dependency. The harness itself is hosted.
 *
 * Copyright (c) 2014-2026 Syoyo Fujita and TinyEXR authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "toc_internal.h" /* exercises the libm-free core directly */

#include <stdio.h>

/* A trivial bump allocator over a fixed static buffer (no malloc). */
static unsigned char g_pool[1 << 20];
static size_t g_used = 0;
static void *pool_alloc(void *user, size_t size) {
    size_t a = (size + 15u) & ~(size_t)15u;
    (void)user;
    if (g_used + a > sizeof(g_pool)) return NULL;
    {
        void *p = g_pool + g_used;
        g_used += a;
        return p;
    }
}
static void pool_free(void *user, void *ptr) {
    (void)user;
    (void)ptr;
}

int main(void) {
    toc_allocator alloc;
    toc_sb sb;
    char *s;
    float l2;
    alloc.user = NULL;
    alloc.alloc = pool_alloc;
    alloc.free = pool_free;

    /* libm-free transcendental: log2(8) == 3. */
    l2 = toc_log2f(8.0f);
    if (l2 < 2.999f || l2 > 3.001f) {
        printf("FAIL: toc_log2f(8)=%f\n", (double)l2);
        return 1;
    }

    /* string builder + hex-float emit, all through the custom allocator. */
    toc_sb_init(&sb, &alloc);
    toc_sb_puts(&sb, "x=");
    toc_sb_hexfloat(&sb, 3.0f); /* 0x1.8p+1f */
    toc_sb_putc(&sb, ' ');
    toc_sb_decfloat(&sb, 0.5f); /* 0.5 */
    s = sb.buf;
    if (!s || sb.oom) {
        printf("FAIL: strbuf oom\n");
        return 1;
    }
    printf("ok: tocio freestanding core (\"%s\", pool=%zu B)\n", s, g_used);
    return 0;
}
