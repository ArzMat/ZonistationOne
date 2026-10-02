/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Part of ZoniStation One, a PlayStation 1 emulator.
 * See LICENSE for the full licence text and THIRD-PARTY.md for the
 * components of this project that have other authors.
 *
 * Which primitives must not share a draw call with their neighbours.
 *
 * Both renderers draw into the same image their fragment shader samples VRAM
 * from, and order the two with a barrier between draw calls (glTextureBarrier,
 * a Vulkan pipeline barrier). Inside one draw call there is no such order: a
 * fragment that reads a pixel another primitive of the same call wrote gets
 * either value. On hardware every primitive sees everything drawn before it.
 *
 * Merging consecutive primitives with identical state into one call is what
 * keeps the batch count low, so it stays the rule; the exception is a primitive
 * that may read what its own call draws:
 *   - the mask test (GP0(E6h).1) reads the destination pixel, which an earlier
 *     overlapping primitive may have just written;
 *   - a textured primitive whose texture page or CLUT lies inside the drawing
 *     area, the only place the call can write (psx-spx
 *     gpu/rendering-attributes.md:139-140).
 * Such a primitive gets a call of its own, with the barrier before it.
 */
#ifndef ZS1_RENDERER_ISOLATE_H
#define ZS1_RENDERER_ISOLATE_H

#include <stdbool.h>
#include <stdint.h>

static inline bool zs1_span_hits(int a0, int a1, int b0, int b1) {
    return a0 <= b1 && b0 <= a1;
}

/* Rectangle [x, x+w) x [y, y+h) against the inclusive drawing area; X wraps at
 * 1024 as texture pages near the right edge do. */
static inline bool zs1_rect_hits_area(int x, int y, int w, int h,
                                      int left, int top, int right, int bottom) {
    if (!zs1_span_hits(y, y + h - 1, top, bottom)) return false;
    if (zs1_span_hits(x, x + w - 1, left, right)) return true;
    return x + w > 1024 && zs1_span_hits(0, x + w - 1 - 1024, left, right);
}

/* tpage and clut are the attribute words the shaders decode (ps1.frag): page X
 * in 64-halfword steps (bits 0-3), page Y in 256-line steps (bit 4), depth in
 * bits 7-8; CLUT X in 16-halfword steps (bits 0-5), CLUT Y in bits 6-14. */
static inline bool zs1_prim_reads_drawn_area(bool mask_test, bool textured,
                                             uint16_t tpage, uint16_t clut,
                                             int left, int top, int right, int bottom) {
    if (right < left || bottom < top) return false;      /* empty area: draws nothing */
    if (mask_test) return true;
    if (!textured) return false;
    const unsigned depth = (tpage >> 7) & 3u;
    const int page_w = depth == 0 ? 64 : depth == 1 ? 128 : 256;
    if (zs1_rect_hits_area((tpage & 0xF) * 64, ((tpage >> 4) & 1) * 256, page_w, 256,
                           left, top, right, bottom))
        return true;
    if (depth < 2)
        return zs1_rect_hits_area((clut & 0x3F) * 16, (clut >> 6) & 0x1FF, depth == 0 ? 16 : 256, 1,
                                  left, top, right, bottom);
    return false;
}

#endif
