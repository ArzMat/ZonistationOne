/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 ZioZoni95
 *
 * Part of ZoniStation One, a PlayStation 1 emulator.
 * See LICENSE for the full licence text and THIRD-PARTY.md for the
 * components of this project that have other authors.
 */
/*
 * vram_test.c - unit test for the pure helpers in src/gpu/vram.c:
 *
 *   vram_split_rect()  a rectangle that runs past the right or bottom edge of
 *                      VRAM wraps to the opposite edge "without any carry-out
 *                      from X to Y, nor from Y to X" (psx-spx
 *                      gpu/memory-transfer-commands.md:95-98); the pieces must
 *                      cover exactly the wrapped pixel set, in bounds.
 */
#include <stdio.h>
#include <string.h>
#include "log.h"

void log_print(LogCategory category, LogLevel level, const char* format, ...) {
    (void)category; (void)level; (void)format;
}

#include "../src/gpu/vram.c"

static int g_fail = 0, g_checks = 0;

#define CHECK(cond, ...) do {                                   \
    g_checks++;                                                 \
    if (!(cond)) {                                              \
        g_fail++;                                               \
        printf("FAIL %s:%d: ", __FILE__, __LINE__);             \
        printf(__VA_ARGS__);                                    \
        printf("\n");                                           \
    }                                                           \
} while (0)

static uint32_t g_rng = 0x9E3779B9u;
static uint32_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }

static uint8_t s_pix[VRAM_HEIGHT][VRAM_WIDTH];

/* Does the union of the pieces equal the wrapped rectangle, with no overlap? */
static int split_covers_exactly(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    VramRect p[4];
    int n = vram_split_rect(x, y, w, h, p);
    memset(s_pix, 0, sizeof s_pix);
    for (int i = 0; i < n; i++) {
        if (p[i].w == 0 || p[i].h == 0) return 0;
        if ((uint32_t)p[i].x + p[i].w > VRAM_WIDTH || (uint32_t)p[i].y + p[i].h > VRAM_HEIGHT) return 0;
        for (uint32_t yy = p[i].y; yy < (uint32_t)p[i].y + p[i].h; yy++)
            for (uint32_t xx = p[i].x; xx < (uint32_t)p[i].x + p[i].w; xx++) {
                if (s_pix[yy][xx]) return 0;           /* overlap */
                s_pix[yy][xx] = 1;
            }
    }
    uint32_t ww = w > VRAM_WIDTH ? VRAM_WIDTH : w, hh = h > VRAM_HEIGHT ? VRAM_HEIGHT : h;
    uint32_t count = 0;
    for (uint32_t r = 0; r < hh; r++)
        for (uint32_t c = 0; c < ww; c++) {
            if (!s_pix[(y + r) & (VRAM_HEIGHT - 1)][(x + c) & (VRAM_WIDTH - 1)]) return 0;
            count++;
        }
    uint32_t total = 0;
    for (uint32_t r = 0; r < VRAM_HEIGHT; r++)
        for (uint32_t c = 0; c < VRAM_WIDTH; c++) total += s_pix[r][c];
    return total == count;
}

static void test_split(void) {
    VramRect p[4];
    CHECK(vram_split_rect(10, 10, 0, 5, p) == 0 && vram_split_rect(10, 10, 5, 0, p) == 0,
          "empty rectangles must give no pieces");
    CHECK(vram_split_rect(100, 50, 64, 32, p) == 1 && p[0].x == 100 && p[0].y == 50 &&
          p[0].w == 64 && p[0].h == 32, "an in-bounds rectangle is one piece");
    /* The case the GPU hardware test uses: 8 pixels from x=1020. */
    CHECK(vram_split_rect(1020, 100, 8, 1, p) == 2 && p[0].x == 1020 && p[0].w == 4 &&
          p[1].x == 0 && p[1].w == 4 && p[1].y == 100, "x=1020 w=8 splits 4+4");
    CHECK(vram_split_rect(0, 500, 16, 20, p) == 2 && p[0].h == 12 && p[1].y == 0 && p[1].h == 8,
          "y=500 h=20 splits 12+8");
    CHECK(vram_split_rect(1000, 500, 100, 100, p) == 4, "a corner rectangle splits in four");
    CHECK(vram_split_rect(0, 0, 1024, 512, p) == 1 && p[0].w == 1024 && p[0].h == 512,
          "all of VRAM is one piece");
    CHECK(vram_split_rect(1024 + 3, 512 + 7, 2, 2, p) == 1 && p[0].x == 3 && p[0].y == 7,
          "coordinates are taken modulo the VRAM size");

    int bad = 0;
    for (int i = 0; i < 400; i++) {
        uint32_t x = rnd() & 0x3FF, y = rnd() & 0x1FF;
        uint32_t w = 1 + (rnd() % 1024), h = 1 + (rnd() % 512);
        if (i % 4 == 0) { x = 1024 - 1 - (rnd() % 8); w = 1 + (rnd() % 40); }
        if (i % 5 == 0) { y = 512 - 1 - (rnd() % 8); h = 1 + (rnd() % 40); }
        if (!split_covers_exactly(x, y, w, h)) {
            if (bad < 5) printf("  split (%u,%u %ux%u) does not cover the wrapped rectangle exactly\n", x, y, w, h);
            bad++;
        }
    }
    CHECK(bad == 0, "%d of 400 random rectangles split wrongly", bad);
}

int main(void) {
    test_split();
    printf("vram_test: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
