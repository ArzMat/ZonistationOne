/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 ZioZoni95
 *
 * Part of ZoniStation One, a PlayStation 1 emulator.
 * See LICENSE for the full licence text and THIRD-PARTY.md for the
 * components of this project that have other authors.
 */
#ifndef VRAM_H
#define VRAM_H

#include <stdint.h> // For uint8_t, uint16_t, uint32_t

// Define the dimensions and size of the PlayStation's VRAM
// 1024 pixels wide, 512 pixels high, 16 bits (2 bytes) per pixel
#define VRAM_WIDTH 1024
#define VRAM_HEIGHT 512
#define VRAM_BPP 2 // Bytes per pixel
#define VRAM_SIZE (VRAM_WIDTH * VRAM_HEIGHT * VRAM_BPP) // 1 Megabyte

// Structure to hold the VRAM data
typedef struct {
    uint8_t data[VRAM_SIZE]; // Buffer for the 1MB VRAM content
} Vram;

// --- Function Prototypes ---

/**
 * @brief Initializes the VRAM memory (e.g., fills with zeros or a pattern).
 * @param vram Pointer to the Vram struct to initialize.
 */
void vram_init(Vram* vram);

/**
 * @brief Reads a 32-bit value from VRAM at the specified byte offset (little-endian).
 * Note: VRAM is typically accessed 16 bits at a time.
 * @param vram Pointer to the Vram instance.
 * @param offset The byte offset within VRAM.
 * @return The 32-bit value read.
 */
uint32_t vram_load32(Vram* vram, uint32_t offset);

/**
 * @brief Writes a 32-bit value to VRAM at the specified byte offset (little-endian).
 * Note: VRAM is typically accessed 16 bits at a time.
 * @param vram Pointer to the Vram instance.
 * @param offset The byte offset within VRAM.
 * @param value The 32-bit value to write.
 */
void vram_store32(Vram* vram, uint32_t offset, uint32_t value);

/**
 * @brief Reads a 16-bit value (pixel) from VRAM at the specified byte offset (little-endian).
 * This is the primary access method for pixel data.
 * @param vram Pointer to the Vram instance.
 * @param offset The byte offset within VRAM (should be 16-bit aligned).
 * @return The 16-bit value read.
 */
uint16_t vram_load16(Vram* vram, uint32_t offset);

/**
 * @brief Writes a 16-bit value (pixel) to VRAM at the specified byte offset (little-endian).
 * This is the primary access method for pixel data.
 * @param vram Pointer to the Vram instance.
 * @param offset The byte offset within VRAM (should be 16-bit aligned).
 * @param value The 16-bit value to write.
 */
void vram_store16(Vram* vram, uint32_t offset, uint16_t value);

/**
 * @brief Reads an 8-bit value from VRAM at the specified byte offset.
 * @param vram Pointer to the Vram instance.
 * @param offset The byte offset within VRAM.
 * @return The 8-bit value read.
 */
uint8_t vram_load8(Vram* vram, uint32_t offset);

/**
 * @brief Writes an 8-bit value to VRAM at the specified byte offset.
 * @param vram Pointer to the Vram instance.
 * @param offset The byte offset within VRAM.
 * @param value The 8-bit value to write.
 */
void vram_store8(Vram* vram, uint32_t offset, uint8_t value);

/* --- Rectangles that wrap at the VRAM edges ---------------------------------
 * Copy, fill and upload rectangles that run past the right or bottom edge wrap
 * to the opposite edge, "without any carry-out from X to Y, nor from Y to X"
 * (psx-spx gpu/memory-transfer-commands.md:95-98). The renderers take plain
 * in-bounds rectangles, so a wrapped one is handed to them as up to four. */
typedef struct {
    uint16_t x, y, w, h;
} VramRect;

/* Split (x,y,w,h) into in-bounds pieces: x and y are taken modulo the VRAM size,
 * w is clamped to 1..1024 and h to 1..512. Returns the number of pieces (0 for
 * an empty rectangle, at most 4); out[0] always starts at (x,y). */
int vram_split_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, VramRect out[4]);


#endif // VRAM_H