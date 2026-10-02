/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 ZioZoni95
 * SPDX-FileCopyrightText: 2002 Pete Bernert and the PCSX-Redux authors
 *
 * Part of ZoniStation One, a PlayStation 1 emulator.
 * See LICENSE for the full licence text and THIRD-PARTY.md for the
 * components of this project that have other authors.
 */
/*
 * SPU voice decode & mixing — ported 1:1 from pcsx-redux (Pete Bernert / PCSX-Redux authors)
 * Original: pcsx-redux/src/spu/spu.cc  (GPL-2.0+)
 * Adapted to C for ZonistationOne.
 */

#include "spu.h"
#include "interconnect.h"
#include "log.h"
#include <string.h>

extern int spu_adsr_mix(SpuVoice* voice);

/* =========================================================================
 * ADPCM filter coefficients (same as pcsx-redux f[5][2])
 * ========================================================================= */
static inline int adpcm_clamp16(int v) {
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return v;
}

static const int adpcm_f[5][2] = {
    {   0,    0 },
    {  60,    0 },
    { 115,  -52 },
    {  98,  -55 },
    { 122,  -60 },
};

/* =========================================================================
 * 4-point Gaussian interpolation — the hardware table and formula
 *
 * Transcribed from `DOCS/soundprocessingunitspu.md:225-291` ("The Gauss table
 * contains the following values") and applied with the interpolation given at
 * `:215-224`. 512 entries, indexed four at a time by the 8-bit index taken from
 * the pitch counter (`:201`, "Counter.Bit4..11 are used as 8bit gaussian
 * interpolation index" — bits 8..15 of our 16-bit fractional `spos`).
 *
 * Self-checking: the documentation notes at `:295-299` that the real table is
 * slightly bugged, each group of four summing to 7F7Fh..7F81h rather than the
 * theoretical 8000h. The transcription reproduces exactly that range, which is
 * a strong check that it was copied down correctly.
 *
 * This replaces a 1024-entry table taken from pcsx-redux (Chris Moeller), which
 * is a differently normalised variant used with a >>11 accumulator. The table
 * below is the console's own, and it comes from the hardware documentation.
 * ========================================================================= */
