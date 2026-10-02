/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 ZioZoni95
 *
 * Part of ZoniStation One, a PlayStation 1 emulator.
 * See LICENSE for the full licence text and THIRD-PARTY.md for the
 * components of this project that have other authors.
 */
/*
 * MDEC — Macroblock Decoder
 *
 * The decode stages are written from the hardware documentation in
 * `DOCS/macroblockdecodermdec.md`: rl_decode_block (:138-158),
 * real_idct_core (:192-212), yuv_to_rgb (:220-234), y_to_mono (:236-245) and
 * the zigzag/zagzig tables (:271-295). The FIFO plumbing and the incremental
 * state machine around them are this project's own — the documented pseudocode
 * decodes from a flat source buffer, while this has to make progress a
 * halfword at a time as DMA delivers them.
 *
 * Line numbers in the comments below that say "psx-spx" refer to the current
 * psx-spx layout, ps1/cpu/mdec/macroblockdecodermdec.md (abbreviated mdec.md):
 * rl_decode_block is at :146-166, real_idct_core at :200-226, yuv_to_rgb at
 * :228-244, y_to_mono at :246-255.
 */

#include "mdec.h"
#include "log.h"
#include "lua_debug.h"
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * FIFO helpers
 * ---------------------------------------------------------------------- */

static bool in_empty(const Mdec* m)    { return m->in_count == 0; }
static bool in_full(const Mdec* m)     { return m->in_count == MDEC_IN_FIFO_HW; }
static uint32_t in_space(const Mdec* m){ return MDEC_IN_FIFO_HW - m->in_count; }

static uint16_t in_pop(Mdec* m) {
    uint16_t v = m->in_buf[m->in_head];
    m->in_head = (m->in_head + 1) % MDEC_IN_FIFO_HW;
    m->in_count--;
    return v;
}
static void in_push(Mdec* m, uint16_t v) {
    m->in_buf[m->in_tail] = v;
    m->in_tail = (m->in_tail + 1) % MDEC_IN_FIFO_HW;
    m->in_count++;
}
static bool out_empty(const Mdec* m)   { return m->out_count == 0; }
/* The output FIFO is 768 words, not a power of two, so the index wraps with a
 * compare rather than a modulo: this runs once per output word, 128 or 192
 * times per macroblock. */
static void out_push(Mdec* m, uint32_t v) {
    m->out_buf[m->out_tail] = v;
    if (++m->out_tail == MDEC_OUT_FIFO_W) m->out_tail = 0;
    m->out_count++;
}
static uint32_t out_pop(Mdec* m) {
    uint32_t v = m->out_buf[m->out_head];
    if (++m->out_head == MDEC_OUT_FIFO_W) m->out_head = 0;
    m->out_count--;
    return v;
}

/* -------------------------------------------------------------------------
 * Status register
 * ---------------------------------------------------------------------- */

/* Status bits 15-0 while no MDEC(1/2/3) is running.
 *
 * psx-spx mdec.md:43 defines them as "Number of Parameter Words remaining minus
 * 1 (FFFFh=None)"; the reset value is 0000h (status=80040000h, :58); MDEC(0)
 * and MDEC(4..7) copy their command bits 0-15 there "without the minus 1
 * effect, and without actually expecting any parameters" (:120-126). While a
 * command runs the value follows from remaining_halfwords; once it is idle the
 * last of the three above has to be remembered somewhere.
 *
 * A file-static rather than a field in Mdec for the same reason as
 * g_mdec_macroblocks_out below: Mdec is saved as one sized span, and a new
 * field would make every existing state unloadable. mdec_state_restored()
 * resets it after a load. */
static uint16_t g_mdec_idle_low16 = 0;

/* Bit 30 is "Data-In Fifo Full (0=No, 1=Full, or Last word received)"
 * (psx-spx mdec.md:34). The last word of a command has been received once the
 * input FIFO holds everything the running command still has to consume. */
static bool mdec_last_word_received(const Mdec* m) {
    if (m->decode_state == MDEC_ST_IDLE) return false;
    return m->in_count >= m->remaining_halfwords;
}

/* Bits 18-16, "Current Block (0..3=Y1..Y4, 4=Cr, 5=Cb) (or for mono: always
 * 4=Y)" (psx-spx mdec.md:42). "If there's data in the output fifo, then the
 * Current Block bits are always set to the current output block number (ie.
 * Y1..Y4; or Y for mono) ... If the output fifo is empty, then the bits
 * indicate the currently processsed incoming block" (:45-50).
 *
 * The output FIFO here holds a whole macroblock already in the 16x16 order
 * DMA1 would produce, so the output block is taken from how far the macroblock
 * has been read: each quarter of it is one 8x8 block's worth of words. */
