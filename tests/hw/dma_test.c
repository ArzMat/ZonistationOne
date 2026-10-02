/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * DMA register writeback: a SyncMode 1 transfer leaves MADR at the end address
 * and BA at zero (psx-spx system/dmachannels.md:27-29, :56-58).
 */
#include "hw.h"

#define DMA2_MADR REG32(0xBF8010A0)
#define DMA2_BCR  REG32(0xBF8010A4)
#define DMA2_CHCR REG32(0xBF8010A8)

#define BLOCK_WORDS 16u
#define BLOCKS      2u

static volatile uint32_t g_src[BLOCK_WORDS * BLOCKS] __attribute__((aligned(4)));
static uint16_t g_back[64];

int main(void) {
    suite_begin("dma");
    GP1 = 0x00000000u;
    GP1 = 0x04000002u;                             /* GP1(04h): DMA direction CPU->GP0 */
    DPCR |= 0x00000800u;                           /* channel 2 enable */

    for (uint32_t i = 0; i < BLOCK_WORDS * BLOCKS; i++)
        g_src[i] = ((2 * i + 1) << 16) | (2 * i);

    gp0(0xA0000000u);                              /* 16x4 pixels = 32 words */
    gp0((200u << 16) | 200u);
    gp0((4u << 16) | 16u);

    uint32_t start = phys(g_src);
    DMA2_MADR = start;
    DMA2_BCR  = (BLOCKS << 16) | BLOCK_WORDS;
    DMA2_CHCR = 0x01000201u;                       /* from RAM, SyncMode 1, start */
    uint32_t n = POLL_LIMIT;
    while ((DMA2_CHCR & (1u << 24)) && --n) {}
    check("transfer_completes", n != 0, DMA2_CHCR, 0);

    uint32_t want_madr = start + BLOCK_WORDS * BLOCKS * 4;
    check("madr_holds_end_address", DMA2_MADR == want_madr, DMA2_MADR, want_madr);
    check("bcr_block_count_zero", (DMA2_BCR >> 16) == 0, DMA2_BCR, BLOCK_WORDS);

    int ok = gpu_read_rect(200, 200, 16, 4, g_back);
    uint32_t bad = 0;
    for (uint32_t i = 0; i < 64 && ok; i++)
        if (g_back[i] != (uint16_t)i) bad++;
    check("data_reached_vram", ok && !bad, bad, 0);

    suite_end();
    return 0;
}