static const int gauss_table[512] = {
        -1,     -1,     -1,     -1,     -1,     -1,     -1,     -1,
        -1,     -1,     -1,     -1,     -1,     -1,     -1,     -1,
         0,      0,      0,      0,      0,      0,      0,      1,
         1,      1,      1,      2,      2,      2,      3,      3,
         3,      4,      4,      5,      5,      6,      7,      7,
         8,      9,      9,     10,     11,     12,     13,     14,
        15,     16,     17,     18,     19,     21,     22,     24,
        25,     27,     28,     30,     32,     33,     35,     37,
        39,     41,     44,     46,     48,     51,     53,     56,
        58,     61,     64,     67,     70,     73,     77,     80,
        84,     87,     91,     95,     99,    103,    107,    111,
       116,    120,    125,    130,    135,    140,    145,    150,
       156,    161,    167,    173,    179,    186,    192,    199,
       205,    212,    219,    227,    234,    242,    250,    257,
       266,    274,    283,    291,    300,    309,    319,    328,
       338,    348,    358,    369,    379,    390,    401,    412,
       424,    436,    448,    460,    473,    485,    498,    512,
       525,    539,    553,    567,    582,    597,    612,    627,
       643,    659,    675,    692,    708,    726,    743,    761,
       779,    797,    816,    835,    854,    874,    894,    914,
       935,    956,    977,    999,   1020,   1043,   1066,   1089,
      1112,   1136,   1160,   1184,   1209,   1234,   1260,   1286,
      1312,   1339,   1366,   1394,   1422,   1450,   1479,   1508,
      1537,   1567,   1598,   1628,   1660,   1691,   1723,   1756,
      1789,   1822,   1856,   1890,   1924,   1959,   1995,   2031,
      2067,   2104,   2141,   2179,   2217,   2256,   2295,   2334,
      2374,   2415,   2456,   2497,   2539,   2582,   2624,   2668,
      2712,   2756,   2801,   2846,   2892,   2938,   2985,   3032,
      3079,   3128,   3176,   3225,   3275,   3325,   3376,   3427,
      3479,   3531,   3584,   3637,   3691,   3745,   3799,   3855,
      3910,   3967,   4023,   4081,   4138,   4197,   4255,   4315,
      4374,   4435,   4495,   4557,   4619,   4681,   4744,   4807,
      4871,   4935,   5000,   5065,   5131,   5197,   5264,   5332,
      5399,   5468,   5536,   5606,   5676,   5746,   5817,   5888,
      5959,   6032,   6104,   6177,   6251,   6325,   6400,   6475,
      6550,   6626,   6702,   6779,   6856,   6934,   7012,   7091,
      7170,   7249,   7329,   7409,   7490,   7571,   7653,   7735,
      7817,   7900,   7983,   8066,   8150,   8234,   8319,   8404,
      8489,   8575,   8661,   8748,   8834,   8922,   9009,   9097,
      9185,   9273,   9362,   9451,   9541,   9630,   9720,   9811,
      9901,   9992,  10083,  10174,  10266,  10358,  10450,  10542,
     10635,  10727,  10820,  10913,  11007,  11100,  11194,  11288,
     11382,  11476,  11571,  11665,  11760,  11855,  11950,  12045,
     12140,  12236,  12331,  12427,  12522,  12618,  12714,  12809,
     12905,  13001,  13097,  13193,  13289,  13385,  13481,  13577,
     13673,  13769,  13865,  13961,  14056,  14152,  14248,  14343,
     14439,  14534,  14630,  14725,  14820,  14915,  15010,  15104,
     15199,  15293,  15387,  15481,  15575,  15669,  15762,  15855,
     15948,  16041,  16133,  16226,  16317,  16409,  16500,  16592,
     16682,  16773,  16863,  16953,  17042,  17131,  17220,  17308,
     17396,  17484,  17571,  17658,  17744,  17830,  17916,  18001,
     18086,  18170,  18254,  18337,  18420,  18502,  18584,  18665,
     18746,  18826,  18905,  18985,  19063,  19141,  19219,  19295,
     19372,  19447,  19522,  19597,  19671,  19744,  19816,  19888,
     19959,  20030,  20100,  20169,  20238,  20306,  20373,  20439,
     20505,  20570,  20634,  20698,  20760,  20822,  20884,  20944,
     21004,  21063,  21121,  21178,  21235,  21290,  21345,  21399,
     21452,  21505,  21556,  21607,  21657,  21706,  21754,  21801,
     21848,  21893,  21938,  21982,  22025,  22066,  22107,  22148,
     22187,  22225,  22262,  22299,  22334,  22369,  22402,  22435,
     22467,  22498,  22527,  22556,  22584,  22611,  22637,  22662,
     22686,  22709,  22731,  22752,  22772,  22791,  22809,  22826,
     22842,  22857,  22872,  22885,  22897,  22908,  22918,  22927,
     22935,  22942,  22948,  22953,  22957,  22960,  22962,  22963,
};

/* =========================================================================
 * Gauss interpolation — pcsx-redux 1:1
 * ========================================================================= */

static int voice_interpolate(SpuVoice* voice) {
    /* DOCS:215-224 —
     *   out  = (gauss[0FFh-i] * oldest) SAR 15
     *   out += (gauss[1FFh-i] * older ) SAR 15
     *   out += (gauss[100h+i] * old   ) SAR 15
     *   out += (gauss[000h+i] * new   ) SAR 15
     * The ring holds the four most recent samples, oldest first at gpos. */
    const int i = (voice->spos >> 8) & 0xFF;
    const int gpos = voice->gpos;
    const int oldest = (int)voice->gauss_ring[gpos];
    const int older  = (int)voice->gauss_ring[(gpos + 1) & 3];
    const int old    = (int)voice->gauss_ring[(gpos + 2) & 3];
    const int newest = (int)voice->gauss_ring[(gpos + 3) & 3];

    int out  = (gauss_table[0x0FF - i] * oldest) >> 15;
    out     += (gauss_table[0x1FF - i] * older ) >> 15;
    out     += (gauss_table[0x100 + i] * old   ) >> 15;
    out     += (gauss_table[0x000 + i] * newest) >> 15;
    return out;
}

