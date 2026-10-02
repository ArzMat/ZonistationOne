/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 ZioZoni95
 *
 * Part of ZoniStation One, a PlayStation 1 emulator.
 * See LICENSE for the full licence text and THIRD-PARTY.md for the
 * components of this project that have other authors.
 */
#ifndef MDEC_H
#define MDEC_H

/*
 * MDEC — Macroblock Decoder
 * PSX-SPX: DOCS/macroblockdecodermdec.md
 * Decode stages written from DOCS/macroblockdecodermdec.md (rl_decode_block,
 * real_idct_core, yuv_to_rgb, y_to_mono). See THIRD-PARTY.md.
 *
 * Registers:
 *   0x1F801820 (W): Command/Parameter   (R): Decoded Data
 *   0x1F801824 (W): Control/Reset       (R): Status
 *
 * Status register bits (per PSX-SPX):
 *   31: data-out FIFO empty
 *   30: data-in FIFO full
 *   29: command busy
 *   28: data-in request  (DMA0 should transfer)
 *   27: data-out request (DMA1 should transfer)
 *   26-25: output depth  (0=4bit 1=8bit 2=24bit 3=15bit)
 *   24: output signed
 *   23: output bit15
 *   18-16: current block index
 *   15-0: parameter words remaining minus 1 (FFFFh = none; the MDEC(0)
 *         count, or 0000h after reset, while idle)
 */

#include <stdint.h>
#include <stdbool.h>

#define MDEC_IN_FIFO_HW   2048   /* halfword capacity of input FIFO */
#define MDEC_OUT_FIFO_W    768   /* word capacity of output FIFO */
#define MDEC_NUM_BLOCKS      6   /* Cr, Cb, Y1, Y2, Y3, Y4 */

typedef enum {
    MDEC_ST_IDLE = 0,
    MDEC_ST_DECODING,
    MDEC_ST_WRITING,
    MDEC_ST_SET_QTABLE,
    MDEC_ST_SET_SCALE,
    MDEC_ST_NOCOMMAND    /* no longer entered: MDEC(0)/(4..7) take no parameters.
                          * Kept so a state saved by an older build still loads. */
} MdecDecodeState;

typedef struct Mdec {
    MdecDecodeState decode_state;

    /* Command/output parameters */
    uint8_t  output_depth;   /* 0=4bit 1=8bit 2=24bit 3=15bit */
    bool     output_signed;
    uint8_t  output_bit15;
    bool     enable_dma_in;
    bool     enable_dma_out;

    /* Input FIFO (halfwords) */
    uint16_t in_buf[MDEC_IN_FIFO_HW];
    uint32_t in_head, in_tail, in_count;

    /* Output FIFO (32-bit words) */
    uint32_t out_buf[MDEC_OUT_FIFO_W];
    uint32_t out_head, out_tail, out_count;

    /* Decode state */
    uint32_t remaining_halfwords;
    uint32_t current_block;        /* 0=Cr 1=Cb 2..5=Y1..Y4 */
    uint32_t current_coefficient;  /* 0..63; 64 = start of new block */
    uint32_t current_q_scale;

    /* Six decoded 8×8 blocks (Cr, Cb, Y1..Y4) */
    int16_t  blocks[MDEC_NUM_BLOCKS][64];

    /* Assembled 16×16 RGB macroblock (0x00BBGGRR per pixel) */
    uint32_t block_rgb[256];

    /* Quantization tables (set by command 0x02) */
    uint8_t  iq_y[64];    /* luma */
    uint8_t  iq_uv[64];   /* chroma */

    /* IDCT scale table (set by command 0x03; 64 × int16) */
    int16_t  scale_table[64];
} Mdec;

/* Public API */
void     mdec_init(Mdec* m);
uint32_t mdec_read(Mdec* m, uint32_t addr);
void     mdec_write(Mdec* m, uint32_t addr, uint32_t value);
void     mdec_dma_in(Mdec* m, uint32_t word);   /* DMA0: push one 32-bit word */
uint32_t mdec_dma_out(Mdec* m);                  /* DMA1: pull one 32-bit word */
bool     mdec_input_has_space(const Mdec* m);    /* true: room for >=1 more word (2 halfwords) */
bool     mdec_output_has_data(const Mdec* m);    /* true: at least 1 word ready to pop */
void     mdec_execute(Mdec* m);                  /* try to advance the decode state machine */

/* Re-derive the state that lives outside the saved Mdec span (the idle value
 * of status bits 15-0). Call after a savestate has been read into *m. */
void     mdec_state_restored(Mdec* m);

/* Macroblocks pushed out since boot — a UI counter, deliberately not part of
 * the savestated struct. */
uint32_t mdec_stat_macroblocks(void);

#endif /* MDEC_H */
