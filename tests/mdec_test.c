/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 ZioZoni95
 *
 * Part of ZoniStation One, a PlayStation 1 emulator.
 * See LICENSE for the full licence text and THIRD-PARTY.md for the
 * components of this project that have other authors.
 */
/*
 * mdec_test.c - unit test for src/core/mdec.c against the psx-spx MDEC page,
 * ps1/cpu/mdec/macroblockdecodermdec.md (cited below as mdec.md:<line>).
 *
 * The real decoder is compiled into this program (#include of the .c file) and
 * driven only through its register interface: command and parameter words go in
 * at 1F801820h, results come out of the output FIFO, status is read at
 * 1F801824h. Nothing here needs SDL, GL, a BIOS or a disc.
 *
 * What it checks:
 *   [zigzag]   zagzig[] against the rule "zagzig[zigzag[i]]=i" (mdec.md:300-303)
 *   [V1]       the reset bit keeps the quant and scale tables (mdec.md:116-117)
 *   [V2]       MDEC(0)/(4..7) take no parameters, status bits 15-0 (mdec.md:43,
 *              :58, :119-126), including FE00FE00h padding read as a command
 *   [V4]       yuv_to_rgb: no pre-clip of Cr/Cb, saturation after the sum by
 *              default, the literal 9-bit wrap only with ZS1_MDEC_WRAP9=1
 *   [R9]       signed output packing (mdec.md:39, :239)
 *   [R14]      the scale table's upper 13 bits (mdec.md:317-318)
 *   [F14]      the int32 IDCT and the per-sample chroma terms decode random
 *              streams bit-exactly like the previous int64, per-pixel
 *              structure, which is kept below as ref_decode()
 *   [status]   bit 30 "last word received", bits 18-16 with output pending
 *
 * ref_decode() keeps the pre-change decoder's structure (dense int64 IDCT,
 * chroma recomputed for every pixel) with switchable semantics. With
 * MDEC_TEST_OLD_SEMANTICS defined and MDEC_SOURCE pointing at the pre-change
 * mdec.c, only the random comparison runs, against the old semantics: that is
 * how the reference itself was checked to reproduce the old decoder bit for
 * bit before it was used to judge the new one. With MDEC_TEST_PRE_FIX the full
 * set of checks builds against the pre-change decoder, where they fail.
 */
#define _POSIX_C_SOURCE 200809L
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "log.h"

void log_print(LogCategory category, LogLevel level, const char* format, ...) {
    (void)category; (void)level; (void)format;
}
void lua_debug_notify(const char* event_name) { (void)event_name; }

#ifndef MDEC_SOURCE
#define MDEC_SOURCE "../src/core/mdec.c"
#endif
#include MDEC_SOURCE

#ifdef MDEC_TEST_PRE_FIX
/* Shims for the internals the checks below look at, so that the same checks
 * can be built against the pre-change decoder and seen to fail there:
 *   gcc ... -DMDEC_TEST_PRE_FIX -DMDEC_SOURCE='"<old mdec.c>"' tests/mdec_test.c */
static int     g_mdec_wrap9;
static int32_t g_scale_k[64];
static void mdec_refresh_scale_k(const Mdec* m) {
    for (int i = 0; i < 64; i++) g_scale_k[i] = m->scale_table[i] / 8;
}
void mdec_state_restored(Mdec* m) { (void)m; }
#endif

/* ------------------------------------------------------------------------ */

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

/* mdec.md:279-289 */
static const uint8_t doc_zigzag[64] = {
     0, 1, 5, 6,14,15,27,28,  2, 4, 7,13,16,26,29,42,
     3, 8,12,17,25,30,41,43,  9,11,18,24,31,40,44,53,
    10,19,23,32,39,45,52,54, 20,22,33,38,46,51,55,60,
    21,34,37,47,50,56,59,61, 35,36,48,49,57,58,62,63 };

/* mdec.md:308-315, the standard scale table */
static const uint16_t doc_scale[64] = {
    0x5A82,0x5A82,0x5A82,0x5A82,0x5A82,0x5A82,0x5A82,0x5A82,
    0x7D8A,0x6A6D,0x471C,0x18F8,0xE707,0xB8E3,0x9592,0x8275,
    0x7641,0x30FB,0xCF04,0x89BE,0x89BE,0xCF04,0x30FB,0x7641,
    0x6A6D,0xE707,0x8275,0xB8E3,0x471C,0x7D8A,0x18F8,0x9592,
    0x5A82,0xA57D,0xA57D,0x5A82,0x5A82,0xA57D,0xA57D,0x5A82,
    0x471C,0x8275,0x18F8,0x6A6D,0x9592,0xE707,0x7D8A,0xB8E3,
    0x30FB,0x89BE,0x7641,0xCF04,0xCF04,0x7641,0x89BE,0x30FB,
    0x18F8,0xB8E3,0x6A6D,0x8275,0x7D8A,0x9592,0x471C,0xE707 };

#define REG_DATA 0x1F801820u
#define REG_CTRL 0x1F801824u