static void store_interp(SpuVoice* voice, int fa) {
    if (fa > 32767)  fa =  32767;
    if (fa < -32767) fa = -32767;
    voice->gauss_ring[voice->gpos] = (int16_t)fa;
    voice->gpos = (voice->gpos + 1) & 3;
}

/* =========================================================================
 * ADPCM block fetch
 *
 * One 16-byte block per 28 samples: header byte (shift/filter), flags byte,
 * then 14 bytes of nibbles (psx-spx spu/soundprocessingunitspu.md:149-174).
 * The sample decode is the pcsx-redux loop; the address, IRQ and loop handling
 * around it are written from the documentation:
 *
 *  - the IRQ address traps the block being *read*, at the moment it is read
 *    (:824). This compared against `curr_addr + 16`, the block after the one
 *    just decoded, so an IRQ address on the first block of a looping buffer
 *    never fired: the block that precedes it in memory is not part of the
 *    loop. That is the classic double-buffered SPU stream (:855-866), whose
 *    refill then never came.
 *  - Loop Start copies the current address to the repeat address, every time
 *    (:134-135, :165). A software write to the repeat address used to switch
 *    this off until the next Key On.
 *  - Loop End jumps to the repeat address after the block has been played, and
 *    sets ENDX (:136-138, :163). It used to jump only when this voice had seen
 *    a Loop Start since Key On, and to stop the voice otherwise, so a ring with
 *    no Loop Start flag, looped through a repeat address the game wrote before
 *    Key On, played once and went silent.
 *  - Code 1 (End without Repeat) still jumps, and also forces Release with the
 *    envelope at 0 (:164, :171). The voice keeps reading: "There's no way to
 *    stop the output" (:180).
 *
 * `decode` is false for a silent voice: the samples are not needed, only the
 * position, the flags and the IRQ check are.
 * ========================================================================= */

static void voice_fetch_block(Spu* spu, struct Interconnect* inter, SpuVoice* voice, bool decode) {
    /* Code 1 on the block just played: its Release and zero envelope are due
     * now that the block is done, on the same "after playing the current ADPCM
     * block" boundary as the jump itself (:136-138). A silent voice has no
     * envelope left to force. */
    if (voice->reach_end) {
        voice->reach_end = false;
        if (voice->on) {
            voice->adsr_state  = ADSR_STATE_RELEASE;
            voice->stop        = false;
            voice->EnvelopeVol = 0;
            voice->adsr_volume = 0;
        }
    }

    const uint32_t mask = SPU_RAM_SIZE - 1;
    const uint32_t a = voice->curr_addr & mask;          /* this block */
    const uint8_t* ram8 = (const uint8_t*)spu->ram;

    spu_check_irq_range(spu, inter, a, ADPCM_BLOCK_SIZE);

    const int flags = (int)ram8[(a + 1) & mask];

    if (decode) {
        int predict_nr   = (int)ram8[a];
        int shift_factor = predict_nr & 0xF;
        predict_nr >>= 4;

        if (predict_nr > 4) predict_nr = 4;
        if (shift_factor > 12) shift_factor = 9;

        int s_1 = voice->s_1;
        int s_2 = voice->s_2;
        unsigned int nSample = 0;

        /* The decoded sample is saturated to 16 bits *before* it becomes filter
         * state. The SPU's datapath is 16-bit, and the equivalent CD-XA decoder
         * is explicit about it (`DOCS/cdromformat.md:836-837`, already applied
         * in `cdrom_audio.c`); the sample is clamped to 16 bits on decode here
         * too. Feeding the raw prediction back lets one overflowing nibble
         * poison the remaining 27 samples of the block, and leaves out-of-range
         * values in SB[] that are only clamped much later, after the envelope
         * and volume have already scaled them. */
        for (uint32_t i = 2; nSample < 28; i++) {
            int d = (int)ram8[(a + i) & mask];
            int s = (d & 0x0F) << 12;
            if (s & 0x8000) s |= 0xFFFF0000;
            int fa = (s >> shift_factor) + ((s_1 * adpcm_f[predict_nr][0]) >> 6) + ((s_2 * adpcm_f[predict_nr][1]) >> 6);
            fa = adpcm_clamp16(fa);
            s_2 = s_1; s_1 = fa;
            voice->SB[nSample++] = fa;

            s = (d & 0xF0) << 8;
            if (s & 0x8000) s |= 0xFFFF0000;
            fa = (s >> shift_factor) + ((s_1 * adpcm_f[predict_nr][0]) >> 6) + ((s_2 * adpcm_f[predict_nr][1]) >> 6);
            fa = adpcm_clamp16(fa);
            s_2 = s_1; s_1 = fa;
            voice->SB[nSample++] = fa;
        }

        voice->s_1 = s_1;
        voice->s_2 = s_2;
    }

    /* Loop Start: the repeat address becomes this block (:134-135). */
    if (flags & 4)
        voice->repeat_address = (uint16_t)(a >> 3);

    /* Loop End: the next block is the one at the repeat address, read once this
     * block has played, which is the next fetch (:136-138). Applied after Loop
     * Start, so a block carrying both repeats itself: the documented silent
     * dummy loop (:180-182). */
    if (flags & 1) {
        voice->curr_addr = ((uint32_t)voice->repeat_address * 8) & mask;
        voice->endx_mask = true;
        if (!(flags & 2))
            voice->reach_end = true;     /* Code 1, End+Mute (:171) */
    } else {
        voice->curr_addr = (a + ADPCM_BLOCK_SIZE) & mask;
        voice->endx_mask = false;
    }

    voice->SBPos = 0;

    LOG_SPU_TRACE("[SPU] Voice block 0x%05X flags=0x%02X next=0x%05X lsa=0x%04X",
                  a, flags, voice->curr_addr, voice->repeat_address);
}

