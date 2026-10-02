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