static void w(Mdec* m, uint32_t v) { mdec_write(m, REG_DATA, v); }
static uint32_t status(Mdec* m)    { return mdec_read(m, REG_CTRL); }

static void upload_scale(Mdec* m, const int16_t* t) {
    w(m, 0x60000000u);
    for (int i = 0; i < 64; i += 2)
        w(m, (uint32_t)(uint16_t)t[i] | ((uint32_t)(uint16_t)t[i + 1] << 16));
}
static void upload_quant(Mdec* m, const uint8_t* y, const uint8_t* uv) {
    w(m, 0x40000001u);
    for (int i = 0; i < 64; i += 4)
        w(m, y[i] | (y[i+1] << 8) | (y[i+2] << 16) | ((uint32_t)y[i+3] << 24));
    for (int i = 0; i < 64; i += 4)
        w(m, uv[i] | (uv[i+1] << 8) | (uv[i+2] << 16) | ((uint32_t)uv[i+3] << 24));
}
static void std_tables(Mdec* m, uint8_t q) {
    int16_t s[64]; uint8_t t[64];
    for (int i = 0; i < 64; i++) { s[i] = (int16_t)doc_scale[i]; t[i] = q; }
    upload_scale(m, s);
    upload_quant(m, t, t);
}

/* Send a parameter stream (halfwords) as words, draining the output FIFO after
 * every word as DMA1 would, so the decoder never stalls on a full FIFO. */
static size_t feed(Mdec* m, const uint16_t* hw, size_t n_hw, uint32_t* out, size_t out_cap) {
    size_t n_out = 0;
    for (size_t i = 0; i < n_hw; i += 2) {
        uint32_t word = hw[i] | ((i + 1 < n_hw ? (uint32_t)hw[i + 1] : 0xFE00u) << 16);
        mdec_dma_in(m, word);
        while (mdec_output_has_data(m)) {
            uint32_t v = mdec_dma_out(m);
            if (n_out < out_cap) out[n_out] = v;
            n_out++;
        }
    }
    return n_out;
}

/* A macroblock made of DC-only blocks: each block is "q=1, dc" then the FE00h
 * end code. With the standard table a DC-only block decodes to a constant. */
static size_t dc_stream(uint16_t* hw, const int* dc_levels, int nblocks) {
    size_t n = 0;
    for (int b = 0; b < nblocks; b++) {
        hw[n++] = (uint16_t)((1u << 10) | ((uint32_t)dc_levels[b] & 0x3FFu));
        hw[n++] = 0xFE00u;
    }
    return n;
}

static uint32_t cmd_decode(int depth, int sgn, int b15, uint32_t words) {
    return (1u << 29) | ((uint32_t)depth << 27) | ((uint32_t)sgn << 26) |
           ((uint32_t)b15 << 25) | (words & 0xFFFFu);
}

/* ======================================================================== *
 * Reference decoder: the pre-change structure, switchable semantics.
 * ======================================================================== */

typedef struct {
    int scale_floor;  /* 1: scale>>3 (upper 13 bits, R14); 0: C "/8" (old)        */
    int preclip;      /* 1: clip every IDCT output to 9 bits and saturate (old)    */
    int wrap9;        /* 1: yuv_to_rgb wraps Y+C to 9 bits before saturating       */
    int mask;         /* 1: mask each channel to 8 bits before packing (R9)        */
} RefSem;

