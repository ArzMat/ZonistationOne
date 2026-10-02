/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 ZioZoni95
 *
 * Part of ZoniStation One, a PlayStation 1 emulator.
 * See LICENSE for the full licence text and THIRD-PARTY.md for the
 * components of this project that have other authors.
 */
#include "vram.h"
#include <stdio.h>
#include "log.h"
#include <string.h> // For memset

/**
 * @brief Initializes the VRAM memory.
 * Fills with 0x00 as a default state.
 * @param vram Pointer to the Vram struct to initialize.
 */
void vram_init(Vram* vram) {
// Fill VRAM with zeros initially. Unlike RAM, VRAM often starts cleared.
    memset(vram->data, 0x00, VRAM_SIZE);
}

// Helper for bounds checking (inline for potential performance)
static inline int is_out_of_bounds(uint32_t offset, uint32_t access_size) {
    // Check if offset + (access_size - 1) exceeds the last valid index (VRAM_SIZE - 1)
    return offset > VRAM_SIZE - access_size;
}


// Reads a 32-bit value from VRAM (Little-Endian)
uint32_t vram_load32(Vram* vram, uint32_t offset) {
    if (offset % 4 != 0) {
         LOG_VRAM_WARN("[INTERCONNECT] VRAM Load32 unaligned: offset 0x%x", offset);
        // You might handle this differently (e.g., return garbage or specific behavior)
        // but for now, we'll proceed, acknowledging it's likely unintended.
    }
     if (is_out_of_bounds(offset, 4)) {
        LOG_VRAM_ERROR("[INTERCONNECT] VRAM Load32 out of bounds: offset 0x%x", offset);
        return 0; // Or handle error appropriately
    }
    uint32_t b0 = vram->data[offset + 0];
    uint32_t b1 = vram->data[offset + 1];
    uint32_t b2 = vram->data[offset + 2];
    uint32_t b3 = vram->data[offset + 3];
    return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
}

// Writes a 32-bit value to VRAM (Little-Endian)
void vram_store32(Vram* vram, uint32_t offset, uint32_t value) {
     if (offset % 4 != 0) {
         LOG_VRAM_WARN("[INTERCONNECT] VRAM Store32 unaligned: offset 0x%x", offset);
        // Proceeding, but acknowledging potential issue.
    }
     if (is_out_of_bounds(offset, 4)) {
        LOG_VRAM_ERROR("[INTERCONNECT] VRAM Store32 out of bounds: offset 0x%x", offset);
        return; // Or handle error appropriately
    }
    vram->data[offset + 0] = (uint8_t)(value & 0xFF);
    vram->data[offset + 1] = (uint8_t)((value >> 8) & 0xFF);
    vram->data[offset + 2] = (uint8_t)((value >> 16) & 0xFF);
    vram->data[offset + 3] = (uint8_t)((value >> 24) & 0xFF);
}

// Reads a 16-bit value from VRAM (Little-Endian) - Primary access method
uint16_t vram_load16(Vram* vram, uint32_t offset) {
     if (offset % 2 != 0) {
         LOG_VRAM_WARN("[INTERCONNECT] VRAM Load16 unaligned: offset 0x%x", offset);
        // Proceeding, but this is usually an error for pixel access.
     }
     if (is_out_of_bounds(offset, 2)) {
        LOG_VRAM_ERROR("[INTERCONNECT] VRAM Load16 out of bounds: offset 0x%x", offset);
        return 0;
    }
    uint16_t b0 = vram->data[offset + 0];
    uint16_t b1 = vram->data[offset + 1];
    return b0 | (b1 << 8);
}

