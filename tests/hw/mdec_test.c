/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * MDEC command handling seen from the CPU: decode one macroblock, then decode
 * it again after a reset and after DMA-style FE00h padding, and require the same
 * output each time. No reference image is needed: the checks are that the
 * hardware state the documentation says survives does survive.
 */
#include "hw.h"

#define MB_WORDS_15BPP 128u                         /* 16x16 pixels, 2 per word */
#define CMD_DECODE_15BPP 0x38000000u                /* MDEC(1), depth 3 = 15bit */

/* Standard scale table (psx-spx cpu/mdec/macroblockdecodermdec.md, MDEC(3)). */
static const uint16_t k_scale[64] = {
    0x5A82,0x5A82,0x5A82,0x5A82,0x5A82,0x5A82,0x5A82,0x5A82,
    0x7D8A,0x6A6D,0x471C,0x18F8,0xE707,0xB8E3,0x9592,0x8275,
    0x7641,0x30FB,0xCF04,0x89BE,0x89BE,0xCF04,0x30FB,0x7641,
    0x6A6D,0xE707,0x8275,0xB8E3,0x471C,0x7D8A,0x18F8,0x9592,
    0x5A82,0xA57D,0xA57D,0x5A82,0x5A82,0xA57D,0xA57D,0x5A82,
    0x471C,0x8275,0x18F8,0x6A6D,0x9592,0xE707,0x7D8A,0xB8E3,
    0x30FB,0x89BE,0x7641,0xCF04,0xCF04,0x7641,0x89BE,0x30FB,
    0x18F8,0xB8E3,0x6A6D,0x8275,0x7D8A,0x9592,0x471C,0xE707 };

/* One colour macroblock, six blocks (Cr, Cb, Y1..Y4). Each block is a start
 * halfword (q_scale in bits 15-10, DC in 9-0), one AC coefficient (run in
 * 15-10, level in 9-0) and the FE00h end code: 18 halfwords, 9 words. */
static const uint16_t k_macroblock_hw[18] = {
    0x0410, 0x0C02, 0xFE00,     /* Cr: q 1, DC 010h; run 3, level 2 */
    0x0460, 0x07F0, 0xFE00,     /* Cb: DC 060h; run 1, level -16 */
    0x0440, 0x0C08, 0xFE00,     /* Y1 */
    0x0420, 0x03F8, 0xFE00,     /* Y2 */
    0x0480, 0x0C04, 0xFE00,     /* Y3 */
    0x0440, 0x0FF8, 0xFE00,     /* Y4 */
};
#define MB_PARAM_WORDS 9u

static uint32_t g_out[3][MB_WORDS_15BPP];

static void upload_tables(void) {
    MDEC_DATA = 0x40000001u;                       /* MDEC(2), luma + chroma tables */
    for (int i = 0; i < 32; i++) MDEC_DATA = 0x02020202u;
    MDEC_DATA = 0x60000000u;                       /* MDEC(3), scale table */
    for (int i = 0; i < 64; i += 2) MDEC_DATA = k_scale[i] | ((uint32_t)k_scale[i + 1] << 16);
}

/* Returns the number of words read before the output ran dry. */
static uint32_t decode(uint32_t* out) {
    MDEC_DATA = CMD_DECODE_15BPP | MB_PARAM_WORDS;
    for (uint32_t i = 0; i < MB_PARAM_WORDS; i++)
        MDEC_DATA = k_macroblock_hw[2 * i] | ((uint32_t)k_macroblock_hw[2 * i + 1] << 16);
    for (uint32_t i = 0; i < MB_WORDS_15BPP; i++) {
        uint32_t n = POLL_LIMIT;
        while ((MDEC_CTRL & (1u << 31)) && --n) {}  /* data-out FIFO empty */
        if (!n) return i;
        out[i] = MDEC_DATA;
    }
    return MB_WORDS_15BPP;
}

static uint32_t first_difference(const uint32_t* a, const uint32_t* b) {
    for (uint32_t i = 0; i < MB_WORDS_15BPP; i++)
        if (a[i] != b[i]) return i;
    return MB_WORDS_15BPP;
}

int main(void) {
    suite_begin("mdec");
    MDEC_CTRL = 0x80000000u;                       /* reset */
    upload_tables();

    uint32_t got = decode(g_out[0]);
    check("decode_complete", got == MB_WORDS_15BPP, got, MB_WORDS_15BPP);
    /* Bits 15-0 after a finished command: "(FFFFh=None)",
     * psx-spx cpu/mdec/macroblockdecodermdec.md:43. */
    check("status_words_remaining_none", (MDEC_CTRL & 0xFFFFu) == 0xFFFFu, MDEC_CTRL & 0xFFFFu, 0xFFFFu);

    /* The reset bit keeps the tables (macroblockdecodermdec.md:116-117). */
    MDEC_CTRL = 0x80000000u;
    got = decode(g_out[1]);
    uint32_t at = first_difference(g_out[0], g_out[1]);
    check("reset_keeps_tables", got == MB_WORDS_15BPP && at == MB_WORDS_15BPP,
          got == MB_WORDS_15BPP ? g_out[1][at % MB_WORDS_15BPP] : got,
          g_out[0][at % MB_WORDS_15BPP]);

    /* FE00h padding after a finished MDEC(1) (macroblockdecodermdec.md:77-79)
     * reads as command 7, which takes no parameters (:119-126). */
    MDEC_DATA = 0xFE00FE00u;
    MDEC_DATA = 0xFE00FE00u;
    got = decode(g_out[2]);
    at = first_difference(g_out[0], g_out[2]);
    check("padding_is_not_a_parameter_count", got == MB_WORDS_15BPP && at == MB_WORDS_15BPP,
          got, MB_WORDS_15BPP);

    suite_end();
    return 0;
}