static int32_t r_sext10(int32_t v) { v &= 0x3FF; return v >= 0x200 ? v - 0x400 : v; }
static int32_t r_sext9(int32_t v)  { v &= 0x1FF; return v >= 0x100 ? v - 0x200 : v; }
static int32_t r_clamp(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

/* rl_decode_block as the decoder implements it (mdec.md:146-166): FE00h before
 * a block is skipped, and the block ends once k reaches 63. Returns false if the
 * stream runs out first. */
static int ref_rle(const uint16_t* hw, size_t n, size_t* pos, int16_t blk[64], const uint8_t qt[64]) {
    memset(blk, 0, 64 * sizeof(int16_t));
    uint16_t v;
    do { if (*pos >= n) return 0; v = hw[(*pos)++]; } while (v == 0xFE00u);
    int32_t q = (v >> 10) & 0x3F;
    int32_t val = r_sext10(v) * qt[0];
    if (q == 0) val = r_sext10(v) * 2;
    val = r_clamp(val, -0x400, 0x3FF);
    if (q > 0) blk[zagzig[0]] = (int16_t)val; else blk[0] = (int16_t)val;
    int32_t k = 0;
    for (;;) {
        if (*pos >= n) return 0;
        v = hw[(*pos)++];
        k += ((v >> 10) & 0x3F) + 1;
        if (k < 64) {
            val = (r_sext10(v) * qt[k] * q + 4) / 8;
            if (q == 0) val = r_sext10(v) * 2;
            val = r_clamp(val, -0x400, 0x3FF);
            if (q > 0) blk[zagzig[k]] = (int16_t)val; else blk[k] = (int16_t)val;
        }
        if (k >= 63) return 1;
    }
}

/* real_idct_core with a dense int64 sum, as before (mdec.md:206-219). */
static void ref_idct(int16_t blk[64], const int16_t scale[64], const RefSem* sem) {
    int32_t src[64], dst[64];
    for (int i = 0; i < 64; i++) src[i] = blk[i];
    for (int pass = 0; pass < 2; pass++) {
        for (int x = 0; x < 8; x++)
            for (int y = 0; y < 8; y++) {
                int64_t sum = 0;
                for (int z = 0; z < 8; z++) {
                    int32_t s = scale[x + z * 8];
                    int32_t k = sem->scale_floor ? (s >> 3) : (s / 8);
                    sum += (int64_t)src[y + z * 8] * k;
                }
                dst[x + y * 8] = (int32_t)((sum + 0xFFF) >> 13);
            }
        for (int i = 0; i < 64; i++) src[i] = dst[i];
    }
    for (int i = 0; i < 64; i++)
        blk[i] = sem->preclip ? (int16_t)r_clamp(r_sext9(src[i]), -128, 127) : (int16_t)src[i];
}

/* yuv_to_rgb with the chroma terms recomputed for every pixel, as before. */
static void ref_yuv(uint32_t rgb[256], int xx, int yy, const int16_t* Cr, const int16_t* Cb,
                    const int16_t* Yb, int sgn, const RefSem* sem) {
    int16_t addval = sgn ? 0 : 0x80;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            int16_t R = Cr[((x + xx) / 2) + ((y + yy) / 2) * 8];
            int16_t B = Cb[((x + xx) / 2) + ((y + yy) / 2) * 8];
            int16_t G = (int16_t)(-0.3437f * (float)B + -0.7143f * (float)R);
            R = (int16_t)(1.402f * (float)R);
            B = (int16_t)(1.772f * (float)B);
            int16_t Y = Yb[x + y * 8];
            int32_t r = (int32_t)Y + R, g = (int32_t)Y + G, b = (int32_t)Y + B;
            if (sem->wrap9) { r = r_sext9(r); g = r_sext9(g); b = r_sext9(b); }
            int16_t ro = (int16_t)(r_clamp(r, -128, 127) + addval);
            int16_t go = (int16_t)(r_clamp(g, -128, 127) + addval);
            int16_t bo = (int16_t)(r_clamp(b, -128, 127) + addval);
            uint32_t px = sem->mask
                ? ((uint32_t)ro & 0xFFu) | (((uint32_t)go & 0xFFu) << 8) | (((uint32_t)bo & 0xFFu) << 16)
                : (uint32_t)(uint16_t)ro | ((uint32_t)(uint16_t)go << 8) | ((uint32_t)(uint16_t)bo << 16);
            rgb[(x + xx) + (y + yy) * 16] = px;
        }
}

static void ref_mono(uint32_t rgb[64], const int16_t* Yb, int sgn, const RefSem* sem) {
    int32_t addval = sgn ? 0 : 0x80;
    for (int i = 0; i < 64; i++) {
        int32_t v = r_clamp(r_sext9(Yb[i]), -128, 127);
        rgb[i] = sem->mask ? ((uint32_t)(v + addval) & 0xFFu) : (uint32_t)(v + addval);
    }
}

/* The output packing, copied from the decoder (it is not under test here). */
static size_t ref_pack(const uint32_t* p, int depth, int b15, uint32_t* out) {
    size_t n = 0;
    switch (depth) {
    case 0:
        for (int i = 0; i < 8; i++, p += 8)
            out[n++] = (p[0]>>4) | ((p[1]>>4)<<4) | ((p[2]>>4)<<8) | ((p[3]>>4)<<12)
                     | ((p[4]>>4)<<16) | ((p[5]>>4)<<20) | ((p[6]>>4)<<24) | ((p[7]>>4)<<28);
        break;
    case 1:
        for (int i = 0; i < 16; i++, p += 4)
            out[n++] = p[0] | (p[1]<<8) | (p[2]<<16) | (p[3]<<24);
        break;
    case 2: {
        uint32_t idx = 0, st = 0, rgb = 0;
        while (idx < 256) {
            switch (st) {
            case 0: rgb = p[idx++]; st = 1; break;
            case 1: rgb |= (p[idx] & 0xFFu) << 24; out[n++] = rgb; rgb = p[idx] >> 8;  idx++; st = 2; break;
            case 2: rgb |= p[idx] << 16;           out[n++] = rgb; rgb = p[idx] >> 16; idx++; st = 3; break;
            case 3: rgb |= p[idx] << 8;            out[n++] = rgb; idx++; st = 0; break;
            }
        }
        break;
    }
    case 3: {
        uint16_t a = (uint16_t)(b15 << 15);
        for (int i = 0; i < 256; i += 2) {
            uint32_t c0 = p[i], c1 = p[i + 1];
            uint16_t w0 = (uint16_t)(((c0 >> 3) & 0x1F) | (((c0 >> 11) & 0x1F) << 5) | (((c0 >> 19) & 0x1F) << 10) | a);
            uint16_t w1 = (uint16_t)(((c1 >> 3) & 0x1F) | (((c1 >> 11) & 0x1F) << 5) | (((c1 >> 19) & 0x1F) << 10) | a);
            out[n++] = (uint32_t)w0 | ((uint32_t)w1 << 16);
        }
        break;
    }
    }
    return n;
}