// Writes a 16-bit value to VRAM (Little-Endian) - Primary access method
void vram_store16(Vram* vram, uint32_t offset, uint16_t value) {
     if (offset % 2 != 0) {
         LOG_VRAM_WARN("[INTERCONNECT] VRAM Store16 unaligned: offset 0x%x", offset);
        // Proceeding, but this is usually an error for pixel access.
    }
    if (is_out_of_bounds(offset, 2)) {
        LOG_VRAM_ERROR("[INTERCONNECT] VRAM Store16 out of bounds: offset 0x%x", offset);
        return;
    }
    vram->data[offset + 0] = (uint8_t)(value & 0xFF);
    vram->data[offset + 1] = (uint8_t)((value >> 8) & 0xFF);
}

// Reads an 8-bit value from VRAM
uint8_t vram_load8(Vram* vram, uint32_t offset) {
    if (is_out_of_bounds(offset, 1)) {
        LOG_VRAM_ERROR("[INTERCONNECT] VRAM Load8 out of bounds: offset 0x%x", offset);
        return 0;
    }
    return vram->data[offset];
}

// Writes an 8-bit value to VRAM
void vram_store8(Vram* vram, uint32_t offset, uint8_t value) {
    if (is_out_of_bounds(offset, 1)) {
        LOG_VRAM_ERROR("[INTERCONNECT] VRAM Store8 out of bounds: offset 0x%x", offset);
        return;
    }
    vram->data[offset] = value;
}
/* -------------------------------------------------------------------------
 * Wrapped rectangles
 * ---------------------------------------------------------------------- */

int vram_split_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, VramRect out[4]) {
    if (w == 0 || h == 0) return 0;
    x &= VRAM_WIDTH - 1;
    y &= VRAM_HEIGHT - 1;
    if (w > VRAM_WIDTH)  w = VRAM_WIDTH;
    if (h > VRAM_HEIGHT) h = VRAM_HEIGHT;
    const uint32_t w0 = (x + w > VRAM_WIDTH)  ? VRAM_WIDTH  - x : w;
    const uint32_t h0 = (y + h > VRAM_HEIGHT) ? VRAM_HEIGHT - y : h;
    const uint32_t w1 = w - w0, h1 = h - h0;
    int n = 0;
    out[n].x = (uint16_t)x; out[n].y = (uint16_t)y; out[n].w = (uint16_t)w0; out[n].h = (uint16_t)h0; n++;
    if (w1) { out[n].x = 0; out[n].y = (uint16_t)y; out[n].w = (uint16_t)w1; out[n].h = (uint16_t)h0; n++; }
    if (h1) { out[n].x = (uint16_t)x; out[n].y = 0; out[n].w = (uint16_t)w0; out[n].h = (uint16_t)h1; n++; }
    if (w1 && h1) { out[n].x = 0; out[n].y = 0; out[n].w = (uint16_t)w1; out[n].h = (uint16_t)h1; n++; }
    return n;
}

/* -------------------------------------------------------------------------
 * Raster tile map
 *
 * One bit per 16x16 tile, a row of 64 tiles per uint64_t. A file-static rather
 * than a Gpu field: Gpu is saved as raw spans and a new field would move every
 * byte after it, and the map is a cache that can always be rebuilt
 * conservatively (vram_raster_mark_all) instead of restored.
 * ---------------------------------------------------------------------- */

#define VRAM_TILE_SHIFT 4u
#define VRAM_TILES_Y    (VRAM_HEIGHT >> VRAM_TILE_SHIFT)   /* 32; 64 per row */

static uint64_t s_raster_tiles[VRAM_TILES_Y];

/* The last drawing area marked, valid until something clears a tile. */
static bool     s_area_marked = false;
static uint32_t s_area_l, s_area_t, s_area_r, s_area_b;

/* Bits tx0..tx1 inclusive, 0 <= tx0 <= tx1 <= 63. */
static inline uint64_t tile_bits(uint32_t tx0, uint32_t tx1) {
    uint64_t hi = (tx1 >= 63u) ? ~(uint64_t)0 : (((uint64_t)1 << (tx1 + 1u)) - 1u);
    uint64_t lo = ((uint64_t)1 << tx0) - 1u;
    return hi & ~lo;
}

