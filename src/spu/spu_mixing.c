/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 ZioZoni95
 * SPDX-FileCopyrightText: 2002 Pete Bernert and the PCSX-Redux authors
 *
 * Part of ZoniStation One, a PlayStation 1 emulator.
 * See LICENSE for the full licence text and THIRD-PARTY.md for the
 * components of this project that have other authors.
 */
#include "spu.h"
#include "interconnect.h"
#include "log.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <SDL3/SDL.h>

/* Forward declarations from other modules */
extern void spu_reverb_init(Spu* spu);

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

static int32_t clamp16(int32_t v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return v;
}

/* =========================================================================
 * Noise Generator (Dr. Hell's algorithm)
 * ========================================================================= */

static const uint8_t noise_freq_add[5] = { 0, 84, 140, 180, 210 };

/* The bit shifted into the noise LFSR, straight from the documented generator
 * (`DOCS/soundprocessingunitspu.md:534`):
 *
 *     ParityBit = NoiseLevel.Bit15 xor Bit12 xor Bit11 xor Bit10 xor 1
 *
 * This used to be a 64-entry lookup indexed by `(noise_level >> 10) & 63` —
 * the same function, since index bit N is level bit 10+N, so the entry is
 * `idx5 ^ idx2 ^ idx1 ^ idx0 ^ 1`. The table's second half was a copy of its
 * first, which made the parity wrong for every state with bit 15 set: half the
 * sequence. Computing it removes the whole class of transcription error, and
 * the result matches PCSX-Redux's table entry for entry. */
static inline uint32_t noise_parity(uint32_t level) {
    return ((level >> 15) ^ (level >> 12) ^ (level >> 11) ^ (level >> 10) ^ 1u) & 1u;
}

static void noise_tick(Spu* spu) {
    uint32_t clock = (spu->control >> 8) & 0x3F;
    uint32_t level = (0x8000 >> (clock >> 2)) << 16;

    spu->noise_count += 0x10000 + noise_freq_add[clock & 3];
    if ((spu->noise_count & 0xFFFF) >= noise_freq_add[4]) {
        spu->noise_count += 0x10000 - noise_freq_add[clock & 3];
    }

    if (spu->noise_count >= level) {
        spu->noise_count %= level;
        spu->noise_level = (spu->noise_level << 1) | noise_parity(spu->noise_level);
    }
}

/* =========================================================================
 * Capture Buffer
 * ========================================================================= */

/* The four capture buffers are the first 4 KB of SPU RAM itself
 * (psx-spx spu/soundprocessingunitspu.md:51-62): CD left at 000h, CD right at
 * 400h, voice 1 at 800h, voice 3 at C00h, 200h signed 16-bit samples each
 * (:839), one per 44.1 kHz tick. They used to go to a private array no game
 * could read back, and nothing could trap them: with the IRQ address inside
 * 0000h-01FFh (byte address 000h-FFFh) every write is an access that raises
 * IRQ9 (:836-844), which is how lip-sync and capture-driven effects are timed
 * (:861-865). RAM_CTRL always reads 0004h here, so the "bit3-2 both zero"
 * exception at :844 never applies. */
static void capture_write(Spu* spu, struct Interconnect* inter, int channel, int16_t value) {
    uint32_t byte_addr = (uint32_t)channel * 0x400u + (spu->capture_pos & 0x1FFu) * 2u;
    spu->ram[byte_addr >> 1] = (uint16_t)value;
    spu_check_irq(spu, inter, byte_addr);
}

static void capture_increment(Spu* spu) {
    spu->capture_pos = (spu->capture_pos + 1u) & 0x1FFu;
    /* SPUSTAT.11: first or second half of the buffers (:650, :661-663). */
    spu->status = (spu->status & ~SPU_STATUS_CB_HALF) |
        ((spu->capture_pos >= 0x100) ? SPU_STATUS_CB_HALF : 0);
}

/* =========================================================================
 * Reverb
 *
 * Implemented from the formula in DOCS/soundprocessingunitspu.md ("SPU Reverb
 * Formula"): same-side and different-side reflection, a 4-tap comb early echo,
 * then two all-pass stages, all running at 22050 Hz.
 *
 * Addressing is the part that is easy to get wrong: every src/dst/disp register
 * holds an SPU address *divided by 8*, and src/dst are relative to the current
 * buffer address while disp registers are relative to those. Internally this
 * works in halfwords, hence the x4. Treating the registers as halfword offsets
 * (which this code used to do) puts every read and write at a quarter of its
 * intended distance — inside the buffer they alias each other, and outside it
 * they land on the voices' own sample data.
 *
 * The buffer runs from mBASE to the end of SPU RAM and advances one halfword
 * per step; everything wraps inside that window.
 * ========================================================================= */

/* Resolve an offset (in halfwords, may be negative) against the current buffer
 * address, wrapped into mBASE..end-of-RAM. */
static uint32_t rev_addr(const Spu* spu, int32_t off_hw) {
    const int32_t end = (int32_t)(SPU_RAM_SIZE / 2);
    int32_t base = (int32_t)spu->reverb_base * 4;
    if (base < 0 || base >= end) base = 0;
    const int32_t span = end - base;
    int32_t a = (int32_t)spu->reverb_current_addr + off_hw - base;
    /* About 30 of these per 22050 Hz step. The offset nearly always lands
     * inside the work area already, and there `%` is the identity, so the
     * division is only paid when it would change something; the result is
     * the same in every case. */
    if ((uint32_t)a >= (uint32_t)span) {
        a %= span;
        if (a < 0) a += span;
    }
    return (uint32_t)(base + a);
}