/* Decode a whole MDEC(1) parameter stream. */
static size_t ref_decode(const uint16_t* hw, size_t n, int depth, int sgn, int b15,
                         const uint8_t* iq_y, const uint8_t* iq_uv, const int16_t* scale,
                         const RefSem* sem, uint32_t* out) {
    size_t pos = 0, n_out = 0;
    int16_t blk[6][64];
    uint32_t rgb[256];
    for (;;) {
        if (depth <= 1) {
            if (!ref_rle(hw, n, &pos, blk[0], iq_y)) break;
            ref_idct(blk[0], scale, sem);
            ref_mono(rgb, blk[0], sgn, sem);
        } else {
            int ok = 1;
            for (int b = 0; b < 6 && ok; b++) {
                ok = ref_rle(hw, n, &pos, blk[b], b >= 2 ? iq_y : iq_uv);
                if (ok) ref_idct(blk[b], scale, sem);
            }
            if (!ok) break;
            ref_yuv(rgb, 0, 0, blk[0], blk[1], blk[2], sgn, sem);
            ref_yuv(rgb, 8, 0, blk[0], blk[1], blk[3], sgn, sem);
            ref_yuv(rgb, 0, 8, blk[0], blk[1], blk[4], sgn, sem);
            ref_yuv(rgb, 8, 8, blk[0], blk[1], blk[5], sgn, sem);
        }
        n_out += ref_pack(rgb, depth, b15, out + n_out);
    }
    return n_out;
}

/* ------------------------------------------------------------------------ */

static uint32_t g_rng = 0x12345678u;
static uint32_t rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
static int rnd_range(int lo, int hi) { return lo + (int)(rnd() % (uint32_t)(hi - lo + 1)); }

/* One random block: a DC/q_scale halfword, then (run, level) entries until the
 * end code or until k reaches 63. Levels are drawn wide on purpose so the IDCT
 * rings past the 8-bit range and the clipping paths are exercised. */
static size_t gen_block(uint16_t* hw) {
    size_t n = 0;
    int q = (rnd() % 8 == 0) ? 0 : rnd_range(1, 63);
    if (rnd() % 16 == 0) hw[n++] = 0xFE00u;           /* leading padding */
    hw[n++] = (uint16_t)((q << 10) | (rnd_range(-512, 511) & 0x3FF));
    int k = 0;
    int entries = rnd_range(0, 20);
    for (int e = 0; e < entries; e++) {
        int run = (rnd() % 4 == 0) ? rnd_range(0, 20) : rnd_range(0, 3);
        int lvl = (rnd() % 3 == 0) ? rnd_range(-512, 511) : rnd_range(-40, 40);
        hw[n++] = (uint16_t)((run << 10) | (lvl & 0x3FF));
        k += run + 1;
        if (k >= 63) return n;
    }
    hw[n++] = 0xFE00u;                                /* end code */
    return n;
}

#define MAX_HW  (64 * 1024)
#define MAX_OUT (256 * 1024)

static uint16_t s_hw[MAX_HW];
static uint32_t s_out_dec[MAX_OUT], s_out_ref[MAX_OUT];

/* Random streams through the decoder and the reference; returns mismatches. */
static int compare_random(const RefSem* sem, int iterations) {
    static Mdec m;
    int bad = 0;
    for (int it = 0; it < iterations; it++) {
        int16_t scale[64]; uint8_t qy[64], quv[64];
        int std = (it % 3) != 0;
        for (int i = 0; i < 64; i++) {
            scale[i] = std ? (int16_t)doc_scale[i]
                           : (i == 0 && it % 2 ? (int16_t)-32768 : (int16_t)rnd());
            qy[i]  = (uint8_t)(std ? (uint32_t)rnd_range(1, 64) : rnd());
            quv[i] = (uint8_t)(std ? (uint32_t)rnd_range(1, 64) : rnd());
        }
        int depth = rnd_range(0, 3), sgn = rnd_range(0, 1), b15 = rnd_range(0, 1);
        int nmb   = rnd_range(1, 12);
        int blocks_per_mb = depth <= 1 ? 1 : 6;
        size_t n = 0;
        for (int b = 0; b < nmb * blocks_per_mb; b++) n += gen_block(s_hw + n);
        if (n & 1) s_hw[n++] = 0xFE00u;

        mdec_init(&m);
        upload_scale(&m, scale);
        upload_quant(&m, qy, quv);
        w(&m, cmd_decode(depth, sgn, b15, (uint32_t)(n / 2)));
        size_t nd = feed(&m, s_hw, n, s_out_dec, MAX_OUT);
        size_t nr = ref_decode(s_hw, n, depth, sgn, b15, qy, quv, scale, sem, s_out_ref);

        if (nd != nr) {
            if (bad < 5) printf("  iter %d: %zu words decoded, reference %zu (depth %d)\n", it, nd, nr, depth);
            bad++;
            continue;
        }
        for (size_t i = 0; i < nd; i++)
            if (s_out_dec[i] != s_out_ref[i]) {
                if (bad < 5)
                    printf("  iter %d word %zu: decoder %08x reference %08x (depth %d signed %d)\n",
                           it, i, s_out_dec[i], s_out_ref[i], depth, sgn);
                bad++;
                break;
            }
    }
    return bad;
}