/* Pitch counter step for one output sample, DOCS/soundprocessingunitspu.md:
 * 187-199, transcribed in the documentation's own units (one sample = 1000h);
 * the caller shifts it, because this file's spos counter runs at 16x that (one
 * sample = 10000h).
 *
 *   Step = VxPitch                  ;range +0000h..+FFFFh
 *   IF PMON.Bit(x)=1 AND (x>0)
 *     Factor = VxOUTX(x-1)          ;range -8000h..+7FFFh
 *     Factor = Factor+8000h         ;range +0000h..+FFFFh
 *     Step = SignExpand16to32(Step) ;hardware glitch on VxPitch>7FFFh
 *     Step = (Step * Factor) SAR 15
 *     Step = Step AND 0000FFFFh     ;hardware glitch on VxPitch>7FFFh
 *   IF Step>3FFFh then Step=4000h   ;range +0000h..+3FFFh (0..176.4 kHz)
 *
 * The last line is the one that matters here, and it sits *outside* the
 * modulation branch: every voice is capped at 4000h, modulated or not. This
 * code only capped modulated voices, so an unmodulated voice could step at up
 * to FFFFh, 705.6 kHz against the 176.4 kHz the hardware allows, four times
 * too fast. Playing sample data at four times its rate through a 4-point
 * interpolator with no low-pass aliases straight to the top of the band, which
 * is audible as short bursts of buzz rather than as a wrong note.
 *
 * Note the cap sets 4000h; it does not clamp to 3FFFh. The two differ by one
 * step and the documentation is explicit about which it is. */
static inline int32_t voice_pitch_step(const Spu* spu, const SpuVoice* voice, int voice_idx) {
    int32_t step = (int32_t)voice->pitch;                  /* 0000h..FFFFh */

    if (voice_idx > 0 && ((spu->pitch_mod >> voice_idx) & 1)) {
        int32_t factor = (int32_t)spu->voices[voice_idx - 1].sval + 0x8000;
        step = (int32_t)(int16_t)step;                     /* SignExpand16to32 */
        step = (int32_t)(((int64_t)step * factor) >> 15);
        step &= 0xFFFF;
    }

    if (step > 0x3FFF) step = 0x4000;
    return step;
}

/* A voice whose envelope has finished, that was keyed off, or that ended on a
 * Code 1 block is silent, but it is still reading SPU RAM: "all voices are
 * permanently reading data from SPU RAM - even in Noise mode, even if the Voice
 * Volume is zero, and even if the ADSR pattern has finished the Release period
 * - so even inaudible voices can trigger IRQs" (soundprocessingunitspu.md:
 * 825-829). Such a voice used to freeze where it stopped, so an IRQ a game
 * timed off a muted voice never came.
 *
 * Only the position moves here (pitch counter, block flags, loops, ENDX and the
 * IRQ check); nothing is decoded, interpolated or mixed, so a silent voice
 * costs an add and a compare per sample and one header read per 28 samples.
 * Key On re-initialises everything this leaves stale (decoder history, Gauss
 * ring). */
