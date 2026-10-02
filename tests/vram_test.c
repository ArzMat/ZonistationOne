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
 *   vram_raster_*()    the tile map that decides whether GP0(80h)/(C0h) need a
 *                      readback. The property that matters is conservatism: a
 *                      pixel marked and not since covered by a clear must make
 *                      vram_raster_any() true for every rectangle containing it.
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

static void test_tiles_basic(void) {
    vram_raster_clear(0, 0, VRAM_WIDTH, VRAM_HEIGHT);
    CHECK(!vram_raster_any(0, 0, VRAM_WIDTH, VRAM_HEIGHT), "a full clear leaves nothing marked");

    vram_raster_mark_area(0, 0, 319, 239);
    CHECK(vram_raster_any(319, 239, 1, 1), "the drawing area's last pixel is marked");
    CHECK(vram_raster_any(0, 0, 1, 1), "the drawing area's first pixel is marked");
    CHECK(!vram_raster_any(320, 0, 64, 64), "right of the area (tile-aligned) is not marked");
    CHECK(!vram_raster_any(0, 240, 64, 16), "below the area (tile-aligned) is not marked");

    /* A clear that covers a tile only in part keeps it; one that covers it fully drops it. */
    vram_raster_clear(0, 0, 8, 16);
    CHECK(vram_raster_any(0, 0, 1, 1), "a partly covered tile must stay marked");
    vram_raster_clear(0, 0, 16, 16);
    CHECK(!vram_raster_any(0, 0, 16, 16), "a fully covered tile is cleared");
    CHECK(vram_raster_any(16, 0, 1, 1), "the next tile is untouched");

    /* After a clear, marking the same area again must re-mark it. */
    vram_raster_mark_area(0, 0, 319, 239);
    CHECK(vram_raster_any(0, 0, 1, 1), "re-marking the same area after a clear");

    /* An empty area is marked as its first column (the GL scissor is widened
     * to one pixel there); an area past line 511 marks every line. */
    vram_raster_clear(0, 0, VRAM_WIDTH, VRAM_HEIGHT);
    vram_raster_mark_area(100, 100, 50, 200);
    CHECK(vram_raster_any(100, 150, 1, 1) && !vram_raster_any(128, 0, 896, 512) &&
          !vram_raster_any(0, 0, 96, 512), "right < left marks the column at left only");
    vram_raster_mark_area(512, 600, 527, 700);
    CHECK(vram_raster_any(512, 0, 1, 1) && vram_raster_any(527, 511, 1, 1) && !vram_raster_any(528, 0, 16, 512),
          "an area below line 511 is taken as every line of its columns");

    /* Wrapped clears and queries. */
    vram_raster_mark_all();
    vram_raster_clear(1008, 496, 32, 32);   /* the four 16x16 corner tiles */
    CHECK(!vram_raster_any(1008, 496, 16, 16) && !vram_raster_any(0, 0, 16, 16) &&
          !vram_raster_any(0, 496, 16, 16) && !vram_raster_any(1008, 0, 16, 16),
          "a wrapped clear reaches all four corners");
    CHECK(vram_raster_any(1000, 490, 40, 40), "the wrapped query still sees the neighbours");
}

/* Model check: per-pixel truth against the tile map, random mark/clear/query. */
static void test_tiles_conservative(void) {
    static uint8_t truth[VRAM_HEIGHT][VRAM_WIDTH];
    memset(truth, 0, sizeof truth);
    vram_raster_clear(0, 0, VRAM_WIDTH, VRAM_HEIGHT);
    int missed = 0, stale_after_clear = 0;
    for (int op = 0; op < 3000; op++) {
        uint32_t x = rnd() & 0x3FF, y = rnd() & 0x1FF;
        uint32_t w = 1 + (rnd() % 200), h = 1 + (rnd() % 120);
        switch (rnd() % 3) {
        case 0:
            vram_raster_mark(x, y, w, h);
            for (uint32_t r = 0; r < h; r++)
                for (uint32_t c = 0; c < w; c++)
                    truth[(y + r) & 511][(x + c) & 1023] = 1;
            break;
        case 1:
            vram_raster_clear(x, y, w, h);
            for (uint32_t r = 0; r < h; r++)
                for (uint32_t c = 0; c < w; c++)
                    truth[(y + r) & 511][(x + c) & 1023] = 0;
            /* A tile-aligned clear must leave its own rectangle clean. */
            {
                uint32_t ax = x & ~15u, ay = y & ~15u;
                vram_raster_clear(ax, ay, 32, 32);
                for (uint32_t r = 0; r < 32; r++)
                    for (uint32_t c = 0; c < 32; c++)
                        truth[(ay + r) & 511][(ax + c) & 1023] = 0;
                if (vram_raster_any(ax, ay, 32, 32)) stale_after_clear++;
            }
            break;
        default: {
            int any_truth = 0;
            for (uint32_t r = 0; r < h && !any_truth; r++)
                for (uint32_t c = 0; c < w; c++)
                    if (truth[(y + r) & 511][(x + c) & 1023]) { any_truth = 1; break; }
            if (any_truth && !vram_raster_any(x, y, w, h)) missed++;
            break;
        }
        }
    }
    CHECK(missed == 0, "%d queries missed a marked pixel (the map must be conservative)", missed);
    CHECK(stale_after_clear == 0, "%d tile-aligned clears left their rectangle marked", stale_after_clear);
}

int main(void) {
    test_split();
    test_tiles_basic();
    test_tiles_conservative();
    printf("vram_test: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