#ifndef MDEC_TEST_OLD_SEMANTICS

/* ======================================================================== */

static void test_zigzag(void) {
    uint8_t zz[64];
    for (int i = 0; i < 64; i++) zz[doc_zigzag[i]] = (uint8_t)i;
    int bad = 0;
    for (int i = 0; i < 64; i++) if (zz[i] != zagzig[i]) bad++;
    CHECK(bad == 0, "[zigzag] %d entries differ from zagzig[zigzag[i]]=i (mdec.md:300-303)", bad);
}

static void test_v1_reset_keeps_tables(void) {
    static Mdec m;
    mdec_init(&m);
    CHECK(m.scale_table[0] == 0 && m.iq_y[0] == 0, "[V1] power-on tables are not zero");
    std_tables(&m, 2);
    mdec_write(&m, REG_CTRL, 0x80000000u);
    CHECK((uint16_t)m.scale_table[0] == 0x5A82 && (uint16_t)m.scale_table[63] == 0xE707,
          "[V1] reset cleared the scale table (scale[0]=%04x, mdec.md:116-117)",
          (uint16_t)m.scale_table[0]);
    CHECK(m.iq_y[0] == 2 && m.iq_uv[0] == 2, "[V1] reset cleared the quant tables (iq_y[0]=%u iq_uv[0]=%u)",
          m.iq_y[0], m.iq_uv[0]);
    CHECK(status(&m) == 0x80040000u, "[V1] status after reset %08x, want 80040000h (mdec.md:58)", status(&m));

    /* And a frame decoded after the reset, without re-sending the tables, is
     * not flat grey: Y=-60 everywhere comes out as 68 (= -60 + 128). */
    uint16_t hw[32];
    int dc[6] = { 0, 0, -240, -240, -240, -240 };
    size_t n = dc_stream(hw, dc, 6);
    m.enable_dma_out = true;
    w(&m, cmd_decode(2, 0, 0, (uint32_t)(n / 2)));
    uint32_t out[256];
    size_t got = feed(&m, hw, n, out, 256);
    CHECK(got == 192, "[V1] decode after reset produced %zu words, want 192", got);
    CHECK(got > 0 && (out[0] & 0xFF) == 68, "[V1] decode after reset: first R=%u, want 68", out[0] & 0xFF);
}