static int16_t rev_rd(const Spu* spu, int32_t off_hw) {
    return (int16_t)spu->ram[rev_addr(spu, off_hw)];
}

/* Values written to the reverb buffer are saturated to 16 bits. */
/* SPUCNT bit 7 disables *writes* to the reverb work area; reads keep happening
 * and the unit keeps producing output (DOCS/soundprocessingunitspu.md:826-835,
 * confirmed by the bus-timing table at :1113-1123). Gating the output instead —
 * which is what this used to do — meant the reverb went on scribbling over SPU
 * RAM while a game believed it had switched the unit off. */
static void rev_wr(Spu* spu, int32_t off_hw, int32_t value) {
    if (!(spu->control & SPU_CTRL_REVERB_ENABLE)) return;
    spu->ram[rev_addr(spu, off_hw)] = (uint16_t)(int16_t)clamp16(value);
}

/* Volume registers are signed 16-bit; products are divided by 0x8000. */
#define RV(name)  ((int32_t)(int16_t)spu->reverb_regs[REVERB_REG_##name / 2])
/* Address registers are SPU addresses / 8 — x4 to reach halfwords. */
#define RA(name)  ((int32_t)spu->reverb_regs[REVERB_REG_##name / 2] * 4)
#define RMUL(a, b) (((int32_t)(a) * (int32_t)(b)) >> 15)

static void reverb_process(Spu* spu, int32_t input_l, int32_t input_r,
                           int32_t* out_l, int32_t* out_r) {
    /* Register aliases, doc name on the left. The names in the register map are
     * this project's older ones; the mapping is fixed by the register order. */
    const int32_t vLIN  = RV(IN_COEF_L),  vRIN  = RV(IN_COEF_R);
    const int32_t vIIR  = RV(IIR_ALPHA),  vWALL = RV(IIR_COEF);
    const int32_t vAPF1 = RV(FB_ALPHA),   vAPF2 = RV(FB_X);
    const int32_t vC1   = RV(ACC_COEF_A), vC2   = RV(ACC_COEF_B);
    const int32_t vC3   = RV(ACC_COEF_C), vC4   = RV(ACC_COEF_D);

    const int32_t dAPF1 = RA(FB_SRC_A),   dAPF2 = RA(FB_SRC_B);
    const int32_t mLSAME = RA(IIR_DEST_A0), mRSAME = RA(IIR_DEST_A1);
    const int32_t mLDIFF = RA(IIR_DEST_B0), mRDIFF = RA(IIR_DEST_B1);
    const int32_t dLSAME = RA(IIR_SRC_A0),  dRSAME = RA(IIR_SRC_A1);
    const int32_t dLDIFF = RA(IIR_SRC_B1),  dRDIFF = RA(IIR_SRC_B0);
    const int32_t mLCOMB1 = RA(ACC_SRC_A0), mRCOMB1 = RA(ACC_SRC_A1);
    const int32_t mLCOMB2 = RA(ACC_SRC_B0), mRCOMB2 = RA(ACC_SRC_B1);
    const int32_t mLCOMB3 = RA(ACC_SRC_C0), mRCOMB3 = RA(ACC_SRC_C1);
    const int32_t mLCOMB4 = RA(ACC_SRC_D0), mRCOMB4 = RA(ACC_SRC_D1);
    const int32_t mLAPF1 = RA(MIX_DEST_A0), mRAPF1 = RA(MIX_DEST_A1);
    const int32_t mLAPF2 = RA(MIX_DEST_B0), mRAPF2 = RA(MIX_DEST_B1);

    /* Input from the mixer */
    const int32_t Lin = RMUL(vLIN, clamp16(input_l));
    const int32_t Rin = RMUL(vRIN, clamp16(input_r));

    /* Same side reflection (L-to-L, R-to-R) */
    {
        int32_t prev = rev_rd(spu, mLSAME - 1);
        rev_wr(spu, mLSAME, RMUL(Lin + RMUL(rev_rd(spu, dLSAME), vWALL) - prev, vIIR) + prev);
        prev = rev_rd(spu, mRSAME - 1);
        rev_wr(spu, mRSAME, RMUL(Rin + RMUL(rev_rd(spu, dRSAME), vWALL) - prev, vIIR) + prev);
    }

    /* Different side reflection (R-to-L, L-to-R) */
    {
        int32_t prev = rev_rd(spu, mLDIFF - 1);
        rev_wr(spu, mLDIFF, RMUL(Lin + RMUL(rev_rd(spu, dRDIFF), vWALL) - prev, vIIR) + prev);
        prev = rev_rd(spu, mRDIFF - 1);
        rev_wr(spu, mRDIFF, RMUL(Rin + RMUL(rev_rd(spu, dLDIFF), vWALL) - prev, vIIR) + prev);
    }

    /* Early echo: 4-tap comb filter reading from the buffer */
    int32_t Lout = RMUL(vC1, rev_rd(spu, mLCOMB1)) + RMUL(vC2, rev_rd(spu, mLCOMB2))
                 + RMUL(vC3, rev_rd(spu, mLCOMB3)) + RMUL(vC4, rev_rd(spu, mLCOMB4));
    int32_t Rout = RMUL(vC1, rev_rd(spu, mRCOMB1)) + RMUL(vC2, rev_rd(spu, mRCOMB2))
                 + RMUL(vC3, rev_rd(spu, mRCOMB3)) + RMUL(vC4, rev_rd(spu, mRCOMB4));

    /* All-pass 1, fed by the comb output */
    {
        int32_t tl = rev_rd(spu, mLAPF1 - dAPF1);
        int32_t tr = rev_rd(spu, mRAPF1 - dAPF1);
        Lout -= RMUL(vAPF1, tl);
        Rout -= RMUL(vAPF1, tr);
        rev_wr(spu, mLAPF1, Lout);
        rev_wr(spu, mRAPF1, Rout);
        Lout = RMUL(Lout, vAPF1) + tl;
        Rout = RMUL(Rout, vAPF1) + tr;
    }

    /* All-pass 2, fed by all-pass 1 */
    {
        int32_t tl = rev_rd(spu, mLAPF2 - dAPF2);
        int32_t tr = rev_rd(spu, mRAPF2 - dAPF2);
        Lout -= RMUL(vAPF2, tl);
        Rout -= RMUL(vAPF2, tr);
        rev_wr(spu, mLAPF2, Lout);
        rev_wr(spu, mRAPF2, Rout);
        Lout = RMUL(Lout, vAPF2) + tl;
        Rout = RMUL(Rout, vAPF2) + tr;
    }

    /* Raw 22050 Hz reverb output — the output resampler and vLOUT/vROUT are
     * applied by the caller (rev_reverb_resample). */
    *out_l = clamp16(Lout);
    *out_r = clamp16(Rout);

    /* One halfword per 22050 Hz step, wrapped inside the work area. */
    {
        const int32_t end = (int32_t)(SPU_RAM_SIZE / 2);
        int32_t base = (int32_t)spu->reverb_base * 4;
        if (base < 0 || base >= end) base = 0;
        int32_t next = (int32_t)spu->reverb_current_addr + 1;
        if (next >= end || next < base) next = base;
        spu->reverb_current_addr = (uint32_t)next;
    }
}