static uint32_t mdec_status_block(const Mdec* m) {
    if (!out_empty(m)) {
        if (m->output_depth <= 1) return 4u;
        uint32_t total  = (m->output_depth == 2) ? 192u : 128u;
        uint32_t popped = (m->out_count < total) ? total - m->out_count : 0u;
        return (popped * 4u) / total;
    }
    /* current_block: 0=Cr 1=Cb 2..5=Y1..Y4; mono decodes into block 0 -> 4=Y. */
    return (m->current_block + 4u) % (uint32_t)MDEC_NUM_BLOCKS;
}

/* Bits 15-0: the parameter words a running MDEC(1/2/3) still expects, minus 1
 * (psx-spx mdec.md:43). A word whose first halfword has been consumed and its
 * second not yet still counts as remaining, and 0 remaining reads FFFFh: the
 * command can be busy with no parameters left while its last macroblock waits
 * for the output FIFO to drain. */
static uint16_t mdec_status_low16(const Mdec* m) {
    if (m->decode_state == MDEC_ST_IDLE) return g_mdec_idle_low16;
    return (uint16_t)(((m->remaining_halfwords + 1u) >> 1) - 1u);
}

static uint32_t mdec_get_status(const Mdec* m) {
    uint32_t s = 0;
    if (out_empty(m))   s |= (1u << 31);  /* data_out_fifo_empty */
    if (in_full(m) || mdec_last_word_received(m))
                        s |= (1u << 30);  /* data_in_fifo_full, or last word received */
    if (m->decode_state != MDEC_ST_IDLE)
                        s |= (1u << 29);  /* command_busy */
    bool in_req  = m->enable_dma_in  && (in_space(m) >= 64);
    bool out_req = m->enable_dma_out && !out_empty(m);
    if (in_req)  s |= (1u << 28);         /* data_in_request  (DMA0) */
    if (out_req) s |= (1u << 27);         /* data_out_request (DMA1) */
    s |= ((uint32_t)m->output_depth & 3u) << 25;
    if (m->output_signed)  s |= (1u << 24);
    s |= ((uint32_t)m->output_bit15 & 1u) << 23;
    s |= mdec_status_block(m) << 16;
    s |= mdec_status_low16(m);
    return s;
}

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */
static inline int32_t sign_extend10(int32_t v) { return (v << 22) >> 22; }
static inline int32_t sign_extend9 (int32_t v) { return (v << 23) >> 23; }
static inline int32_t clamp_s32(int32_t v, int32_t lo, int32_t hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

/* Macroblocks pushed to the output FIFO since boot.
 *
 * A file-static rather than a field in Mdec: the savestate writes the struct as
 * one sized span and refuses a state whose section size does not match, so
 * adding a counter to it would invalidate every existing v6 state for the sake
 * of a number the UI reads. */
static uint32_t g_mdec_macroblocks_out = 0;

uint32_t mdec_stat_macroblocks(void) { return g_mdec_macroblocks_out; }

/* -------------------------------------------------------------------------
 * RLE decode — DOCS/macroblockdecodermdec.md:138-158 (rl_decode_block).
 *
 * Restructured to run incrementally: the documented loop reads straight from a
 * source pointer, this one resumes wherever the input FIFO ran dry and returns
 * true when one 8x8 block is complete.
 *
 * zagzig is the reversed zigzag order, generated by the rule at :292-295
 * ("for i=0 to 63, zagzig[zigzag[i]]=i") from the zigzag table at :271-282.
 * ---------------------------------------------------------------------- */
static const uint8_t zagzig[64] = {
     0, 1, 8,16, 9, 2, 3,10,17,24,32,25,18,11, 4, 5,
    12,19,26,33,40,48,41,34,27,20,13, 6, 7,14,21,28,
    35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,
    58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63
};

static bool mdec_decode_rle(Mdec* m, int16_t* blk, const uint8_t* qt) {
    if (m->current_coefficient == 64) {
        memset(blk, 0, 64 * sizeof(int16_t));

        /* Skip 0xFE00 padding halfwords */
        uint16_t n;
        for (;;) {
            if (in_empty(m) || m->remaining_halfwords == 0)
                return false;
            n = in_pop(m);
            m->remaining_halfwords--;
            if (n != 0xFE00) break;
        }

        m->current_coefficient = 0;
        m->current_q_scale = (n >> 10) & 0x3F;
        int32_t val = sign_extend10(n & 0x3FF) * (int32_t)qt[0];
        if (m->current_q_scale == 0)
            val = sign_extend10(n & 0x3FF) * 2;
        val = clamp_s32(val, -0x400, 0x3FF);
        if (m->current_q_scale > 0)
            blk[zagzig[0]] = (int16_t)val;
        else
            blk[0] = (int16_t)val;
    }

    while (!in_empty(m) && m->remaining_halfwords > 0) {
        uint16_t n = in_pop(m);
        m->remaining_halfwords--;

        m->current_coefficient += ((n >> 10) & 0x3Fu) + 1u;
        if (m->current_coefficient < 64) {
            int32_t val = (sign_extend10(n & 0x3FF) *
                           (int32_t)qt[m->current_coefficient] *
                           (int32_t)m->current_q_scale + 4) / 8;
            if (m->current_q_scale == 0)
                val = sign_extend10(n & 0x3FF) * 2;
            val = clamp_s32(val, -0x400, 0x3FF);
            if (m->current_q_scale > 0)
                blk[zagzig[m->current_coefficient]] = (int16_t)val;
            else
                blk[m->current_coefficient] = (int16_t)val;
        }

        if (m->current_coefficient >= 63) {
            m->current_coefficient = 64;
            return true;
        }
    }
    return false;
}

/* -------------------------------------------------------------------------
 * IDCT: real_idct_core, psx-spx mdec.md:200-226.
 *
 *   for pass = 0 to 1
 *     for x = 0 to 7, for y = 0 to 7
 *       sum = SUM over z of src[y+z*8] * (scaletable[x+z*8] / 8)
 *       dst[x+y*8] = (sum + 0FFFh) / 2000h
 *     swap(src, dst)
 *
 * The "+0FFFh then /2000h" strips the fractional bits and rounds up when the
 * fraction was above a half; the documentation notes (:220-226) that hardware
 * is only approximately this, so exactness beyond it is not claimed. The shift
 * is arithmetic rather than a C division so negative sums floor the way a
 * hardware shifter does instead of truncating toward zero.
 *
 * "scaletable/8": "the hardware does actually use only the upper 13bit of
 * those 16bit values" (mdec.md:317-318). Keeping the upper 13 bits of a signed
 * 16-bit value is a floor division by 8, which is what g_scale_k holds. The C
 * "/ 8" used before truncated toward zero, so for 28 of the 64 standard
 * entries (the negative ones whose low 3 bits are not zero) the result
 * depended on the low 3 bits the hardware ignores, one step off.
 *
 * Range, and why int32 is exact. The RLE stage saturates every coefficient to
 * -400h..3FFh (mdec_decode_rle, mdec.md:156), so |src| <= 1024 in pass 0, and
 * |scale/8| <= 4096 for any 16-bit table. Pass 0: |sum| <= 8*1024*4096 = 2^25,
 * so |dst| <= (2^25+0FFFh)>>13 = 4096. Pass 1: |sum| <= 8*4096*4096 = 2^27,
 * so the output is within +-2^14 = 16384 and fits the int16_t block. Both sums
 * plus 0FFFh stay far below 2^31, so 32-bit accumulators give exactly what the
 * 64-bit ones did.
 * ---------------------------------------------------------------------- */

/* scale_table[i] >> 3 (the upper 13 bits), rebuilt whenever the table it was
 * taken from differs from the live one. Kept out of Mdec so the saved span
 * does not change; the memcmp makes it follow a savestate load, a reset or an
 * MDEC(3) without anyone having to remember to refresh it. */
static int16_t g_scale_src[64];
static int32_t g_scale_k[64];
static bool    g_scale_k_valid = false;

static void mdec_refresh_scale_k(const Mdec* m) {
    if (g_scale_k_valid && memcmp(g_scale_src, m->scale_table, sizeof g_scale_src) == 0)
        return;
    for (int i = 0; i < 64; i++) {
        int32_t s = m->scale_table[i];
        int32_t q = s / 8;
        if (s < 0 && (s % 8) != 0) q -= 1;   /* floor, i.e. drop the low 3 bits */
        g_scale_k[i] = q;
    }
    memcpy(g_scale_src, m->scale_table, sizeof g_scale_src);
    g_scale_k_valid = true;
}

/* One pass: dst[x+y*8] = (SUM_z src[y+z*8] * k[x+z*8] + 0FFFh) >> 13.
 *
 * The sum is built row of src by row of src (z outermost), as an outer product
 * of src's row z (over y) with k's row z (over x). Integer addition does not
 * care about the order, so this is the same sum as the documented loop; it lets
 * the inner loop run over 8 contiguous int32 lanes, and a row of zero
 * coefficients, the usual case after RLE, is skipped outright. */
static void mdec_idct_pass(const int32_t* restrict src, int32_t* restrict dst,
                           const int32_t* restrict k) {
    int32_t acc[64];   /* acc[x*8 + y] */
    memset(acc, 0, sizeof acc);
    for (int z = 0; z < 8; z++) {
        const int32_t* s  = src + z * 8;
        const int32_t* kz = k + z * 8;
        if ((s[0] | s[1] | s[2] | s[3] | s[4] | s[5] | s[6] | s[7]) == 0) continue;
        for (int x = 0; x < 8; x++) {
            const int32_t kx = kz[x];
            int32_t* a = acc + x * 8;
            for (int y = 0; y < 8; y++) a[y] += s[y] * kx;
        }
    }
    for (int x = 0; x < 8; x++)
        for (int y = 0; y < 8; y++)
            dst[x + y * 8] = (acc[x * 8 + y] + 0xFFF) >> 13;
}

/* The block leaves the IDCT unclipped. Both consumers clip it themselves, each
 * as the documentation says: y_to_mono wraps Y to 9 bits (mdec.md:250), and
 * yuv_to_rgb clips Y+R, Y+G, Y+B, not Y, Cr or Cb on their own (:235). This used
 * to clip every block here, chroma included, which dimmed saturated colours at
 * the edges of bright areas. */
static void mdec_idct(const Mdec* m, int16_t* blk) {
    int32_t a[64], b[64];
    mdec_refresh_scale_k(m);
    for (int i = 0; i < 64; i++) a[i] = blk[i];
    mdec_idct_pass(a, b, g_scale_k);
    mdec_idct_pass(b, a, g_scale_k);
    for (int i = 0; i < 64; i++) blk[i] = (int16_t)a[i];   /* |a[i]| <= 16384, see above */
}

/* -------------------------------------------------------------------------
 * YUV -> RGB: yuv_to_rgb, psx-spx mdec.md:228-244.
 *
 *   R=(Y+R) AND 1FFh, G=(Y+G) AND 1FFh, B=(Y+B) AND 1FFh ;clip to signed 9bit
 *   R=MinMax(-128,127,R) ...                               ;saturate to 8 bit
 *
 * Only the saturation is applied by default. Taken literally, the 9-bit wrap
 * turns any Y+R above 255 into a small or negative number: Y=120 with Cr=100
 * gives R=0 instead of 255, a bright red pixel that comes out black. No hardware
 * capture available to this project shows that happening, and the same text
 * calls its own arithmetic approximate (:244), so the safe reading is the one
 * that cannot invert a highlight. ZS1_MDEC_WRAP9=1 applies the documented wrap
 * as written, for the A/B against a capture that would settle it.
 *
 * The documentation notes the exact fixed-point resolution is unknown, so the
 * published float coefficients are used as written.
 * ---------------------------------------------------------------------- */
static int g_mdec_wrap9 = -1;

static bool mdec_wrap9(void) {
    if (g_mdec_wrap9 < 0) {
        const char* v = getenv("ZS1_MDEC_WRAP9");
        g_mdec_wrap9 = (v && v[0] == '1') ? 1 : 0;
        if (g_mdec_wrap9)
            LOG_MDEC_INFO("[MDEC] ZS1_MDEC_WRAP9=1: yuv_to_rgb wraps Y+C to signed 9 bits "
                          "before saturating (mdec.md:235)");
    }
    return g_mdec_wrap9 != 0;
}

/* The chroma terms depend only on the Cr/Cb sample, and each sample covers a
 * 2x2 group of pixels (mdec.md:232), so they are computed once per sample with
 * the same float expressions instead of once per pixel. */
static void mdec_chroma_terms(const int16_t* Crblk, const int16_t* Cbblk,
                              int16_t* Rc, int16_t* Gc, int16_t* Bc) {
    for (int i = 0; i < 64; i++) {
        int16_t R = Crblk[i];
        int16_t B = Cbblk[i];
        Gc[i] = (int16_t)(-0.3437f*(float)B + -0.7143f*(float)R);
        Rc[i] = (int16_t)(1.402f*(float)R);
        Bc[i] = (int16_t)(1.772f*(float)B);
    }
}

static inline int32_t mdec_rgb_channel(int32_t sum, bool wrap9) {
    if (wrap9) sum = sign_extend9(sum & 0x1FF);
    return clamp_s32(sum, -128, 127);
}

/* Writes one 8x8 quadrant into block_rgb[0..255] (16x16 layout).
 *
 * Each channel is masked to its 8 bits before packing. With signed output
 * (mdec.md:39, :239: the xor 808080h is skipped) a negative channel is a
 * negative int, and packing it unmasked spread its sign bits over the channels
 * above it: a grey of Y=-60 came out with G and B both at 31 in 15bpp. */
static void mdec_yuv_to_rgb(Mdec* m, uint32_t xx, uint32_t yy,
                            const int16_t* Rc, const int16_t* Gc, const int16_t* Bc,
                            const int16_t* Yblk, bool wrap9) {
    const int32_t addval = m->output_signed ? 0 : 0x80;
    for (uint32_t y = 0; y < 8; y++) {
        for (uint32_t x = 0; x < 8; x++) {
            const uint32_t c = ((x + xx) >> 1) + ((y + yy) >> 1) * 8u;
            const int32_t  Y = Yblk[x + y * 8];
            const int32_t  R = mdec_rgb_channel(Y + Rc[c], wrap9) + addval;
            const int32_t  G = mdec_rgb_channel(Y + Gc[c], wrap9) + addval;
            const int32_t  B = mdec_rgb_channel(Y + Bc[c], wrap9) + addval;
            m->block_rgb[(x + xx) + (y + yy) * 16] =
                ((uint32_t)R & 0xFFu) |
                (((uint32_t)G & 0xFFu) << 8) |
                (((uint32_t)B & 0xFFu) << 16);
        }
    }
}

/* Mono: y_to_mono, psx-spx mdec.md:246-254: clip to signed 9 bits ("Y AND
 * 1FFh"), saturate to signed 8, then bias by 128 unless output is signed. The
 * result is masked to 8 bits for the same reason as the colour path. */
static void mdec_yuv_to_mono(Mdec* m) {
    const int32_t addval = m->output_signed ? 0 : 0x80;
    for (int i = 0; i < 64; i++) {
        int32_t v = clamp_s32(sign_extend9((int32_t)m->blocks[0][i] & 0x1FF), -128, 127);
        m->block_rgb[i] = (uint32_t)(v + addval) & 0xFFu;
    }
}

/* -------------------------------------------------------------------------
 * Copy decoded block_rgb to the output FIFO in the packed formats of
 * DOCS/macroblockdecodermdec.md (15bpp/24bpp/8bpp/4bpp output depths)
 * ---------------------------------------------------------------------- */
static void mdec_copy_out_block(Mdec* m) {
    switch (m->output_depth) {
        case 0: { /* 4-bit mono: 64 pixels → 8 words */
            const uint32_t* p = m->block_rgb;
            for (int i = 0; i < 64/8; i++) {
                uint32_t v = (p[0]>>4) | ((p[1]>>4)<<4) | ((p[2]>>4)<<8) | ((p[3]>>4)<<12)
                           | ((p[4]>>4)<<16) | ((p[5]>>4)<<20) | ((p[6]>>4)<<24) | ((p[7]>>4)<<28);
                p += 8;
                out_push(m, v);
            }
            break;
        }
        case 1: { /* 8-bit mono: 64 pixels → 16 words */
            const uint32_t* p = m->block_rgb;
            for (int i = 0; i < 64/4; i++) {
                uint32_t v = p[0] | (p[1]<<8) | (p[2]<<16) | (p[3]<<24);
                p += 4;
                out_push(m, v);
            }
            break;
        }
        case 2: { /* 24-bit color: 256 pixels → 192 words, packed tightly */
            uint32_t idx = 0, state = 0, rgb = 0;
            while (idx < 256) {
                switch (state) {
                    case 0: rgb = m->block_rgb[idx++]; state = 1; break;
                    case 1:
                        rgb |= (m->block_rgb[idx] & 0xFFu) << 24;
                        out_push(m, rgb);
                        rgb = m->block_rgb[idx] >> 8;
                        idx++; state = 2; break;
                    case 2:
                        rgb |= m->block_rgb[idx] << 16;
                        out_push(m, rgb);
                        rgb = m->block_rgb[idx] >> 16;
                        idx++; state = 3; break;
                    case 3:
                        rgb |= m->block_rgb[idx] << 8;
                        out_push(m, rgb);
                        idx++; state = 0; break;
                }
            }
            break;
        }
        case 3: { /* 15-bit color: 256 pixels → 128 words (2 pixels/word) */
            uint16_t a = (uint16_t)m->output_bit15 << 15;
            for (int i = 0; i < 256; i += 2) {
                uint32_t c0 = m->block_rgb[i];
                uint16_t r0 = (c0 >>  3) & 0x1Fu;
                uint16_t g0 = (c0 >> 11) & 0x1Fu;
                uint16_t b0 = (c0 >> 19) & 0x1Fu;
                uint16_t col0 = r0 | (uint16_t)(g0<<5) | (uint16_t)(b0<<10) | a;

                uint32_t c1 = m->block_rgb[i+1];
                uint16_t r1 = (c1 >>  3) & 0x1Fu;
                uint16_t g1 = (c1 >> 11) & 0x1Fu;
                uint16_t b1 = (c1 >> 19) & 0x1Fu;
                uint16_t col1 = r1 | (uint16_t)(g1<<5) | (uint16_t)(b1<<10) | a;

                out_push(m, (uint32_t)col0 | ((uint32_t)col1 << 16));
            }
            break;
        }
        default: break;
    }
    LOG_MDEC_DEBUG("[MDEC] Block copy out done, out_count=%u", m->out_count);
    g_mdec_macroblocks_out++;
    /* State: idle if no more data, else loop for next macroblock */
    m->decode_state = (m->remaining_halfwords == 0) ? MDEC_ST_IDLE : MDEC_ST_DECODING;
}

/* -------------------------------------------------------------------------
 * Decode one macroblock (mono or color)
 * Returns true if macroblock fully decoded and pushed to output FIFO.
 * ---------------------------------------------------------------------- */
static bool mdec_decode_macroblock(Mdec* m) {
    if (m->output_depth <= 1) {
        /* Mono path: one 8×8 block */
        if (!out_empty(m)) return false;
        if (!mdec_decode_rle(m, m->blocks[0], m->iq_y)) return false;
        mdec_idct(m, m->blocks[0]);
        m->current_block = 0;
        m->current_coefficient = 64;
        m->current_q_scale = 0;
        LOG_MDEC_DEBUG("[MDEC] Decoded mono, %u hw remain", m->remaining_halfwords);
        mdec_yuv_to_mono(m);
        mdec_copy_out_block(m);
        return true;
    } else {
        /* Color path: 6 blocks (Cr, Cb, Y1..Y4) */
        for (; m->current_block < (uint32_t)MDEC_NUM_BLOCKS; m->current_block++) {
            const uint8_t* qt = (m->current_block >= 2) ? m->iq_y : m->iq_uv;
            if (!mdec_decode_rle(m, m->blocks[m->current_block], qt)) return false;
            mdec_idct(m, m->blocks[m->current_block]);
        }
        /* All 6 blocks decoded. Wait for previous output to drain. */
        if (!out_empty(m)) return false;
        m->current_block = 0;
        m->current_coefficient = 64;
        m->current_q_scale = 0;
        LOG_MDEC_DEBUG("[MDEC] Decoded color macroblock, %u hw remain", m->remaining_halfwords);
        int16_t Rc[64], Gc[64], Bc[64];
        mdec_chroma_terms(m->blocks[0], m->blocks[1], Rc, Gc, Bc);
        const bool wrap9 = mdec_wrap9();
        mdec_yuv_to_rgb(m, 0, 0, Rc, Gc, Bc, m->blocks[2], wrap9);
        mdec_yuv_to_rgb(m, 8, 0, Rc, Gc, Bc, m->blocks[3], wrap9);
        mdec_yuv_to_rgb(m, 0, 8, Rc, Gc, Bc, m->blocks[4], wrap9);
        mdec_yuv_to_rgb(m, 8, 8, Rc, Gc, Bc, m->blocks[5], wrap9);
        lua_debug_notify("mdec_macroblock");
        mdec_copy_out_block(m);
        return true;
    }
}

/* -------------------------------------------------------------------------
 * Command handlers
 * ---------------------------------------------------------------------- */
static void mdec_handle_set_qtable(Mdec* m) {
    /* 32 halfwords = 64 bytes = luma quantization table */
    for (int i = 0; i < 64; i += 2) {
        uint16_t hw = in_pop(m);
        m->iq_y[i]   = (uint8_t)(hw & 0xFF);
        m->iq_y[i+1] = (uint8_t)(hw >> 8);
    }
    m->remaining_halfwords -= 32;
    if (m->remaining_halfwords >= 32) {
        /* Optionally followed by chroma table */
        for (int i = 0; i < 64; i += 2) {
            uint16_t hw = in_pop(m);
            m->iq_uv[i]   = (uint8_t)(hw & 0xFF);
            m->iq_uv[i+1] = (uint8_t)(hw >> 8);
        }
        m->remaining_halfwords -= 32;
    }
    LOG_MDEC_DEBUG("[MDEC] SetQuantTable done");
}

static void mdec_handle_set_scale(Mdec* m) {
    /* 64 halfwords = 64 x int16_t, stored EXACTLY AS DELIVERED — do not transpose.
     *
     * The orientation here and the indexing in mdec_idct() are one decision, not
     * two. DOCS/macroblockdecodermdec.md:192-196 describes real_idct_core as
     * "dst = src * scaletable ... but with src being diagonally mirrored, ie.
     * the matrices are processed column by column, instead of row by column",
     * and the pseudocode at :198-207 reads the table as scaletable[x+z*8]. That
     * indexing already accounts for the mirroring, so the table must arrive
     * untouched.
     *
     * This used to transpose on load, which was correct for a different inner
     * loop that read scale_table[y*8+u] — the transpose of the above. Keeping
     * both put the basis one transpose out: every 8x8 block decoded with its
     * edge row and column too dark, so FMV frames came out under a regular
     * 8-pixel grid. If that grid ever reappears, these two are disagreeing
     * again. */
    uint16_t packed[64];
    for (int i = 0; i < 64; i++)
        packed[i] = in_pop(m);
    for (int i = 0; i < 64; i++)
        m->scale_table[i] = (int16_t)packed[i];
    m->remaining_halfwords -= 64;
    LOG_MDEC_DEBUG("[MDEC] SetScale done");
}

/* -------------------------------------------------------------------------
 * Main execute loop. Incremental by necessity: it resumes wherever the input
 * FIFO ran dry and yields when the output FIFO fills, so decoding advances as
 * DMA delivers halfwords rather than over a complete source buffer.
 * ---------------------------------------------------------------------- */
void mdec_execute(Mdec* m) {
    for (;;) {
        switch (m->decode_state) {

            case MDEC_ST_IDLE: {
                if (m->in_count < 2) goto finished;
                uint32_t lo = in_pop(m);
                uint32_t hi = in_pop(m);
                uint32_t cw = lo | (hi << 16);
                uint8_t cmd = (uint8_t)((cw >> 29) & 7u);
                m->output_depth   = (uint8_t)((cw >> 27) & 3u);
                m->output_signed  =           (cw >> 26) & 1u;
                m->output_bit15   = (uint8_t)((cw >> 25) & 1u);
                /* Clear output FIFO on new command */
                m->out_head = m->out_tail = m->out_count = 0;
                uint32_t num_words;
                MdecDecodeState new_state;
                switch (cmd) {
                    case 1: /* DecodeMacroblock */
                        num_words = cw & 0xFFFFu;
                        new_state = MDEC_ST_DECODING;
                        break;
                    case 2: /* SetIqTable */
                        num_words = 16u + ((cw & 1u) ? 16u : 0u);
                        new_state = MDEC_ST_SET_QTABLE;
                        break;
                    case 3: /* SetScale */
                        num_words = 32u;
                        new_state = MDEC_ST_SET_SCALE;
                        break;
                    default:
                        /* MDEC(0) "has no function": bits 25-28 go to the status
                         * as usual (done above) and bits 0-15 are reflected to
                         * status bits 0-15 "without the minus 1 effect, and
                         * without actually expecting any parameters"; MDEC(4..7)
                         * "act identical as MDEC(0)" (psx-spx mdec.md:119-126).
                         *
                         * This used to wait for cw&FFFFh parameter words and
                         * swallow them. FE00h padding (mdec.md:77-79) arriving
                         * as a command word reads as FE00FE00h, an MDEC(7) for
                         * 65024 words, and it ate the next frame whole. */
                        g_mdec_idle_low16 = (uint16_t)(cw & 0xFFFFu);
                        LOG_MDEC_DEBUG("[MDEC] Command %u (0x%08x): no function, no parameters",
                                       cmd, cw);
                        continue;
                }
                /* What bits 15-0 read once this command has finished: "minus 1
                 * (FFFFh=None)" (psx-spx mdec.md:43). */
                g_mdec_idle_low16 = 0xFFFFu;
                m->remaining_halfwords = num_words * 2u;
                m->decode_state = new_state;
                LOG_MDEC_DEBUG("[MDEC] Command: cmd=%u depth=%u signed=%d nwords=%u",
                               cmd, m->output_depth, m->output_signed, num_words);
                continue;
            }

            case MDEC_ST_DECODING: {
                bool decoded = mdec_decode_macroblock(m);
                if (!decoded) {
                    if (m->remaining_halfwords == 0 &&
                        m->current_block < (uint32_t)MDEC_NUM_BLOCKS) {
                        /* Stream ended before all blocks decoded — abort */
                        m->current_block = 0;
                        m->current_coefficient = 64;
                        m->current_q_scale = 0;
                        m->decode_state = MDEC_ST_IDLE;
                        continue;
                    }
                    goto finished;
                }
                /* copy_out_block() already set decode_state. Let DMA drain output. */
                goto finished;
            }

            case MDEC_ST_WRITING:
                goto finished;

            case MDEC_ST_SET_QTABLE: {
                if (m->in_count < m->remaining_halfwords) goto finished;
                mdec_handle_set_qtable(m);
                m->decode_state = MDEC_ST_IDLE;
                continue;
            }

            case MDEC_ST_SET_SCALE: {
                if (m->in_count < m->remaining_halfwords) goto finished;
                mdec_handle_set_scale(m);
                m->decode_state = MDEC_ST_IDLE;
                continue;
            }

            case MDEC_ST_NOCOMMAND: {
                /* Nothing enters this state any more (MDEC(0)/(4..7) take no
                 * parameters, see above). A state saved by an older build can
                 * still be in it: drop the count it was waiting for, so the next
                 * word is taken as the command it is. */
                m->remaining_halfwords = 0;
                m->decode_state = MDEC_ST_IDLE;
                continue;
            }
        }
    }
finished:;
}

/* =========================================================================
 * Public API
 * ====================================================================== */

/* Power-on state: everything zero, tables included. "There is no usable scale
 * matrix until MDEC(3) has been issued: software that never sends one decodes
 * to flat mid-grey" (psx-spx mdec.md:114-116). */
void mdec_init(Mdec* m) {
    memset(m, 0, sizeof(Mdec));
    m->current_coefficient = 64;  /* 64 = start-of-block sentinel */
    g_mdec_idle_low16 = 0;
    LOG_MDEC_INFO("[MDEC] Initialized");
}

/* 1F801824h bit 31: "Abort any command, and set status=80040000h" (psx-spx
 * mdec.md:58), but "the Reset bit does NOT clear the scale matrix nor the quant
 * tables, so they only need uploading once, not after every reset" (:116-117).
 *
 * This used to be mdec_init(), which zeroed the tables too: a game that resets
 * the MDEC between two movies without re-sending them decoded the second one as
 * flat grey. */
static void mdec_soft_reset(Mdec* m) {
    uint8_t iq_y[64], iq_uv[64];
    int16_t scale[64];
    memcpy(iq_y,  m->iq_y,        sizeof iq_y);
    memcpy(iq_uv, m->iq_uv,       sizeof iq_uv);
    memcpy(scale, m->scale_table, sizeof scale);
    memset(m, 0, sizeof(Mdec));
    m->current_coefficient = 64;
    memcpy(m->iq_y,        iq_y,  sizeof iq_y);
    memcpy(m->iq_uv,       iq_uv, sizeof iq_uv);
    memcpy(m->scale_table, scale, sizeof scale);
    g_mdec_idle_low16 = 0;        /* status 80040000h: bits 15-0 read 0000h */
}

void mdec_state_restored(Mdec* m) {
    (void)m;
    /* The idle value of status bits 15-0 is not in the saved span (see
     * g_mdec_idle_low16). FFFFh is what it reads after any completed
     * MDEC(1/2/3), which is where a running game almost always is; a state
     * taken right after a reset or an MDEC(0) reads FFFFh here instead of 0000h
     * or the MDEC(0) count until the next command. */
    g_mdec_idle_low16 = 0xFFFFu;
}

uint32_t mdec_read(Mdec* m, uint32_t addr) {
    if (addr == 0x1F801824) {
        uint32_t s = mdec_get_status(m);
        LOG_MDEC_DEBUG("[MDEC] Status read -> 0x%08x", s);
        return s;
    }
    if (addr == 0x1F801820) {
        return mdec_dma_out(m);
    }
    return 0;
}

void mdec_write(Mdec* m, uint32_t addr, uint32_t value) {
    if (addr == 0x1F801824) {
        LOG_MDEC_DEBUG("[MDEC] Control <- 0x%08x", value);
        if (value & (1u << 31)) {
            mdec_soft_reset(m);
            LOG_MDEC_INFO("[MDEC] Software reset");
            return;
        }
        m->enable_dma_in  = (value >> 30) & 1u;
        m->enable_dma_out = (value >> 29) & 1u;
        mdec_execute(m);
        return;
    }
    if (addr == 0x1F801820) {
        in_push(m, (uint16_t)value);
        in_push(m, (uint16_t)(value >> 16));
        mdec_execute(m);
        return;
    }
}

void mdec_dma_in(Mdec* m, uint32_t word) {
    if (m->in_count + 2 > MDEC_IN_FIFO_HW) {
        LOG_MDEC_WARN("[MDEC] Input FIFO overflow");
        return;
    }
    in_push(m, (uint16_t)word);
    in_push(m, (uint16_t)(word >> 16));
    mdec_execute(m);
}

uint32_t mdec_dma_out(Mdec* m) {
    if (out_empty(m)) {
        LOG_MDEC_WARN("[MDEC] Output FIFO empty on read");
        return 0xFFFFFFFF;
    }
    uint32_t v = out_pop(m);
    if (out_empty(m))
        mdec_execute(m);
    return v;
}

bool mdec_input_has_space(const Mdec* m) {
    return in_space(m) >= 2;
}

bool mdec_output_has_data(const Mdec* m) {
    return !out_empty(m);
}