static void test_v2_no_parameter_commands(void) {
    static Mdec m;
    mdec_init(&m);
    std_tables(&m, 2);
    uint32_t s = status(&m);
    CHECK((s & 0xFFFF) == 0xFFFF, "[V2] bits 15-0 after MDEC(3)+MDEC(2) = %04x, want FFFFh (mdec.md:43)", s & 0xFFFF);
    CHECK(!(s & (1u << 29)), "[V2] busy after the tables were loaded");

    /* MDEC(0) with a count of 5: no parameters, count reflected without -1. */
    w(&m, 0x00000005u);
    s = status(&m);
    CHECK(m.decode_state == MDEC_ST_IDLE, "[V2] MDEC(0) left the decoder in state %d", (int)m.decode_state);
    CHECK((s & 0xFFFF) == 0x0005, "[V2] bits 15-0 after MDEC(0) count 5 = %04x, want 0005h (mdec.md:120-123)", s & 0xFFFF);
    CHECK(!(s & (1u << 29)), "[V2] MDEC(0) reads busy");
    w(&m, 0x60000000u);   /* the next command must be taken as one */
    CHECK(m.decode_state == MDEC_ST_SET_SCALE, "[V2] the MDEC(3) after MDEC(0) was swallowed (state %d)",
          (int)m.decode_state);
    for (int i = 0; i < 32; i++) w(&m, 0x5A825A82u);
    CHECK(m.decode_state == MDEC_ST_IDLE, "[V2] MDEC(3) did not complete");

    /* MDEC(4..7) act as MDEC(0) (mdec.md:125-126); bits 25-28 still go to
     * status 23-26 (:120-121). */
    for (uint32_t c = 4; c <= 7; c++) {
        w(&m, (c << 29) | (3u << 27) | 0x1234u);
        s = status(&m);
        CHECK(m.decode_state == MDEC_ST_IDLE && (s & 0xFFFF) == 0x1234,
              "[V2] MDEC(%u): state %d, bits 15-0 %04x", c, (int)m.decode_state, s & 0xFFFF);
        CHECK(((s >> 25) & 3) == 3, "[V2] MDEC(%u) did not reflect bits 27-28 to status 25-26", c);
    }

    /* Harness case [3b]: an exact MDEC(1), two FE00FE00h padding words after it
     * (mdec.md:77-79), then the next frame. The padding must not eat it. */
    mdec_init(&m);
    std_tables(&m, 2);
    uint16_t hw[32];
    int dc[6] = { 16, 16, 16, 16, 16, 16 };
    size_t n = dc_stream(hw, dc, 6);
    uint32_t out[512];
    w(&m, cmd_decode(3, 0, 0, (uint32_t)(n / 2)));
    size_t f1 = feed(&m, hw, n, out, 512);
    CHECK(f1 == 128, "[V2] frame 1 gave %zu words, want 128", f1);
    CHECK((status(&m) & 0xFFFF) == 0xFFFF, "[V2] bits 15-0 after MDEC(1) = %04x, want FFFFh", status(&m) & 0xFFFF);
    w(&m, 0xFE00FE00u);
    w(&m, 0xFE00FE00u);
    s = status(&m);
    CHECK(m.decode_state == MDEC_ST_IDLE && m.remaining_halfwords == 0,
          "[V2] FE00FE00h padding left state %d with %u halfwords pending",
          (int)m.decode_state, m.remaining_halfwords);
    CHECK((s & 0xFFFF) == 0xFE00, "[V2] padding word as MDEC(7): bits 15-0 = %04x, want FE00h", s & 0xFFFF);
    w(&m, cmd_decode(3, 0, 0, (uint32_t)(n / 2)));
    size_t f2 = feed(&m, hw, n, out, 512);
    CHECK(f2 == 128, "[V2] frame 2 after the padding gave %zu words, want 128 (harness [3b] gave 0)", f2);

    /* While MDEC(1) runs: parameter words remaining minus 1. */
    mdec_init(&m);
    std_tables(&m, 2);
    w(&m, cmd_decode(3, 0, 0, 10));
    s = status(&m);
    CHECK((s & 0xFFFF) == 9 && (s & (1u << 29)), "[V2] MDEC(1) of 10 words, none sent: %08x, want busy and 0009h", s);
    w(&m, 0xFE00FE00u);
    w(&m, 0xFE00FE00u);
    CHECK((status(&m) & 0xFFFF) == 7, "[V2] after 2 of 10 words: bits 15-0 = %04x, want 0007h", status(&m) & 0xFFFF);
    mdec_write(&m, REG_CTRL, 0x80000000u);
    CHECK(status(&m) == 0x80040000u, "[V2] reset during MDEC(1): status %08x, want 80040000h", status(&m));

    /* After a savestate load the idle value is not known; FFFFh is assumed. */
    mdec_state_restored(&m);
    CHECK((status(&m) & 0xFFFF) == 0xFFFF, "[V2] after mdec_state_restored: %04x", status(&m) & 0xFFFF);
}

/* Y=120 (DC level 480, qt 2) and Cr=100 (DC level 400): R = 120 + 140 = 260. */
static void decode_y120_cr100(uint32_t out[192]) {
    static Mdec m;
    mdec_init(&m);
    std_tables(&m, 2);
    uint16_t hw[32];
    int dc[6] = { 400, 0, 480, 480, 480, 480 };   /* Cr, Cb, Y1..Y4 */
    size_t n = dc_stream(hw, dc, 6);
    w(&m, cmd_decode(2, 0, 0, (uint32_t)(n / 2)));
    size_t got = feed(&m, hw, n, out, 192);
    CHECK(got == 192, "[V4] decode gave %zu words", got);
    CHECK(m.blocks[2][0] == 120 && m.blocks[0][0] == 100,
          "[V4] test setup: Y=%d Cr=%d, want 120 and 100", m.blocks[2][0], m.blocks[0][0]);
}