#undef RV
#undef RA
#undef RMUL

/* =========================================================================
 * Reverb input/output resampling (39-tap half-band FIR).
 *
 * The SPU reverb unit runs at 22050 Hz. Rather than the crude "average two
 * input samples, hold the output across two" this used to do, hardware feeds
 * the unit through a 39-tap FIR: the mixer's 44100 Hz signal is downsampled to
 * 22050 Hz for the reverb network, and the network's 22050 Hz output is
 * upsampled back to 44100 Hz. Skipping it left the reverb dull and quiet — the
 * boot chime came out nearly dry. Coefficients and behaviour are the hardware's,
 * from DOCS/soundprocessingunitspu.md ("Reverb Buffer Resampling"); the code is
 * this project's own.
 *
 * The rings are stored doubled (…_buf[.. .. ] repeated at +half) so a 39/20-wide
 * window can be read contiguously without wrapping mid-loop.
 * ========================================================================= */

static const int32_t s_rev_fir[39] = {
    -0x0001, 0x0000,  0x0002, 0x0000, -0x000A, 0x0000,  0x0023, 0x0000,
    -0x0067, 0x0000,  0x010A, 0x0000, -0x0268, 0x0000,  0x0534, 0x0000,
    -0x0B90, 0x0000,  0x2806, 0x4000,  0x2806, 0x0000, -0x0B90, 0x0000,
     0x0534, 0x0000, -0x0268, 0x0000,  0x010A, 0x0000, -0x0067, 0x0000,
     0x0023, 0x0000, -0x000A, 0x0000,  0x0002, 0x0000, -0x0001
};

/* Downsample 44100 -> 22050: the 39-tap FIR over the input ring, window
 * ending at the current (odd) position. >>15 for unity gain.
 *
 * Eighteen of the 39 taps are zero: every odd one except the centre (k = 19).
 * The sum skips them, which leaves it exactly what the full loop computed, with
 * 21 multiplies instead of 39. */
static int32_t rev_fir_down(const int16_t* ring /*[128], doubled at +64*/, int pos) {
    int base = (pos - 38) & 0x3F;
    int64_t acc = (int64_t)s_rev_fir[19] * ring[base + 19];
    for (int k = 0; k < 39; k += 2)
        acc += (int64_t)s_rev_fir[k] * ring[base + k];
    return clamp16((int32_t)(acc >> 15));
}

/* Upsample 22050 -> 44100. Odd host samples interpolate with the 20 non-zero
 * even taps (>>14, the polyphase gain); even host samples pass the aligned
 * reverb sample straight through (the FIR's centre tap). */
static int32_t rev_fir_up(const int16_t* ring /*[64], doubled at +32*/, int pos) {
    int base = ((pos >> 1) - 19) & 0x1F;
    if (pos & 1) {
        int64_t acc = 0;
        for (int k = 0; k < 20; k++)
            acc += (int64_t)s_rev_fir[k * 2] * ring[base + k];
        return clamp16((int32_t)(acc >> 14));
    }
    return ring[base + 9];
}