static void voice_advance_silent(Spu* spu, struct Interconnect* inter, SpuVoice* voice, int voice_idx) {
    int32_t step = voice_pitch_step(spu, voice, voice_idx);
    while (voice->spos >= 0x10000) {
        if (voice->SBPos >= 28) voice_fetch_block(spu, inter, voice, false);
        voice->SBPos++;
        voice->spos -= 0x10000;
    }
    voice->spos += step << 4;

    if (voice->endx_mask) {
        spu->endx |= (1u << voice_idx);
        voice->endx_mask = false;
    }
}

/* =========================================================================
 * Main voice sample generator, after the pcsx-redux MainThread inner loop.
 * Returns ADSR-mixed sample (before L/R volume).  Caller accumulates.
 * ========================================================================= */

int32_t spu_voice_get_sample(Spu* spu, struct Interconnect* inter, int voice_idx) {
    SpuVoice* voice = &spu->voices[voice_idx];

    if (!voice->on) {
        voice->sval = 0;
        /* With SPUCNT.15 = 0 the SPU is off (soundprocessingunitspu.md:630) and
         * spu_set_control has forced every voice off; nothing reads RAM then. */
        if (spu->control & SPU_CTRL_ENABLE)
            voice_advance_silent(spu, inter, voice, voice_idx);
        return 0;
    }

    /* Key-off: transition ADSR to Release phase */
    if (voice->stop) {
        voice->adsr_state = ADSR_STATE_RELEASE;
        voice->stop = false;
    }

    int sinc = voice_pitch_step(spu, voice, voice_idx) << 4;

    /* Advance spos: decode samples into gauss ring until spos < 0x10000 */
    while (voice->spos >= 0x10000) {
        if (voice->SBPos >= 28) voice_fetch_block(spu, inter, voice, true);

        int fa = voice->SB[voice->SBPos++];
        store_interp(voice, fa);
        voice->spos -= 0x10000;
    }

    /* Get interpolated sample */
    int fa;
    if (spu->noise_mode & (1u << voice_idx)) {
        fa = (int)(int16_t)spu->noise_level;
    } else {
        fa = voice_interpolate(voice);
    }

    /* ADSR envelope mix: returns 0-32767 (15-bit) */
    int32_t adsr_vol = spu_adsr_mix(voice);
    int32_t mixed = ((int32_t)fa * adsr_vol) >> 15;

    /* VxOUTX is a 16-bit signed value: "Factor = VxOUTX(x-1) ;range -8000h..+7FFFh
     * (prev voice amplitude)", DOCS/soundprocessingunitspu.md:192. This clamped to
     * +/-FFFFh, twice the range the register can hold, which let a voice put double
     * its legal excursion into the dry mix and doubled the swing of the pitch
     * modulation factor a following voice reads out of it. */
    if (mixed >  32767) mixed =  32767;
    if (mixed < -32768) mixed = -32768;

    voice->sval = mixed;

    /* Advance spos for next call */
    voice->spos += sinc;

    /* Update ENDX status */
    if (voice->endx_mask) {
        spu->endx |= (1u << voice_idx);
        voice->endx_mask = false;
    }

    return mixed;
}