static void test_v4_yuv(void) {
    uint32_t out[192];

    /* Default: saturate after the sum, no wrap. */
    unsetenv("ZS1_MDEC_WRAP9");
    g_mdec_wrap9 = -1;
    decode_y120_cr100(out);
    CHECK((out[0] & 0xFF) == 255, "[V4] default: R for Y=120 Cr=100 is %u, want 255 (saturated)", out[0] & 0xFF);
    CHECK(((out[0] >> 8) & 0xFF) == (uint32_t)(r_clamp(120 + (int16_t)(-0.7143f * 100.0f), -128, 127) + 128),
          "[V4] default: G is %u", (out[0] >> 8) & 0xFF);

    /* ZS1_MDEC_WRAP9=1: the documented (Y+R) AND 1FFh first (mdec.md:235). */
    setenv("ZS1_MDEC_WRAP9", "1", 1);
    g_mdec_wrap9 = -1;
    decode_y120_cr100(out);
    CHECK((out[0] & 0xFF) == 0, "[V4] WRAP9: R for Y=120 Cr=100 is %u, want 0 (260 wraps to -252)", out[0] & 0xFF);
    unsetenv("ZS1_MDEC_WRAP9");
    g_mdec_wrap9 = -1;

    /* Chroma is not clipped on its own any more: a Cr block above 127 (DC level
     * 511 times qt 3 saturates the coefficient to 3FFh, which decodes to 128)
     * gives an R term of int(1.402*128) = 179, where the old pre-clip to 127
     * gave 178. Y = -60 (level -160 times qt 3) keeps the sum in range. */
    static Mdec m;
    mdec_init(&m);
    std_tables(&m, 3);
    uint16_t hw[32];
    int dc[6] = { 511, 0, -160, -160, -160, -160 };
    size_t n = dc_stream(hw, dc, 6);
    w(&m, cmd_decode(2, 0, 0, (uint32_t)(n / 2)));
    size_t got = feed(&m, hw, n, out, 192);
    int y = m.blocks[2][0], cr = m.blocks[0][0];
    CHECK(got == 192 && cr > 127, "[V4] test setup: Cr=%d should exceed 127", cr);
    int want = r_clamp(y + (int16_t)(1.402f * (float)cr), -128, 127) + 128;
    CHECK((int)(out[0] & 0xFF) == want, "[V4] R with Cr=%d Y=%d is %u, want %d (no chroma pre-clip)",
          cr, y, out[0] & 0xFF, want);

    /* Mono follows y_to_mono literally (mdec.md:250-252): "Y AND 1FFh" as a
     * signed 9-bit value, then saturate. 160 stays 160 and saturates to 127;
     * 300 wraps to -212 and saturates to -128, so an overshoot that bright
     * reads black, exactly as written; 256 wraps to -256. */
    mdec_init(&m);
    m.output_signed = false;
    m.blocks[0][0] = 160;  m.blocks[0][1] = 300; m.blocks[0][2] = -300; m.blocks[0][3] = 256;
    mdec_yuv_to_mono(&m);
    CHECK(m.block_rgb[0] == 255, "[V4] mono 160 -> %u, want 255", m.block_rgb[0]);
    CHECK(m.block_rgb[1] == 0,   "[V4] mono 300 -> %u, want 0 (wraps to -212)", m.block_rgb[1]);
    CHECK(m.block_rgb[2] == 255, "[V4] mono -300 -> %u, want 255 (wraps to 212)", m.block_rgb[2]);
    CHECK(m.block_rgb[3] == 0,   "[V4] mono 256 -> %u, want 0 (wraps to -256)", m.block_rgb[3]);
}

static void test_r9_signed_packing(void) {
    static Mdec m;
    uint16_t hw[32];
    int dc[6] = { 0, 0, -240, -240, -240, -240 };   /* Y = -60 */
    size_t n = dc_stream(hw, dc, 6);
    uint32_t out[256];

    /* 15bpp signed: each channel is the 5 MSBs of the signed byte C4h = 24. */
    mdec_init(&m);
    std_tables(&m, 2);
    w(&m, cmd_decode(3, 1, 0, (uint32_t)(n / 2)));
    size_t got = feed(&m, hw, n, out, 256);
    CHECK(got == 128, "[R9] 15bpp signed gave %zu words", got);
    CHECK(out[0] == 0x63186318u, "[R9] 15bpp signed grey Y=-60: %08x, want 63186318h (harness [4] gave 7ff87ff8h)", out[0]);

    /* 24bpp signed: three bytes of C4h per pixel. */
    mdec_init(&m);
    std_tables(&m, 2);
    w(&m, cmd_decode(2, 1, 0, (uint32_t)(n / 2)));
    got = feed(&m, hw, n, out, 256);
    CHECK(got == 192 && out[0] == 0xC4C4C4C4u, "[R9] 24bpp signed: %08x, want C4C4C4C4h", out[0]);

    /* 8bpp mono signed. */
    int dcm[1] = { -240 };
    n = dc_stream(hw, dcm, 1);
    mdec_init(&m);
    std_tables(&m, 2);
    w(&m, cmd_decode(1, 1, 0, (uint32_t)(n / 2)));
    got = feed(&m, hw, n, out, 256);
    CHECK(got == 16 && out[0] == 0xC4C4C4C4u, "[R9] 8bpp mono signed: %08x, want C4C4C4C4h", out[0]);
}

static void test_r14_scale_upper_13_bits(void) {
    static Mdec m;
    mdec_init(&m);
    std_tables(&m, 2);
    mdec_refresh_scale_k(&m);
    int bad = 0, differs_from_c_div = 0;
    for (int i = 0; i < 64; i++) {
        int32_t s = (int16_t)doc_scale[i];
        if (g_scale_k[i] != (s >> 3)) bad++;
        if (s / 8 != (s >> 3)) differs_from_c_div++;
    }
    CHECK(bad == 0, "[R14] %d scale entries are not the upper 13 bits (mdec.md:317-318)", bad);
    CHECK(differs_from_c_div == 28, "[R14] expected 28 standard entries where C '/8' differs, got %d",
          differs_from_c_div);
    /* The extremes of the 16-bit range. */
    m.scale_table[0] = -32768; m.scale_table[1] = 32767; m.scale_table[2] = -1; m.scale_table[3] = -8;
    mdec_refresh_scale_k(&m);
    CHECK(g_scale_k[0] == -4096 && g_scale_k[1] == 4095 && g_scale_k[2] == -1 && g_scale_k[3] == -1,
          "[R14] extremes: %d %d %d %d", g_scale_k[0], g_scale_k[1], g_scale_k[2], g_scale_k[3]);
}