/* One 44100 Hz reverb sample: push input into the downsample ring; every second
 * host sample run the reverb network on the downsampled input and store its
 * output in the upsample ring; then reconstruct this host sample and apply the
 * reverb output volume. */
static void rev_reverb_resample(Spu* spu, int32_t in_l, int32_t in_r,
                                int32_t* out_l, int32_t* out_r) {
    int pos = (int)spu->reverb_resample_pos & 0x3F;
    int dp = pos;                 /* 0..63 index into the 128-wide doubled ring */
    int16_t il = (int16_t)clamp16(in_l), ir = (int16_t)clamp16(in_r);
    spu->reverb_ds_buf[0][dp] = spu->reverb_ds_buf[0][dp + 64] = il;
    spu->reverb_ds_buf[1][dp] = spu->reverb_ds_buf[1][dp + 64] = ir;

    if (pos & 1) {
        int32_t dl = rev_fir_down(spu->reverb_ds_buf[0], pos);
        int32_t dr = rev_fir_down(spu->reverb_ds_buf[1], pos);
        int32_t rl = 0, rr = 0;
        reverb_process(spu, dl, dr, &rl, &rr);   /* raw 22050 Hz output */
        int up = (pos >> 1) & 0x1F;
        spu->reverb_us_buf[0][up] = spu->reverb_us_buf[0][up + 32] = (int16_t)clamp16(rl);
        spu->reverb_us_buf[1][up] = spu->reverb_us_buf[1][up + 32] = (int16_t)clamp16(rr);
    }

    int32_t ul = rev_fir_up(spu->reverb_us_buf[0], pos);
    int32_t ur = rev_fir_up(spu->reverb_us_buf[1], pos);

    /* Reverb output volume (vLOUT/vROUT), signed 16-bit, product >>15. */
    *out_l = clamp16(((int32_t)ul * (int32_t)spu->reverb_vol_left)  >> 15);
    *out_r = clamp16(((int32_t)ur * (int32_t)spu->reverb_vol_right) >> 15);

    spu->reverb_resample_pos = (spu->reverb_resample_pos + 1) & 0x3F;
}

void spu_reverb_init(Spu* spu) {
    spu->reverb_current_addr = 0;
    memset(spu->reverb_regs, 0, sizeof(spu->reverb_regs));
    memset(spu->reverb_ds_buf, 0, sizeof(spu->reverb_ds_buf));
    memset(spu->reverb_us_buf, 0, sizeof(spu->reverb_us_buf));
    spu->reverb_resample_pos = 0;
}

/* =========================================================================
 * Single Sample Generation
 * ========================================================================= */

