/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Which primitives get a draw call of their own (src/gpu/renderer_isolate.h).
 */
#include <stdio.h>
#include "../src/gpu/renderer_isolate.h"

static int failures, checks;
#define CHECK(cond, msg) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg); } } while (0)

/* tpage attribute: page X (64-halfword steps), page Y (256 lines), depth. */
static uint16_t tp(int page_x, int page_y, int depth) {
    return (uint16_t)((page_x & 0xF) | ((page_y & 1) << 4) | ((depth & 3) << 7));
}
static uint16_t cl(int x16, int y) { return (uint16_t)((x16 & 0x3F) | ((y & 0x1FF) << 6)); }

int main(void) {
    /* Drawing area: a 320x240 back buffer at the top left. */
    const int L = 0, T = 0, R = 319, B = 239;

    CHECK(!zs1_prim_reads_drawn_area(false, false, 0, 0, L, T, R, B), "flat primitive merges");
    CHECK(zs1_prim_reads_drawn_area(true, false, 0, 0, L, T, R, B), "mask test isolates");
    CHECK(!zs1_prim_reads_drawn_area(true, false, 0, 0, 10, 0, 5, 239), "empty area never isolates");

    /* Page 5 (x 320..383) at y 0, 4-bit, CLUT at (320, 480): outside the area. */
    CHECK(!zs1_prim_reads_drawn_area(false, true, tp(5, 0, 0), cl(20, 480), L, T, R, B),
          "texture outside the area merges");
    /* Page 4 (x 256..319), 4-bit: inside the area. */
    CHECK(zs1_prim_reads_drawn_area(false, true, tp(4, 0, 0), cl(20, 480), L, T, R, B),
          "texture page inside the area isolates");
    /* Page 4 in 15-bit is 256 wide (256..511): still overlaps. */
    CHECK(zs1_prim_reads_drawn_area(false, true, tp(4, 0, 2), 0, L, T, R, B), "15-bit page width");
    /* Page 0 at y 256: below a 240-line area. */
    CHECK(!zs1_prim_reads_drawn_area(false, true, tp(0, 1, 0), cl(20, 480), L, T, R, B),
          "lower page row is outside");
    /* Texture outside, but its CLUT row inside the area. */
    CHECK(zs1_prim_reads_drawn_area(false, true, tp(5, 0, 0), cl(2, 100), L, T, R, B),
          "CLUT inside the area isolates");
    /* 15-bit has no CLUT: a CLUT word pointing inside is ignored. */
    CHECK(!zs1_prim_reads_drawn_area(false, true, tp(5, 0, 2), cl(2, 100), L, T, R, B),
          "15-bit ignores the CLUT");
    /* Page 15 in 15-bit spans x 960..1215 and wraps to 0..191. */
    CHECK(zs1_prim_reads_drawn_area(false, true, tp(15, 0, 2), 0, L, T, R, B), "page wraps at x 1024");

    printf("renderer_isolate_test: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
