/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * GPU paths whose result only the GPU side can show: every check dirties the
 * area it reads with a rasterised primitive first, so GP0(C0h) has to fetch the
 * pixels from the renderer rather than from the CPU-side copy of VRAM.
 */
#include "hw.h"

#define RED_15   0x001Fu        /* 24-bit FF0000 (BGR 0000FF) after the fill's 8->5 bit drop */
#define GREEN_15 0x03E0u
#define BLUE_15  0x7C00u

static uint16_t g_buf[32 * 8];

/* Fill outside the drawing area, with GP0(E6h).0 set: psx-spx
 * gpu/i-o-ports-dma-channels-commands-vram.md:86-91 says the fill ignores both
 * the mask setting and the drawing area. A small rectangle drawn afterwards over
 * part of it forces the readback through the renderer. */
static void test_fill_outside_drawing_area(void) {
    gpu_set_drawing_area(0, 0, 319, 239);
    gp0(0xE6000001u);                              /* force bit 15 on drawn pixels */
    gp0(0x020000FFu);                              /* fill, colour BGR 0000FF = red */
    gp0((256u << 16) | 512u);
    gp0((32u << 16) | 64u);
    gp0(0xE6000000u);

    gpu_set_drawing_area(0, 0, 1023, 511);
    gpu_rect(0xFF0000u, 520, 260, 4, 4);           /* blue */

    int ok = gpu_read_rect(512, 256, 32, 8, g_buf);
    uint32_t bad = 0, first_bad = 0, want_first = 0;
    for (uint32_t y = 0; y < 8 && ok; y++) {
        for (uint32_t x = 0; x < 32; x++) {
            uint32_t vx = 512 + x, vy = 256 + y;
            uint16_t want = (vx >= 520 && vx < 524 && vy >= 260 && vy < 264) ? BLUE_15 : RED_15;
            uint16_t got = g_buf[y * 32 + x];
            if (got != want) {
                if (!bad) { first_bad = got; want_first = want; }
                bad++;
            }
        }
    }
    check("fill_outside_drawing_area", ok && !bad, ok ? first_bad : 0xDEADDEADu, want_first);
}

/* GP0(80h) copying pixels rasterised earlier in the same field: the copy has to
 * see them, which on the GPU-rendered backends means a readback that includes
 * the field's pending primitives. */
static void test_copy_of_rasterised_pixels(void) {
    gpu_set_drawing_area(0, 0, 1023, 511);
    gpu_rect(0x00FF00u, 600, 300, 8, 8);           /* green */
    gp0(0x80000000u);
    gp0((300u << 16) | 600u);                      /* source */
    gp0((300u << 16) | 700u);                      /* destination */
    gp0((8u << 16) | 8u);
    gpu_rect(0x0000FFu, 712, 300, 1, 1);           /* dirty the destination's area again */

    int ok = gpu_read_rect(700, 300, 8, 2, g_buf);
    uint32_t bad = 0, first_bad = 0;
    for (int i = 0; i < 16 && ok; i++)
        if (g_buf[i] != GREEN_15) { if (!bad) first_bad = g_buf[i]; bad++; }
    check("copy_of_rasterised_pixels", ok && !bad, ok ? first_bad : 0xDEADDEADu, GREEN_15);
}

/* GP0(A0h) upload crossing the right edge of VRAM wraps to x=0 on the same line
 * (psx-spx gpu/memory-transfer-commands.md:95-98). Both ends are dirtied with a
 * primitive afterwards so the readback comes from the renderer. */
static void test_upload_wraps_at_right_edge(void) {
    gp0(0xA0000000u);
    gp0((100u << 16) | 1020u);
    gp0((1u << 16) | 8u);
    gp0(0x00020001u);
    gp0(0x00040003u);
    gp0(0x00060005u);
    gp0(0x00080007u);
    gpu_set_drawing_area(0, 0, 1023, 511);
    gpu_rect(0xFFFFFFu, 1016, 101, 1, 1);
    gpu_rect(0xFFFFFFu, 8, 101, 1, 1);

    uint16_t right[4], left[4];
    int ok = gpu_read_rect(1020, 100, 4, 1, right) && gpu_read_rect(0, 100, 4, 1, left);
    uint32_t got = ok ? ((uint32_t)right[0] << 24 | (uint32_t)right[3] << 16 |
                         (uint32_t)left[0] << 8 | left[3]) : 0xDEADDEADu;
    check("upload_wraps_at_right_edge", got == 0x01040508u, got, 0x01040508u);
}

/* Two overlapping rectangles drawn with GP0(E6h) = 3 (set the mask bit, and
 * skip pixels whose mask bit is set): the second must leave the first one's
 * pixels alone (psx-spx gpu/rendering-attributes.md:162-169). Same state, so
 * a renderer that merges them into one draw call has to keep them apart. */
static void test_mask_test_between_overlapping_primitives(void) {
    gpu_set_drawing_area(0, 0, 1023, 511);
    gp0(0xE6000003u);
    gpu_rect(0x0000FFu, 800, 300, 8, 1);           /* red, sets bit 15 */
    gpu_rect(0xFF0000u, 804, 300, 8, 1);           /* blue, must skip 804..807 */
    gp0(0xE6000000u);
    uint16_t px[12];
    int ok = gpu_read_rect(800, 300, 12, 1, px);
    uint32_t bad = 0, first = 0, want_first = 0;
    for (int i = 0; i < 12 && ok; i++) {
        uint16_t want = (uint16_t)(0x8000u | (i < 8 ? RED_15 : BLUE_15));
        if (px[i] != want) { if (!bad) { first = px[i]; want_first = want; } bad++; }
    }
    check("mask_test_between_overlapping_primitives", ok && !bad, ok ? first : 0xDEADDEADu, want_first);
}

int main(void) {
    suite_begin("gpu");
    GP1 = 0x00000000u;                             /* GP1(00h) reset */
    gp0(0xE1000400u);                              /* drawing to the display area allowed */
    test_fill_outside_drawing_area();
    test_copy_of_rasterised_pixels();
    test_upload_wraps_at_right_edge();
    test_mask_test_between_overlapping_primitives();
    suite_end();
    return 0;
}