static void spu_generate_one_sample(Spu* spu, struct Interconnect* inter, int16_t* left_out, int16_t* right_out) {
    /* Process key on/off at start of batch */
    spu_process_key_on_off(spu);

    /* Mix all 24 voices — accumulate int32, apply L/R vol, clamp once.
     *
     * A voice feeds the reverb unit only when its bit is set in the per-voice
     * reverb mask (0x1D98/0x1D9A, "voice reverb mode"); the dry mix always gets
     * every voice. Feeding the whole mix into the reverb — which is what this
     * did before — drives the feedback network with far more energy than the
     * game asked for, and the network's own output is what comes back out. */
    int32_t mix_l = 0, mix_r = 0;
    int32_t rev_in_l = 0, rev_in_r = 0;

    for (int v = 0; v < NUM_VOICES; v++) {
        int32_t sval = spu_voice_get_sample(spu, inter, v);
        SpuVoice* voice = &spu->voices[v];
        spu_voice_sweep_tick(voice);
        int32_t vl = ((int32_t)sval * voice->vol_left)  >> 15;
        int32_t vr = ((int32_t)sval * voice->vol_right) >> 15;
        mix_l += vl;
        mix_r += vr;
        if (spu->reverb_on & (1u << v)) {
            rev_in_l += vl;
            rev_in_r += vr;
        }
    }

    /* Clamp accumulated sum */
    mix_l = clamp16(mix_l);
    mix_r = clamp16(mix_r);
    rev_in_l = clamp16(rev_in_l);
    rev_in_r = clamp16(rev_in_r);

    /* SPUCNT.14 = 0 mutes the SPU's own sound, "(Don't care for CD Audio)"
     * (psx-spx spu/soundprocessingunitspu.md:631): the voices, dry and into the
     * reverb, go silent here, before the CD input joins the mix. It used to be
     * applied as a zero main volume at the very end, which took the CD (and an
     * XA cutscene's speech with it) down as well. */
    if (spu->muted) {
        mix_l = mix_r = 0;
        rev_in_l = rev_in_r = 0;
    }

    /* CD audio mixing through SPU, separately routable to the reverb.
     *
     * ZS1_SPU_NO_CDAUDIO=1 drops the CD/XA contribution, the counterpart to
     * ZS1_SPU_NO_REVERB. In a scene where XA streams at the full sample rate and
     * the voices only fire effects, the two switches split the output into its
     * two sources, and an artefact that survives both is in neither. */
    static int s_no_cdaudio = -1;
    if (s_no_cdaudio < 0) s_no_cdaudio = getenv("ZS1_SPU_NO_CDAUDIO") ? 1 : 0;

    if (!s_no_cdaudio && (spu->control & SPU_CTRL_CD_AUDIO_EN)) {
        /* AVOLL/AVOLR, signed, "-8000h..+7FFFh" (:437-443). A volume of 0 is
         * silence: it used to be taken as "uninitialised" and played at full
         * volume, so a game that faded the CD down to 0 for a scene change kept
         * hearing the tail of the old line. No power-on value is documented;
         * the documented setup has the software write this register before
         * any CD audio (psx-spx cdr/cdromdrive.md:352-354, :1014-1016), and
         * spu_step logs once if CD frames arrive while it is still 0. */
        int32_t cd_l = ((int32_t)spu->cd_audio_left  * (int32_t)spu->cd_vol_left)  >> 15;
        int32_t cd_r = ((int32_t)spu->cd_audio_right * (int32_t)spu->cd_vol_right) >> 15;
        mix_l = clamp16(mix_l + cd_l);
        mix_r = clamp16(mix_r + cd_r);
        if (spu->control & SPU_CTRL_CD_REVERB) {
            rev_in_l = clamp16(rev_in_l + cd_l);
            rev_in_r = clamp16(rev_in_r + cd_r);
        }
    }

    /* Capture buffers, in SPU RAM: CD input before the volume (:53-54), voices
     * 1 and 3 after the ADSR (:55-56), whether or not they are audible (:841-843). */
    capture_write(spu, inter, 0, spu->cd_audio_left);
    capture_write(spu, inter, 1, spu->cd_audio_right);
    capture_write(spu, inter, 2, (int16_t)spu->voices[1].sval);
    capture_write(spu, inter, 3, (int16_t)spu->voices[3].sval);
    capture_increment(spu);

    /* Noise tick */
    noise_tick(spu);

    /* Reverb. ZS1_SPU_NO_REVERB=1 bypasses the stage entirely — an A/B switch
     * for deciding whether an audible artefact comes from here or from the
     * voice/mix path. */
    static int s_no_reverb = -1;
    if (s_no_reverb < 0) s_no_reverb = getenv("ZS1_SPU_NO_REVERB") ? 1 : 0;

    /* The reverb unit runs at 22050 Hz, half the output rate. The 44100 Hz
     * mixer signal is downsampled into the network and its output upsampled
     * back, both through the hardware's 39-tap FIR (rev_reverb_resample), which
     * also steps the reverb delay line once per two host samples. */
    int32_t rev_l = 0, rev_r = 0;
    if (!s_no_reverb) {
        rev_reverb_resample(spu, rev_in_l, rev_in_r, &rev_l, &rev_r);
        spu->reverb_in_l  = rev_in_l;   /* kept for the debug meter / emu.reverb() */
        spu->reverb_in_r  = rev_in_r;
        spu->reverb_out_l = rev_l;
        spu->reverb_out_r = rev_r;
    }

    if (!s_no_reverb) {
        mix_l = clamp16(mix_l + rev_l);
        mix_r = clamp16(mix_r + rev_r);
    }

    /* Main volume. MVOLL/MVOLR have the voice volume format (:405-406), so in
     * sweep mode (bit15=1) the level moves once per sample (:414-432) and
     * MVOLXL/MVOLXR read it back (:526-531). Only the fixed mode used to do
     * anything: a scene that set 0 and then faded in with a sweep stayed silent
     * throughout, XA included, and a sweep fade-out never lowered anything. The
     * level applies after voices, reverb and CD are summed. Mute is no longer
     * folded in here; see SPUCNT.14 above. */
    if (spu->main_vol_left & 0x8000)
        spu->main_vol_left_cur = spu_sweep_tick(spu->main_vol_left, (int)spu->main_vol_left_cur,
                                                &spu->main_vol_sweep_count[0]);
    if (spu->main_vol_right & 0x8000)
        spu->main_vol_right_cur = spu_sweep_tick(spu->main_vol_right, (int)spu->main_vol_right_cur,
                                                 &spu->main_vol_sweep_count[1]);
    int32_t mv_l = spu->main_vol_left_cur;
    int32_t mv_r = spu->main_vol_right_cur;

    int32_t final_l = clamp16((mix_l * mv_l) >> 15);
    int32_t final_r = clamp16((mix_r * mv_r) >> 15);

    /* One-shot diagnostics */
    static bool mix_logged = false;
    if (!mix_logged && (mix_l != 0 || mix_r != 0)) {
        mix_logged = true;
        LOG_SPU_INFO("[SPU] First non-zero voice mix: mix=(%d,%d) mvol=(%d,%d) muted=%d final=(%d,%d)",
                     mix_l, mix_r, mv_l, mv_r, spu->muted ? 1 : 0, final_l, final_r);
    }
    static bool silent_warn_logged = false;
    if (!silent_warn_logged && spu->total_samples_generated > 88200 && mix_l == 0 && mix_r == 0) {
        silent_warn_logged = true;
        LOG_SPU_WARN("[SPU] 2s of silence: mvol=(%d,%d) muted=%d total_keys=%u",
                     mv_l, mv_r, spu->muted ? 1 : 0, spu->total_key_on_events);
    }

    /* ZS1_AUDIO_DUMP=<path>: raw interleaved s16 of exactly what the device
     * receives, so the output can be looked at instead of guessed about. */
    {
        static FILE* s_dump = NULL;
        static int    s_dump_tried = 0;
        static unsigned s_dump_frames = 0;
        if (!s_dump_tried) {
            s_dump_tried = 1;
            const char* path = getenv("ZS1_AUDIO_DUMP");
            if (path) {
                s_dump = fopen(path, "wb");
                LOG_SPU_INFO("[SPU] Dumping raw output to %s (reverb %s, SPUCNT=0x%04x)",
                             path, s_no_reverb ? "bypassed" : "on", spu->control);
            }
        }
        if (s_dump) {
            int16_t f[2] = { (int16_t)final_l, (int16_t)final_r };
            fwrite(f, sizeof(int16_t), 2, s_dump);
            /* Flushed about ten times a second. Nothing closes this file, and a
             * run that ends on a signal — a timeout, a kill, the crash being
             * investigated — takes the whole stdio buffer with it, which is how
             * a 30-second capture came back zero bytes. */
            if ((++s_dump_frames & 4095u) == 0) fflush(s_dump);
        }
    }

    *left_out = (int16_t)final_l;
    *right_out = (int16_t)final_r;

    /* Update peak levels for audio meter */
    int32_t abs_l = final_l < 0 ? -final_l : final_l;
    int32_t abs_r = final_r < 0 ? -final_r : final_r;
    if (abs_l > spu->peak_level_left) spu->peak_level_left = abs_l;
    if (abs_r > spu->peak_level_right) spu->peak_level_right = abs_r;
}

