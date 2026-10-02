/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 ZioZoni95
 *
 * Part of ZoniStation One, a PlayStation 1 emulator.
 * See LICENSE for the full licence text and THIRD-PARTY.md for the
 * components of this project that have other authors.
 */
/*
 * dma_test.c - unit test for dma_channel_progress() in src/core/dma.c, the
 * MADR/BCR writeback of psx-spx ps1/system/dmachannels.md:
 *   :23-29  SyncMode 0 leaves MADR alone unless chopping is on; SyncMode 1
 *           holds the start of the current block, then the end address
 *   :30-32  MADR bits 0-1 keep whatever was written
 *   :52     0 in BS/BA/BC means 10000h
 *   :56-58  SyncMode 1 decrements BA to zero, SyncMode 0 with chopping
 *           decrements BC to zero, nothing else changes BCR
 */
#include <stdio.h>
#include "log.h"

void log_print(LogCategory category, LogLevel level, const char* format, ...) {
    (void)category; (void)level; (void)format;
}
LogLevel log_get_current_level(void) { return LOG_LEVEL_SILENT; }
void lua_debug_notify(const char* event_name) { (void)event_name; }

#include "../src/core/dma.c"

void interconnect_set_irq_line(Interconnect* inter, uint32_t irq, bool level) {
    (void)inter; (void)irq; (void)level;
}

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

static DmaChannel chan(DmaSync sync, uint32_t madr, uint16_t bs, uint16_t ba, bool dec, bool chop) {
    DmaChannel ch = {0};
    ch.sync = sync; ch.base_addr = madr; ch.block_size = bs; ch.block_count = ba;
    ch.step = dec ? DECREMENT : INCREMENT; ch.chopping = chop; ch.enable = true;
    return ch;
}

int main(void) {
    /* SyncMode 1, 2 blocks of 16 words from 10000h: the case of the hardware
     * test in tests/hw/dma_test.c. */
    DmaChannel ch = chan(REQUEST, 0x10000, 16, 2, false, false);
    dma_channel_progress(&ch, 0x10000 + 32 * 4, 0);
    CHECK(ch.base_addr == 0x10080 && ch.block_count == 0 && ch.block_size == 16,
          "SyncMode 1 end: MADR %06x BS %u BA %u, want 010080h 16 0",
          ch.base_addr, ch.block_size, ch.block_count);

    /* Mid-transfer: 20 of 32 words moved, the second block 4 words in. MADR is
     * the start of the current block (10040h), one block not finished. */
    ch = chan(REQUEST, 0x10000, 16, 2, false, false);
    dma_channel_progress(&ch, 0x10000 + 20 * 4, 12);
    CHECK(ch.base_addr == 0x10040 && ch.block_count == 1,
          "SyncMode 1 mid-block: MADR %06x BA %u, want 010040h 1", ch.base_addr, ch.block_count);

    /* On a block boundary: 16 of 32 moved, MADR at the next block. */
    ch = chan(REQUEST, 0x10000, 16, 2, false, false);
    dma_channel_progress(&ch, 0x10040, 16);
    CHECK(ch.base_addr == 0x10040 && ch.block_count == 1,
          "SyncMode 1 at a block edge: MADR %06x BA %u", ch.base_addr, ch.block_count);

    /* Decrementing, with MADR bits 0-1 as written. */
    ch = chan(REQUEST, 0x20003, 4, 3, true, false);
    dma_channel_progress(&ch, 0x20000 - 5 * 4, 7);   /* 5 of 12 moved: block 2, 1 word in */
    CHECK(ch.base_addr == ((0x20000 - 4 * 4) | 3) && ch.block_count == 2,
          "SyncMode 1 decrementing: MADR %06x BA %u, want %06x 2",
          ch.base_addr, ch.block_count, (0x20000 - 16) | 3);

    /* BA written as 0 means 10000h blocks; untouched it reads 0 again. */
    ch = chan(REQUEST, 0x1000, 1, 0, false, false);
    dma_channel_progress(&ch, 0x1000, 0x10000);
    CHECK(ch.block_count == 0 && ch.base_addr == 0x1000,
          "BA=0 (10000h) with nothing moved: BA %u MADR %06x", ch.block_count, ch.base_addr);

    /* SyncMode 0 without chopping: neither register moves. */
    ch = chan(MANUAL, 0x8000, 0x100, 1, false, false);
    dma_channel_progress(&ch, 0x8400, 0);
    CHECK(ch.base_addr == 0x8000 && ch.block_size == 0x100 && ch.block_count == 1,
          "SyncMode 0: MADR %06x BC %u changed", ch.base_addr, ch.block_size);

    /* SyncMode 0 with chopping: MADR follows, BC counts down to zero. */
    ch = chan(MANUAL, 0x8000, 0x100, 1, false, true);
    dma_channel_progress(&ch, 0x8400, 0);
    CHECK(ch.base_addr == 0x8400 && ch.block_size == 0 && ch.block_count == 1,
          "SyncMode 0 chopped: MADR %06x BC %u, want 008400h 0", ch.base_addr, ch.block_size);

    /* SyncMode 2 is the caller's business. */
    ch = chan(LINKED_LIST, 0x4000, 0, 0, false, false);
    dma_channel_progress(&ch, 0x9999, 0);
    CHECK(ch.base_addr == 0x4000, "SyncMode 2 touched MADR: %06x", ch.base_addr);

    /* The address counter is 24 bits. */
    ch = chan(REQUEST, 0xFFFFF0, 4, 1, false, false);
    dma_channel_progress(&ch, 0x1000000, 0);
    CHECK(ch.base_addr == 0, "24-bit wrap: MADR %08x", ch.base_addr);

    printf("dma_test: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