void vram_raster_mark(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    VramRect p[4];
    int n = vram_split_rect(x, y, w, h, p);
    for (int i = 0; i < n; i++) {
        uint64_t bits = tile_bits(p[i].x >> VRAM_TILE_SHIFT,
                                  ((uint32_t)p[i].x + p[i].w - 1u) >> VRAM_TILE_SHIFT);
        uint32_t ty1 = ((uint32_t)p[i].y + p[i].h - 1u) >> VRAM_TILE_SHIFT;
        for (uint32_t ty = p[i].y >> VRAM_TILE_SHIFT; ty <= ty1; ty++)
            s_raster_tiles[ty] |= bits;
    }
}

void vram_raster_mark_area(uint32_t left, uint32_t top, uint32_t right, uint32_t bottom) {
    if (s_area_marked && left == s_area_l && top == s_area_t &&
        right == s_area_r && bottom == s_area_b)
        return;
    s_area_l = left; s_area_t = top; s_area_r = right; s_area_b = bottom;
    s_area_marked = true;
    /* An area with right < left or bottom < top clips everything away on
     * hardware, but the GL backend widens such a scissor to one pixel, so it
     * is marked as its first column or row: this map has to follow what the
     * renderer may write, not what it should. One that reaches past line 511 is
     * not something this 1 MB VRAM can address directly, so all lines are
     * marked rather than guessing how it folds. */
    if (right < left) right = left;
    if (bottom < top) bottom = top;
    if (right > VRAM_WIDTH - 1) right = VRAM_WIDTH - 1;
    if (left > VRAM_WIDTH - 1) return;
    if (bottom > VRAM_HEIGHT - 1) { top = 0; bottom = VRAM_HEIGHT - 1; }
    vram_raster_mark(left, top, right - left + 1u, bottom - top + 1u);
}

void vram_raster_mark_all(void) {
    for (uint32_t ty = 0; ty < VRAM_TILES_Y; ty++) s_raster_tiles[ty] = ~(uint64_t)0;
}

bool vram_raster_any(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    VramRect p[4];
    int n = vram_split_rect(x, y, w, h, p);
    for (int i = 0; i < n; i++) {
        uint64_t bits = tile_bits(p[i].x >> VRAM_TILE_SHIFT,
                                  ((uint32_t)p[i].x + p[i].w - 1u) >> VRAM_TILE_SHIFT);
        uint32_t ty1 = ((uint32_t)p[i].y + p[i].h - 1u) >> VRAM_TILE_SHIFT;
        for (uint32_t ty = p[i].y >> VRAM_TILE_SHIFT; ty <= ty1; ty++)
            if (s_raster_tiles[ty] & bits) return true;
    }
    return false;
}

void vram_raster_clear(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    VramRect p[4];
    int n = vram_split_rect(x, y, w, h, p);
    for (int i = 0; i < n; i++) {
        /* Only tiles the piece covers completely: round the start up and the
         * end down to a tile edge. The right and bottom VRAM edges are tile
         * edges, so a piece that reaches them covers its last tile. */
        uint32_t tx0 = ((uint32_t)p[i].x + 15u) >> VRAM_TILE_SHIFT;
        uint32_t tx1 = ((uint32_t)p[i].x + p[i].w) >> VRAM_TILE_SHIFT;   /* exclusive */
        uint32_t ty0 = ((uint32_t)p[i].y + 15u) >> VRAM_TILE_SHIFT;
        uint32_t ty1 = ((uint32_t)p[i].y + p[i].h) >> VRAM_TILE_SHIFT;   /* exclusive */
        if (tx1 <= tx0 || ty1 <= ty0) continue;
        uint64_t bits = tile_bits(tx0, tx1 - 1u);
        for (uint32_t ty = ty0; ty < ty1; ty++) s_raster_tiles[ty] &= ~bits;
        /* The next primitive has to mark its drawing area again, even if it is
         * the same area: these tiles may be inside it. */
        s_area_marked = false;
    }
}