/* =========================================================================
 * Sample generation, driven by the emulated clock
 *
 * One stereo sample every CPU_TICKS_PER_SPU_TICK (768) CPU cycles, i.e. the
 * 44100 Hz the hardware runs its DSP at. Everything that advances with a sample
 * — ADSR envelopes, ADPCM positions, the reverb delay line, the CD audio FIFO —
 * therefore advances in step with the guest's own sense of time, which is what
 * makes note lengths and envelope shapes come out right.
 *
 * A sample is always generated even when the output ring is full: the state has
 * to advance regardless, only the audible result is dropped.
 * ========================================================================= */

void spu_step(struct Interconnect* inter, uint32_t cpu_cycles) {
    Spu* spu = &inter->spu;
    spu->spu_tick_counter += cpu_cycles;

    while (spu->spu_tick_counter >= CPU_TICKS_PER_SPU_TICK) {
        spu->spu_tick_counter -= CPU_TICKS_PER_SPU_TICK;

        /* Feed one CDROM audio sample into SPU CD inputs.
         * The CDROM audio FIFO holds XA/CDDA at 44100 Hz stereo. */
        {
            /* Hold the last CD sample when the FIFO has nothing, rather than
             * falling to zero.
             *
             * The rates match exactly on average — at double speed with the
             * interleave of 8 this stream uses, XA delivers 44100 frames a second
             * and the SPU consumes 44100 — but neither side is smooth. A sector
             * lands 2352 frames at once every 53 ms while the SPU drains in bursts
             * of 64, so the queue bottoms out and momentarily has nothing, about
             * 230 times a second on the measured scene. total_dropped never saw
             * this: nothing is discarded, a frame is simply missing.
             *
             * Zeroing turned each of those into a step to silence and back, which
             * is a click; 230 a second is the buzz that was audible over speech.
             * The CD input on hardware is a continuous signal that the SPU samples,
             * so repeating the previous value is much closer to the truth than
             * dropping to zero, and one held sample is inaudible where a zero is
             * not. The counter stays, because a starving queue is still a fault
             * worth seeing even once it stops being audible. */
            /* File-static rather than a field on Spu: adding one would change
             * sizeof(Spu), and the savestate stores the SPU as one sized span, so
             * every existing state would stop loading. This is a single sample of
             * hold state — losing it across a save and load costs one frame of a
             * signal that is already continuous. */
            static int16_t s_cd_last_l = 0, s_cd_last_r = 0;

            int16_t cl = s_cd_last_l, cr = s_cd_last_r;
            if (!cdrom_audio_fifo_empty(&inter->cdrom.audio_fifo)) {
                cdrom_audio_fifo_pop(&inter->cdrom.audio_fifo, &cl, &cr);
                s_cd_last_l = cl;
                s_cd_last_r = cr;
                static bool s_avol_zero_logged = false;
                if (!s_avol_zero_logged && (spu->control & SPU_CTRL_CD_AUDIO_EN) &&
                    spu->cd_vol_left == 0 && spu->cd_vol_right == 0) {
                    s_avol_zero_logged = true;
                    LOG_SPU_INFO("[SPU] CD audio is arriving with the CD input volume "
                                 "(AVOLL/AVOLR, 1F801DB0h/DB2h) at 0: silent until the "
                                 "guest writes it");
                }
            } else {
                inter->cdrom.audio_fifo.total_starved++;
            }
            /* The CD controller's output stage: Mute, ADPMUTE and the ATV
             * matrix (psx-spx cdr/cdromdrive.md:225-255, :1019-1022). It sits
             * in the drive, ahead of the SPU, and it used to be dead code: the
             * one function that applied it had no caller once the SPU started
             * reading this FIFO directly. Applied after the hold above, so a
             * Mute silences a held sample too. The SPU's own CD volume
             * (AVOLL/AVOLR) comes after, in spu_generate_one_sample, past the
             * capture point. */
            cdrom_apply_output_volume(&inter->cdrom, &cl, &cr);
            spu->cd_audio_left  = cl;
            spu->cd_audio_right = cr;
        }

        /* Generate one stereo sample */
        int16_t l, r;
        spu_generate_one_sample(spu, inter, &l, &r);

        /* Push to circular buffer — lock-free SPSC.
         * Producer owns tail; consumer owns head. Only the producer writes tail
         * (after writing data), so consumer can safely read tail without a lock.
         * __ATOMIC_ACQUIRE on head ensures we see consumer's latest progress.
         * __ATOMIC_RELEASE on tail ensures data writes are visible before tail update. */
        {
            int next_tail = (spu->sample_buf_tail + 1) % SPU_SAMPLE_BUFFER_SIZE;
            int head = __atomic_load_n(&spu->sample_buf_head, __ATOMIC_ACQUIRE);
            if (next_tail != head) {  /* not full */
                int tidx = spu->sample_buf_tail * 2;
                spu->sample_buffer[tidx]     = l;
                spu->sample_buffer[tidx + 1] = r;
                __atomic_store_n(&spu->sample_buf_tail, next_tail, __ATOMIC_RELEASE);
                spu->sample_buf_count++;  /* approximate, for debug display */
                spu->total_samples_generated++;
            } else {
                spu->dropped_samples++;   /* ring full: state advanced, audio lost */
            }
        }
    }

    /* Decay peak levels */
    spu->peak_level_left = (spu->peak_level_left * 15) >> 4;
    spu->peak_level_right = (spu->peak_level_right * 15) >> 4;
}