/* =========================================================================
 * Sweep volume: psx-spx spu/soundprocessingunitspu.md:405-435 (register
 * format) and :447-482 (the envelope operation it runs, once per 44.1 kHz
 * clock). Transcribed from the pseudo-code:
 *
 *   AdsrStep = 7 - StepValue
 *   IF Decreasing XOR PhaseNegative THEN AdsrStep = NOT AdsrStep
 *   AdsrStep = AdsrStep SHL Max(0,11-ShiftValue)
 *   CounterIncrement = 8000h SHR Max(0,ShiftValue-11)
 *   IF exponential AND increase AND AdsrLevel>6000h THEN
 *     IF ShiftValue < 10 THEN AdsrStep /= 4
 *     ELSE IF ShiftValue >= 11 THEN CounterIncrement /= 4
 *     ELSE AdsrStep /= 2, CounterIncrement /= 2
 *   ELSE IF exponential AND decrease THEN AdsrStep=AdsrStep*AdsrLevel/8000h
 *   IF (StepValue | (ShiftValue SHL 2)) != ALL_BITS THEN
 *     CounterIncrement = MAX(CounterIncrement, 1)
 *   Counter += CounterIncrement
 *   IF (Counter & 8000h) == 0 THEN RETURN
 *   AdsrLevel = AdsrLevel + AdsrStep
 *   IF NOT decreasing THEN AdsrLevel = CLAMP(AdsrLevel, -8000h..+7FFFh)
 *   ELSE IF PhaseNegative THEN AdsrLevel = CLAMP(AdsrLevel, -8000h..0h)
 *   ELSE AdsrLevel = MAX(AdsrLevel, 0)
 *
 * What it changes against the code it replaces:
 *  - a linear decrease stops at 0 (:431-432, :480-481). It used to run on to
 *    -8000h, so a fade-out went through silence and came back at full volume
 *    with the phase inverted: the previous scene's sound "returning";
 *  - the phase bit (:419, :433-435, :498-503) inverts the step and picks the
 *    clamp. It was ignored;
 *  - an exponential increase above 6000h is a slower *linear* step (:455-462,
 *    :484-485), not a step proportional to the level;
 *  - all-ones step and shift never step (:466-467, :490-491).
 *
 * Two points the pseudo-code leaves implicit:
 *  - the counter is cleared when it steps. That is what makes a step every
 *    `1 SHL Max(0,ShiftValue-11)` cycles, the older formula the text says is
 *    right up to shift 26 (:493-496), and what the code here always did;
 *  - "/8000h" in the exponential decrease is taken as SAR 15, as in the ADSR
 *    envelope (spu_adsr.c), so a decaying level reaches 0 instead of stalling
 *    one step above it.
 * ========================================================================= */
int spu_sweep_tick(uint16_t reg, int level, int32_t* counter) {
    const int  shift      = (reg >> 2) & 0x1F;
    const int  step_value = reg & 0x03;
    const bool exp_mode   = (reg & 0x4000) != 0;
    const bool decreasing = (reg & 0x2000) != 0;
    const bool phase_neg  = (reg & 0x1000) != 0;

    int32_t step = 7 - step_value;
    if (decreasing != phase_neg) step = ~step;           /* +7..+4 -> -8..-5 */
    if (shift < 11) step *= (int32_t)1 << (11 - shift);
    int32_t inc = 0x8000 >> (shift > 11 ? shift - 11 : 0);

    if (exp_mode && !decreasing && level > 0x6000) {
        if (shift < 10)       step >>= 2;
        else if (shift >= 11) inc  >>= 2;
        else                { step >>= 1; inc >>= 1; }
    } else if (exp_mode && decreasing) {
        step = (step * (int32_t)level) >> 15;
    }
    if ((step_value | (shift << 2)) != 0x7F && inc < 1) inc = 1;

    *counter += inc;
    if (!(*counter & 0x8000)) return level;
    *counter = 0;

    int32_t v = (int32_t)level + step;
    if (!decreasing) {
        if (v >  0x7FFF) v =  0x7FFF;
        if (v < -0x8000) v = -0x8000;
    } else if (phase_neg) {
        if (v > 0)       v = 0;
        if (v < -0x8000) v = -0x8000;
    } else if (v < 0) {
        v = 0;
    }
    return (int)v;
}

/* Called once per sample per voice from spu_mixing.c. Only a register in sweep
 * mode (bit15=1) moves the level; fixed mode set it when it was written. */
void spu_voice_sweep_tick(SpuVoice* voice) {
    if (voice->volume_left & 0x8000)
        voice->vol_left = spu_sweep_tick(voice->volume_left, voice->vol_left,
                                         &voice->vol_left_count);
    if (voice->volume_right & 0x8000)
        voice->vol_right = spu_sweep_tick(voice->volume_right, voice->vol_right,
                                          &voice->vol_right_count);
}