static void test_status_bits(void) {
    static Mdec m;
    mdec_init(&m);
    std_tables(&m, 2);
    m.enable_dma_out = true;

    /* Bit 30: set once the running command has every parameter word (mdec.md:34). */
    uint16_t hw[32];
    int dc[6] = { 16, 16, 16, 16, 16, 16 };
    size_t n = dc_stream(hw, dc, 6);   /* 12 halfwords = 6 words */
    w(&m, cmd_decode(3, 0, 0, (uint32_t)(n / 2) + 2));   /* 2 words more than sent below */
    for (size_t i = 0; i < n; i += 2) w(&m, hw[i] | ((uint32_t)hw[i + 1] << 16));
    uint32_t s = status(&m);
    CHECK(!(s & (1u << 30)), "[status] bit 30 set with 2 parameter words still to come: %08x", s);

    /* Bits 18-16 with output pending: Y1..Y4 as the macroblock is read out. */
    CHECK(!(s & (1u << 31)), "[status] the macroblock should be in the output FIFO");
    CHECK(((s >> 16) & 7) == 0, "[status] output pending, nothing read: block %u, want 0 (Y1)", (s >> 16) & 7);
    for (int i = 0; i < 64; i++) mdec_dma_out(&m);
    CHECK(((status(&m) >> 16) & 7) == 2, "[status] half the macroblock read: block %u, want 2 (Y3)",
          (status(&m) >> 16) & 7);
    while (mdec_output_has_data(&m)) mdec_dma_out(&m);
    CHECK(((status(&m) >> 16) & 7) == 4, "[status] output empty, next block Cr: %u, want 4",
          (status(&m) >> 16) & 7);
    w(&m, 0xFE00FE00u);
    w(&m, 0xFE00FE00u);
    s = status(&m);
    /* The padding was consumed by the next block's skip loop and the command
     * ran out of parameters: it is idle again. */
    CHECK(!(s & (1u << 29)), "[status] still busy after the last word: %08x", s);

    /* Bit 30 while busy with everything received: a mono command whose block is
     * complete but whose output has not been read yet keeps it busy. */
    mdec_init(&m);
    std_tables(&m, 2);
    int dcm[2] = { 16, 16 };
    n = dc_stream(hw, dcm, 2);
    w(&m, cmd_decode(1, 0, 0, (uint32_t)(n / 2)));
    for (size_t i = 0; i < n; i += 2) w(&m, hw[i] | ((uint32_t)hw[i + 1] << 16));
    s = status(&m);
    CHECK((s & (1u << 29)) && (s & (1u << 30)),
          "[status] mono, all words in, output not read: %08x, want busy and bit 30", s);
    CHECK(((s >> 16) & 7) == 4, "[status] mono output pending: block %u, want 4 (Y)", (s >> 16) & 7);
}

#endif /* !MDEC_TEST_OLD_SEMANTICS */

int main(void) {
#ifdef MDEC_TEST_OLD_SEMANTICS
    const RefSem old_sem = { 0, 1, 0, 0 };
    int bad = compare_random(&old_sem, 3000);
    CHECK(bad == 0, "[ref] reference with the old semantics differs from %s in %d of 3000 streams",
          MDEC_SOURCE, bad);
#else
    test_zigzag();
    test_v1_reset_keeps_tables();
    test_v2_no_parameter_commands();
    test_v4_yuv();
    test_r9_signed_packing();
    test_r14_scale_upper_13_bits();
    test_status_bits();

    /* [F14] bit-exactness of the restructured IDCT and colour conversion against
     * the previous structure, with the new semantics, in both yuv_to_rgb modes. */
    const RefSem new_sem = { 1, 0, 0, 1 };
    unsetenv("ZS1_MDEC_WRAP9");
    g_mdec_wrap9 = -1;
    int bad = compare_random(&new_sem, 3000);
    CHECK(bad == 0, "[F14] %d of 3000 random streams differ from the reference (default mode)", bad);

    const RefSem new_sem_wrap = { 1, 0, 1, 1 };
    setenv("ZS1_MDEC_WRAP9", "1", 1);
    g_mdec_wrap9 = -1;
    bad = compare_random(&new_sem_wrap, 1500);
    CHECK(bad == 0, "[F14] %d of 1500 random streams differ from the reference (ZS1_MDEC_WRAP9=1)", bad);
    unsetenv("ZS1_MDEC_WRAP9");
    g_mdec_wrap9 = -1;
#endif
    printf("mdec_test: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