int spu_ring_used(const Spu* spu) {
    int head = __atomic_load_n(&spu->sample_buf_head, __ATOMIC_ACQUIRE);
    return (spu->sample_buf_tail - head + SPU_SAMPLE_BUFFER_SIZE) % SPU_SAMPLE_BUFFER_SIZE;
}

/* Generate everything owed since the last call. Called from the scheduled SPU
 * event and before every SPU register access, so a write can never land in the
 * middle of a sample the guest believes it already produced.
 *
 * The elapsed count is a uint32 difference, so it stays correct across the
 * cycle-counter wrap. It is capped at one second of audio: the only ways to owe
 * more than that are a debugger pause or the very first call, and generating
 * 44100 samples' worth of catch-up in one go would stall the emulator without
 * making anything audible. */
/* Wall-clock nanoseconds spent generating samples, for the frame profiler. */
uint64_t g_spu_gen_ticks = 0;

void spu_catch_up(struct Interconnect* inter) {
    Spu* spu = &inter->spu;
    static int s_prof = -1;
    if (s_prof < 0) s_prof = getenv("ZS1_FRAME_PROFILE") ? 1 : 0;
    uint64_t t_begin = s_prof ? SDL_GetPerformanceCounter() : 0;
    uint32_t now     = inter->cpu_cycle_counter;
    uint32_t elapsed = now - spu->last_update_cycle;

    const uint32_t max_elapsed = (uint32_t)CPU_TICKS_PER_SPU_TICK * SAMPLE_RATE;
    if (elapsed > max_elapsed) {
        elapsed = 0;                 /* resynchronise, do not flood the ring */
        spu->spu_tick_counter = 0;
    }

    spu->last_update_cycle = now;
    if (elapsed) spu_step(inter, elapsed);
    if (s_prof) g_spu_gen_ticks += SDL_GetPerformanceCounter() - t_begin;
}

/* =========================================================================
 * SPU Get Samples (for SDL callback or other consumers)
 * ========================================================================= */

int spu_get_samples(Spu* spu, int16_t* buffer, int max_samples) {
    int count = 0;
    while (count < max_samples) {
        /* __ATOMIC_ACQUIRE on tail: ensure we see data written before tail update */
        int tail = __atomic_load_n(&spu->sample_buf_tail, __ATOMIC_ACQUIRE);
        if (spu->sample_buf_head == tail) break;  /* empty */
        int hidx = spu->sample_buf_head * 2;
        buffer[count * 2]     = spu->sample_buffer[hidx];
        buffer[count * 2 + 1] = spu->sample_buffer[hidx + 1];
        int next_head = (spu->sample_buf_head + 1) % SPU_SAMPLE_BUFFER_SIZE;
        /* __ATOMIC_RELEASE on head: producer can now use this slot */
        __atomic_store_n(&spu->sample_buf_head, next_head, __ATOMIC_RELEASE);
        if (spu->sample_buf_count > 0) spu->sample_buf_count--;
        count++;
    }
    return count;
}

/* =========================================================================
 * SPU Fill Audio (SDL callback helper)
 * ========================================================================= */

/* How far ahead of its working set the stretcher is allowed to pull.
 *
 * This bound is not a detail. The emulation loop paces itself on the ring level
 * (src/main.c: it runs ahead only while the ring holds less than
 * SPU_RING_TARGET_SAMPLES), so the ring is the machine's clock. Draining it into
 * the stretcher faster than the device plays would make the whole emulated
 * machine run fast — CPU, video and audio together — which is exactly what a
 * first version of this did at 1.25x. The stretcher therefore takes only what it
 * needs to serve this callback and leaves the rest of the queue in the ring
 * where the pacer can see it. */
#define STRETCH_HOLD_MARGIN    640

/* SPU_RING_TARGET_SAMPLES, see include/spu.h. Read once and clamped so the ring
 * size is never in question; spu_init() calls this first, on the emulation
 * thread, so the audio thread only ever reads the latched value. */
int spu_ring_target_samples(void) {
    static int s_target = 0;
    if (s_target == 0) {
        int target = SPU_RING_TARGET_DEFAULT;
        const char* env = getenv("ZS1_SPU_RING_TARGET");
        if (env && *env) {
            char* end = NULL;
            long v = strtol(env, &end, 10);
            if (end != env) {
                if (v < SPU_RING_TARGET_MIN) v = SPU_RING_TARGET_MIN;
                if (v > SPU_RING_TARGET_MAX) v = SPU_RING_TARGET_MAX;
                target = (int)v;
                LOG_SPU_INFO("[SPU] Output ring target %d frames (ZS1_SPU_RING_TARGET=%s, "
                             "range %d-%d)", target, env,
                             SPU_RING_TARGET_MIN, SPU_RING_TARGET_MAX);
            } else {
                LOG_SPU_WARN("[SPU] ZS1_SPU_RING_TARGET=%s is not a number, keeping %d",
                             env, target);
            }
        }
        s_target = target;
    }
    return s_target;
}

/* Queue depth the tempo controller aims for, counting everything between the
 * mixer and the device: what the pacer parks in the ring, plus the margin above,
 * plus SPU_STRETCH_WORKING that can never be played because it is the block and
 * search window the stretcher needs in hand before it can emit anything. */
#define STRETCH_TARGET_FRAMES  (SPU_STRETCH_WORKING + STRETCH_HOLD_MARGIN + \
                                SPU_RING_TARGET_SAMPLES)
/* Fraction of target the queue may wander before the tempo moves. Inside it the
 * stretcher is bit-exact passthrough, so ordinary clock wander costs nothing. */
#define STRETCH_DEAD_BAND      0.20

/* Staging for one callback's worth of ring reads. Audio-thread only, like the
 * stretcher itself, so a static is safe and keeps it off the callback's stack. */
static int16_t s_stretch_stage[SPU_STRETCH_IN_CAP * 2];

static int stretch_enabled(void) {
    static int cached = -1;
    if (cached < 0) cached = getenv("ZS1_SPU_NO_STRETCH") ? 0 : 1;
    return cached;
}

void spu_fill_audio(Spu* spu, int16_t* stream, int num_stereo_samples) {
    int filled;

    if (!stretch_enabled()) {
        filled = spu_get_samples(spu, stream, num_stereo_samples);
    } else {
        /* Top the stretcher up to its bound, no further — see STRETCH_HOLD_MARGIN.
         * Frames beyond that stay in the ring, which is what the emulation loop
         * paces against. */
        int want    = SPU_STRETCH_WORKING + STRETCH_HOLD_MARGIN;
        int deficit = want - spu_stretch_queued(&spu->stretch);
        int room    = spu_stretch_input_room(&spu->stretch);
        if (deficit > room) deficit = room;
        if (deficit > 0) {
            int got = spu_get_samples(spu, s_stretch_stage, deficit);
            if (got > 0) spu_stretch_push(&spu->stretch, s_stretch_stage, got);
        }

        int depth = spu_ring_used(spu) + spu_stretch_queued(&spu->stretch);

        /* Tempo is input frames consumed per output frame emitted. Above 1 the
         * queue drains faster than it fills, below 1 slower; the output rate
         * never changes, so the correction is inaudible as pitch. */
        double dev   = ((double)depth - STRETCH_TARGET_FRAMES) / STRETCH_TARGET_FRAMES;
        double tempo = 1.0;
        if (dev > STRETCH_DEAD_BAND || dev < -STRETCH_DEAD_BAND)
            tempo = 1.0 + dev;
        if (tempo < SPU_STRETCH_TEMPO_MIN) tempo = SPU_STRETCH_TEMPO_MIN;
        if (tempo > SPU_STRETCH_TEMPO_MAX) tempo = SPU_STRETCH_TEMPO_MAX;

        bool active = (tempo != 1.0);
        if (active && !spu->stretch_active) spu->stretch_periods++;
        spu->stretch_active = active;
        spu->stretch_tempo  = tempo;
        spu_stretch_set_tempo(&spu->stretch, tempo);

        filled = spu_stretch_pull(&spu->stretch, stream, num_stereo_samples);
    }

    /* Fill remaining with silence if buffer underrun */
    if (filled < num_stereo_samples) {
        spu->underrun_events++;
        spu->underrun_samples += (uint32_t)(num_stereo_samples - filled);
    }
    for (int i = filled; i < num_stereo_samples; i++) {
        stream[i * 2] = 0;
        stream[i * 2 + 1] = 0;
    }
}

/* =========================================================================
 * Legacy spu_generate_samples (kept for compatibility)
 * ========================================================================= */

void spu_generate_samples(Spu* spu, int16_t* buffer, int num_samples) {
    for (int s = 0; s < num_samples; s++) {
        int16_t l, r;
        spu_generate_one_sample(spu, NULL, &l, &r);
        buffer[s * 2] = l;
        buffer[s * 2 + 1] = r;
    }
}
