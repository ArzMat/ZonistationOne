/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 ZioZoni95
 *
 * Part of ZoniStation One, a PlayStation 1 emulator.
 * See LICENSE for the full licence text and THIRD-PARTY.md for the
 * components of this project that have other authors.
 */
/* SPU unit tests (docs/TESTING_PLAN_2026-08-20.md, layer 1).
 *
 * The SPU sources are compiled into this file whole, with the few functions
 * they call outside the SPU stubbed below. Every expectation cites the psx-spx
 * line it checks (spu/soundprocessingunitspu.md unless noted).
 */
#define _POSIX_C_SOURCE 200809L   /* unsetenv */
#include "log.h"

#include "../src/spu/spu.c"
#include "../src/spu/spu_voice.c"
#include "../src/spu/spu_adsr.c"
#include "../src/spu/spu_irq.c"
#include "../src/spu/spu_dma.c"
#include "../src/spu/spu_mixing.c"
#include "../src/spu/spu_stretch.c"
#include "../src/cdrom/cdrom_audio.c"

#include <stdio.h>
#include <stdlib.h>

/* ---- stubs ---------------------------------------------------------------- */

/* The LOG_* macros test this at the call site (include/log.h). */
LogLevel current_log_level = LOG_LEVEL_INFO;
void log_print(LogCategory category, LogLevel level, const char* format, ...) {
    (void)category; (void)level; (void)format;
}

void frame_events_record(FrameEventType type, uint32_t detail) { (void)type; (void)detail; }

Uint64 SDL_GetPerformanceCounter(void) { return 0; }

/* The IRQ line as the interrupt controller sees it: rising edges are what
 * reach I_STAT (psx-spx system/interrupts.md:27-28). */
static bool g_spu_line;
static int  g_spu_edges;
void interconnect_set_irq_line(Interconnect* inter, uint32_t irq_line, bool state) {
    (void)inter;
    if (irq_line != IRQ_SPU) return;
    if (state && !g_spu_line) g_spu_edges++;
    g_spu_line = state;
}

/* The CD controller's output stage lives in cdrom.c; here it is the pure
 * matrix from cdrom_audio.c with a test-controlled mute. */
static bool g_cd_muted;
void cdrom_apply_output_volume(const Cdrom* cdrom, int16_t* l, int16_t* r) {
    (void)cdrom;
    cdrom_audio_apply_output(l, r, g_cd_muted, 0x80, 0, 0, 0x80);
}

/* ---- harness ---------------------------------------------------------------- */

static Interconnect g_inter;
static int g_fail, g_checks;

#define CHECK(cond, ...) do {                                               \
    g_checks++;                                                             \
    if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__);    \
                   printf(__VA_ARGS__); printf("\n"); }                     \
} while (0)

#define SPU_REG(off) (0x1F801C00u + (off))
#define VREG(v, r)   (0x1F801C00u + (uint32_t)(v) * 0x10u + (r))

static Spu* spu(void) { return &g_inter.spu; }

static void wr(uint32_t addr, uint16_t v) { spu_write16(&g_inter, addr, v); }
static uint16_t rd(uint32_t addr) { return spu_read16(&g_inter, addr); }

static void reset_all(void) {
    memset(&g_inter, 0, sizeof(g_inter));
    spu_init(spu());
    cdrom_audio_init(&g_inter.cdrom.audio_fifo, &g_inter.cdrom.xa_adpcm_state);
    g_spu_line = false;
    g_spu_edges = 0;
    g_cd_muted = false;
}

/* A 16-byte ADPCM block at `addr`: shift 0 / filter 0, `flags`, nibbles `fill`. */
static void put_block(uint32_t addr, uint8_t flags, uint8_t fill) {
    uint8_t* ram8 = (uint8_t*)spu()->ram;
    ram8[addr]     = 0x00;
    ram8[addr + 1] = flags;
    for (int i = 2; i < 16; i++) ram8[addr + i] = fill;
}

/* Pitch 1000h: one sample per output sample, so one block per 28 samples. */
static void voice_cfg(int v, uint32_t start) {
    wr(VREG(v, 0x0), 0x3FFF);
    wr(VREG(v, 0x2), 0x3FFF);
    wr(VREG(v, 0x4), 0x1000);
    wr(VREG(v, 0x6), (uint16_t)(start >> 3));
    wr(VREG(v, 0x8), 0x000F);             /* fastest attack, sustain level max */
    wr(VREG(v, 0xA), 0x0000);
}

static void key_on(int v)  { wr(SPU_REG(v < 16 ? 0x188 : 0x18A), (uint16_t)(1u << (v & 15))); }
static void key_off(int v) { wr(SPU_REG(v < 16 ? 0x18C : 0x18E), (uint16_t)(1u << (v & 15))); }

static void one_sample(int16_t* l, int16_t* r) {
    int16_t a, b;
    spu_generate_one_sample(spu(), &g_inter, &a, &b);
    if (l) *l = a;
    if (r) *r = b;
}

/* Run samples until voice v has fetched `n` blocks; record each block address. */
static void run_fetches(int v, int n, uint32_t* addrs) {
    SpuVoice* vc = &spu()->voices[v];
    int got = 0;
    spu_process_key_on_off(spu());       /* a pending Key On lands before the check */
    for (int guard = 0; got < n && guard < 100000; guard++) {
        if (vc->SBPos >= 28 && vc->spos >= 0x10000) addrs[got++] = vc->curr_addr;
        one_sample(NULL, NULL);
    }
}

/* ---- loop semantics (A3: V5, V6, V7, V8) ---------------------------------- */

static void test_loop_code3_with_loop_start(void) {
    reset_all();
    wr(SPU_REG(0x1AA), 0x8000);
    put_block(0x1000, 0x00, 0x11);       /* plain */
    put_block(0x1010, 0x04, 0x22);       /* Loop Start (:134-135) */
    put_block(0x1020, 0x03, 0x33);       /* Code 3, End+Repeat (:173) */
    voice_cfg(0, 0x1000);
    key_on(0);
    uint32_t a[7];
    run_fetches(0, 7, a);
    uint32_t want[7] = { 0x1000, 0x1010, 0x1020, 0x1010, 0x1020, 0x1010, 0x1020 };
    for (int i = 0; i < 7; i++)
        CHECK(a[i] == want[i], "code3 loop fetch %d: got %05x want %05x", i, a[i], want[i]);
    CHECK(spu()->voices[0].repeat_address == 0x1010 >> 3, "LSAX = Loop Start block");
    CHECK(spu()->voices[0].on, "Code 3 keeps the voice playing");
    CHECK(spu()->endx & 1u, "ENDX set at Loop End (:584)");
}

static void test_loop_code0_continues(void) {
    reset_all();
    wr(SPU_REG(0x1AA), 0x8000);
    put_block(0x2000, 0x00, 0x11);
    put_block(0x2010, 0x02, 0x11);       /* Code 2 = Ignored, same as Code 0 (:172) */
    put_block(0x2020, 0x00, 0x11);
    voice_cfg(1, 0x2000);
    key_on(1);
    uint32_t a[3];
    run_fetches(1, 3, a);
    CHECK(a[0] == 0x2000 && a[1] == 0x2010 && a[2] == 0x2020,
          "Code 0/2 continue at the next block: %05x %05x %05x", a[0], a[1], a[2]);
    CHECK(!(spu()->endx & 2u), "no ENDX without Loop End");
}

/* KON must not forget a repeat address written before it (:131, :133-138). */
static void test_kon_keeps_preset_lsax(void) {
    reset_all();
    wr(SPU_REG(0x1AA), 0x8000);
    put_block(0x3000, 0x00, 0x11);
    put_block(0x3010, 0x00, 0x11);
    put_block(0x3020, 0x03, 0x11);       /* Code 3, no Loop Start anywhere */
    voice_cfg(2, 0x3000);
    wr(VREG(2, 0xE), 0x3010 >> 3);       /* LSAX before Key On */
    key_on(2);
    uint32_t a[6];
    run_fetches(2, 6, a);
    uint32_t want[6] = { 0x3000, 0x3010, 0x3020, 0x3010, 0x3020, 0x3010 };
    for (int i = 0; i < 6; i++)
        CHECK(a[i] == want[i], "preset LSAX fetch %d: got %05x want %05x", i, a[i], want[i]);
    CHECK(spu()->voices[2].on, "voice still on after the Loop End");
    CHECK(spu()->voices[2].EnvelopeVol > 0, "envelope kept (Code 3 does not release)");
}

/* Code 1: jump, ENDX, Release, Env=0, and the voice keeps reading (:164, :171, :180). */
static void test_code1_end_mute(void) {
    reset_all();
    wr(SPU_REG(0x1AA), 0x8000);
    put_block(0x4000, 0x00, 0x77);
    put_block(0x4010, 0x01, 0x77);       /* Code 1, End+Mute */
    put_block(0x5000, 0x07, 0x00);       /* silent dummy loop (:180-182) */
    voice_cfg(3, 0x4000);
    wr(VREG(3, 0xA), 0x0000);            /* linear release, fastest */
    wr(VREG(3, 0xE), 0x5000 >> 3);
    key_on(3);
    uint32_t a[2];
    run_fetches(3, 2, a);
    SpuVoice* v = &spu()->voices[3];
    CHECK(a[1] == 0x4010, "second block fetched");
    CHECK(v->curr_addr == 0x5000, "Code 1 jumps to LSAX: %05x", v->curr_addr);
    CHECK(v->reach_end, "Release owed once the block has played");
    CHECK(v->EnvelopeVol > 0, "the End+Mute block itself still plays");
    /* Play to the next fetch. */
    for (int i = 0; i < 30; i++) one_sample(NULL, NULL);
    CHECK(v->EnvelopeVol == 0 && (v->adsr_state == ADSR_STATE_RELEASE || v->adsr_state == ADSR_STATE_STOPPED),
          "Release with Env=0 after the Code 1 block: env=%d state=%d", v->EnvelopeVol, v->adsr_state);
    CHECK(spu()->endx & (1u << 3), "ENDX set");
    /* Silent, but still reading the dummy loop at 0x5000 (:826-829). */
    for (int i = 0; i < 200; i++) one_sample(NULL, NULL);
    CHECK(!v->on, "linear release reached 0: voice out of the mix");
    CHECK(v->curr_addr == 0x5000, "silent voice keeps looping the dummy block: %05x", v->curr_addr);
}

/* A software LSAX write just sets it; a later Loop Start flag overrides it (V8). */
static void test_lsax_write_then_loop_start(void) {
    reset_all();
    wr(SPU_REG(0x1AA), 0x8000);
    put_block(0x6000, 0x00, 0x11);
    put_block(0x6010, 0x04, 0x11);       /* Loop Start */
    put_block(0x6020, 0x03, 0x11);
    voice_cfg(4, 0x6000);
    key_on(4);
    one_sample(NULL, NULL);
    wr(VREG(4, 0xE), 0x7000 >> 3);       /* written while playing block 0 */
    CHECK(spu()->voices[4].repeat_address == 0x7000 >> 3, "LSAX write lands");
    uint32_t a[4];
    run_fetches(4, 3, a);                /* fetches 0x6010 (Loop Start), 0x6020, then the loop */
    CHECK(spu()->voices[4].repeat_address == 0x6010 >> 3,
          "Loop Start after a software write still sets LSAX (:134-135): %04x",
          spu()->voices[4].repeat_address);
    CHECK(a[2] == 0x6010, "Loop End jumps to the Loop Start block: %05x", a[2]);
}

/* ---- IRQ (A2: I1, I2, I4, I5) --------------------------------------------- */

static void ack_irq9(void) {
    uint16_t c = spu()->control;
    wr(SPU_REG(0x1AA), (uint16_t)(c & ~0x40u));
    wr(SPU_REG(0x1AA), c);
}

/* IRQA on the block a looping voice re-enters (:824): every pass, not never. */
static void test_irq_at_loop_start_every_pass(void) {
    reset_all();
    put_block(0x1000, 0x00, 0x11);
    put_block(0x1010, 0x04, 0x11);
    put_block(0x1020, 0x03, 0x11);
    voice_cfg(0, 0x1000);
    wr(SPU_REG(0x1A4), 0x1010 >> 3);
    wr(SPU_REG(0x1AA), 0x8040);
    key_on(0);
    int irqs = 0;
    for (int i = 0; i < 28 * 20; i++) {
        one_sample(NULL, NULL);
        if (spu()->status & SPU_STATUS_IRQ9_FLAG) { irqs++; ack_irq9(); }
    }
    /* Blocks 0,1,2,1,2,... over 20 blocks: block 1 is read 10 times. */
    CHECK(irqs == 10, "IRQ on every read of the IRQA block: got %d want 10", irqs);
    CHECK(g_spu_edges == 10, "one edge per IRQ: %d", g_spu_edges);
}

/* Not one block early: the old code compared IRQA with curr_addr + 16. */
static void test_irq_not_early_and_mid_block(void) {
    reset_all();
    put_block(0x1000, 0x00, 0x11);
    put_block(0x1010, 0x00, 0x11);
    put_block(0x1020, 0x00, 0x11);
    voice_cfg(0, 0x1000);
    wr(SPU_REG(0x1A4), (0x1010 + 8) >> 3);   /* middle of block 1 (:831-834) */
    wr(SPU_REG(0x1AA), 0x8040);
    key_on(0);
    one_sample(NULL, NULL);                  /* fetches block 0 only */
    CHECK(!spu()->irq9_flag, "no IRQ while reading the block before IRQA");
    for (int i = 0; i < 28; i++) one_sample(NULL, NULL);   /* reaches block 1 */
    CHECK(spu()->irq9_flag, "IRQ when the block holding IRQA is read");
}

/* Writing IRQA or TSA is not an access (:824, :852). */
static void test_no_irq_on_register_write(void) {
    reset_all();
    put_block(0x1000, 0x00, 0x11);
    put_block(0x1010, 0x00, 0x11);
    voice_cfg(0, 0x1000);
    wr(SPU_REG(0x1A4), 0x8000 >> 3);              /* away from the capture buffers (:837) */
    wr(SPU_REG(0x1AA), 0x8040);
    key_on(0);
    one_sample(NULL, NULL);                       /* curr_addr is now 0x1010 */
    CHECK(spu()->voices[0].curr_addr == 0x1010, "voice parked at 0x1010");
    wr(SPU_REG(0x1A6), 0x2000 >> 3);              /* TSA */
    wr(SPU_REG(0x1A4), 0x1010 >> 3);              /* IRQA = the voice's next block */
    CHECK(!spu()->irq9_flag && g_spu_edges == 0, "IRQA write raises nothing");
    wr(SPU_REG(0x1A4), 0x2000 >> 3);              /* IRQA = the transfer address */
    CHECK(!spu()->irq9_flag && g_spu_edges == 0, "IRQA = TSA raises nothing");
    wr(SPU_REG(0x1A6), 0x2000 >> 3);              /* TSA = IRQA */
    CHECK(!spu()->irq9_flag && g_spu_edges == 0, "TSA write raises nothing");
    /* A transfer that writes there is an access (:852). */
    wr(SPU_REG(0x1AA), 0x8050);                   /* Manual Write, IRQ9 on */
    wr(SPU_REG(0x1A8), 0x1234);
    CHECK(spu()->irq9_flag && g_spu_edges == 1, "manual write at IRQA raises IRQ9");
}

/* The flag is acknowledged by SPUCNT.6 = 0 only (:635, :655). */
static void test_flag_ack_via_spucnt(void) {
    reset_all();
    wr(SPU_REG(0x1A4), 0x2000 >> 3);
    wr(SPU_REG(0x1A6), 0x2000 >> 3);
    wr(SPU_REG(0x1AA), 0x8050);
    wr(SPU_REG(0x1A8), 0x0001);                   /* IRQ */
    CHECK(spu()->irq9_flag && (rd(SPU_REG(0x1AE)) & 0x40), "flag and STATX.6 set");
    CHECK(g_spu_line, "SPU IRQ line high");
    /* A second match while the flag is up: no new edge. */
    wr(SPU_REG(0x1A6), 0x2000 >> 3);
    wr(SPU_REG(0x1A8), 0x0002);
    CHECK(g_spu_edges == 1, "no new edge while the flag is set: %d", g_spu_edges);
    /* SPUCNT rewritten with bit 6 still 1: no acknowledge. */
    wr(SPU_REG(0x1AA), 0x8050);
    CHECK(spu()->irq9_flag, "SPUCNT write keeping bit 6 does not acknowledge");
    wr(SPU_REG(0x1AA), 0x8010);                   /* bit 6 = 0 */
    CHECK(!spu()->irq9_flag && !(rd(SPU_REG(0x1AE)) & 0x40), "SPUCNT.6 = 0 clears flag and STATX.6");
    CHECK(!g_spu_line, "and drops the line");
    wr(SPU_REG(0x1AA), 0x8050);
    wr(SPU_REG(0x1A6), 0x2000 >> 3);
    wr(SPU_REG(0x1A8), 0x0003);
    CHECK(spu()->irq9_flag && g_spu_edges == 2, "next access after re-enable is a fresh edge");
}

/* A keyed-off, fully released voice keeps reading and can raise IRQs (:825-829). */
static void test_silent_voice_irq(void) {
    reset_all();
    put_block(0x1000, 0x04, 0x11);       /* Loop Start */
    put_block(0x1010, 0x00, 0x11);
    put_block(0x1020, 0x03, 0x11);
    voice_cfg(0, 0x1000);
    wr(SPU_REG(0x1AA), 0x8000);
    key_on(0);
    for (int i = 0; i < 10; i++) one_sample(NULL, NULL);
    key_off(0);
    for (int i = 0; i < 20; i++) one_sample(NULL, NULL);
    CHECK(!spu()->voices[0].on, "voice released to silence");
    wr(SPU_REG(0x1A4), 0x1000 >> 3);
    wr(SPU_REG(0x1AA), 0x8040);
    int irqs = 0;
    for (int i = 0; i < 28 * 9; i++) {
        one_sample(NULL, NULL);
        if (spu()->irq9_flag) { irqs++; ack_irq9(); }
    }
    CHECK(irqs >= 2, "silent voice still raises IRQs at its loop start: %d", irqs);
}

/* Capture writes go to SPU RAM 000h-FFFh and are trapped by IRQA (:51-56, :836-844). */
static void test_capture_into_ram(void) {
    reset_all();
    cdrom_audio_fifo_push(&g_inter.cdrom.audio_fifo, 1234, -2345);
    wr(SPU_REG(0x1AA), 0x8040);
    wr(SPU_REG(0x1A4), (0x400 + 0x10) >> 3);      /* CD right buffer, sample 8 */
    spu_step(&g_inter, CPU_TICKS_PER_SPU_TICK);   /* sample 0 */
    CHECK(spu()->ram[0x000] == (uint16_t)1234, "CD left sample 0 at 000h");
    CHECK(spu()->ram[0x200] == (uint16_t)(int16_t)-2345, "CD right sample 0 at 400h");
    CHECK(!spu()->irq9_flag, "no capture IRQ before the address is written");
    spu_step(&g_inter, CPU_TICKS_PER_SPU_TICK * 8);  /* samples 1..8 */
    CHECK(spu()->irq9_flag, "capture write at IRQA raises IRQ9");
    spu_step(&g_inter, CPU_TICKS_PER_SPU_TICK * (0x100 - 9));
    CHECK(rd(SPU_REG(0x1AE)) & 0x800, "STATX.11 in the second half (:650)");
}

/* ---- sweeps (A4, A11) ------------------------------------------------------- */

static int sweep_run(uint16_t reg, int level, int samples) {
    int32_t counter = 0;
    for (int i = 0; i < samples; i++) level = spu_sweep_tick(reg, level, &counter);
    return level;
}

static void test_sweep_algorithm(void) {
    /* Linear increase saturates at +7FFFh (:431, :477). */
    CHECK(sweep_run(0x8000, 0, 10) == 0x7FFF, "linear increase to 7FFFh");
    /* Linear decrease stops at 0 (:431-432, :480-481). */
    CHECK(sweep_run(0xA000, 0x7FFE, 10) == 0, "linear decrease stops at 0, not below: %d",
          sweep_run(0xA000, 0x7FFE, 10));
    CHECK(sweep_run(0xA000, 0x100, 1) == 0, "one step from 100h lands on 0");
    /* Exponential decrease decays and reaches 0 (:463-464). */
    int e = sweep_run(0xE000, 0x7FFF, 4000);
    CHECK(e >= 0 && e < 0x100, "exponential decrease decays: %d", e);
    /* Phase negative + decreasing from a positive level snaps to 0 (:478-479, :498-500). */
    CHECK(sweep_run(0xB000, 0x4000, 1) == 0, "decrease with negative phase clamps to 0..-8000h");
    /* Phase negative + increasing: the step is negative and it runs through 0 to -8000h (:498-499). */
    int p = sweep_run(0x9000, 0x4000, 10);
    CHECK(p == -0x8000, "increase with negative phase goes to -8000h: %d", p);
    /* Rate: shift 13, step 0: +7 every 4 samples (:453-454, :493-494). */
    CHECK(sweep_run((uint16_t)(0x8000 | (13 << 2)), 0, 4) == 7, "shift 13 steps every 4th sample");
    CHECK(sweep_run((uint16_t)(0x8000 | (13 << 2)), 0, 3) == 0, "and not before");
    /* Exponential increase above 6000h with shift < 10: step / 4 (:455-457). */
    CHECK(sweep_run(0xC000, 0x6001, 1) == 0x6001 + (7 << 11) / 4, "exp increase above 6000h quarters the step");
    CHECK(sweep_run(0xC000, 0x1000, 1) == 0x1000 + (7 << 11), "exp increase below 6000h: full step");
    /* All-ones step and shift never step and never saturate (:466-467, :490-491). */
    CHECK(sweep_run(0x807F, 0x1234, 1000) == 0x1234, "all-ones never steps");
}

static void test_main_volume_sweep(void) {
    reset_all();
    wr(SPU_REG(0x1AA), 0xC000);
    wr(SPU_REG(0x180), 0x0000);               /* fixed 0 */
    wr(SPU_REG(0x182), 0x0000);
    one_sample(NULL, NULL);
    CHECK(rd(SPU_REG(0x1B8)) == 0, "MVOLXL 0 after a fixed 0");
    wr(SPU_REG(0x180), 0x8000);               /* sweep: linear, increase, fastest (:414-422) */
    wr(SPU_REG(0x182), 0x8000);
    for (int i = 0; i < 8; i++) one_sample(NULL, NULL);
    CHECK(rd(SPU_REG(0x1B8)) == 0x7FFF && rd(SPU_REG(0x1BA)) == 0x7FFF,
          "main volume sweeps up and MVOLX reads it (:526-531): %04x", rd(SPU_REG(0x1B8)));
    wr(SPU_REG(0x180), 0xA000);               /* linear decrease */
    for (int i = 0; i < 8; i++) one_sample(NULL, NULL);
    CHECK(rd(SPU_REG(0x1B8)) == 0, "main volume decreases to 0 and stays: %04x", rd(SPU_REG(0x1B8)));
    CHECK(rd(SPU_REG(0x1BA)) == 0x7FFF, "right channel untouched");
    CHECK(rd(SPU_REG(0x180)) == 0xA000, "MVOLL reads back the register");
}

/* ---- manual-write FIFO (A9) -------------------------------------------------- */

static void test_manual_fifo(void) {
    reset_all();
    wr(SPU_REG(0x1AA), 0x8000);               /* Stop (:717) */
    wr(SPU_REG(0x1A6), 0x3000 >> 3);          /* address (:718) */
    for (int i = 0; i < 4; i++) wr(SPU_REG(0x1A8), (uint16_t)(0xA000 + i));   /* FIFO (:719) */
    CHECK(spu()->ram[0x3000 / 2] == 0, "nothing in RAM while the mode is Stop");
    CHECK(spu()->manual_fifo_count == 4, "four halfwords queued");
    wr(SPU_REG(0x1AA), 0x8010);               /* Manual Write (:720) */
    for (int i = 0; i < 4; i++)
        CHECK(spu()->ram[0x3000 / 2 + i] == 0xA000 + i, "FIFO entry %d landed", i);
    CHECK(spu()->manual_fifo_count == 0, "FIFO drained");
    CHECK(spu()->transfer_addr == 0x3008, "transfer address advanced: %05x", spu()->transfer_addr);
    /* At most 32 halfwords (:683). */
    wr(SPU_REG(0x1AA), 0x8000);
    wr(SPU_REG(0x1A6), 0x4000 >> 3);
    for (int i = 0; i < 40; i++) wr(SPU_REG(0x1A8), (uint16_t)(0xB000 + i));
    CHECK(spu()->manual_fifo_count == 32, "FIFO holds 32");
    wr(SPU_REG(0x1AA), 0x8010);
    CHECK(spu()->ram[0x4000 / 2 + 31] == 0xB000 + 31 && spu()->ram[0x4000 / 2 + 32] == 0,
          "32 halfwords written, the rest dropped");
    /* Manual Write already selected: writes go straight in (multi-block, :722-727). */
    wr(SPU_REG(0x1A6), 0x5000 >> 3);
    wr(SPU_REG(0x1A8), 0xC001);
    CHECK(spu()->ram[0x5000 / 2] == 0xC001, "write in Manual Write mode lands at once");
}

/* ---- mixing: SPUCNT.14, CD volume (A7, A10) ------------------------------- */

/* Output of one sample with a loud voice 0 and a CD frame of `cd`. */
static int16_t mix_one(uint16_t spucnt, int16_t cd, int16_t cd_vol) {
    reset_all();
    put_block(0x1000, 0x07, 0x77);       /* loud, self-looping */
    voice_cfg(0, 0x1000);
    wr(SPU_REG(0x180), 0x3FFF);
    wr(SPU_REG(0x182), 0x3FFF);
    wr(SPU_REG(0x1B0), (uint16_t)cd_vol);
    wr(SPU_REG(0x1B2), (uint16_t)cd_vol);
    wr(SPU_REG(0x1AA), spucnt);
    key_on(0);
    for (int i = 0; i < 200; i++) {
        cdrom_audio_fifo_push(&g_inter.cdrom.audio_fifo, cd, cd);
        spu_step(&g_inter, CPU_TICKS_PER_SPU_TICK);
    }
    int16_t buf[2 * 256];
    int n = spu_get_samples(spu(), buf, 256);
    return n > 0 ? buf[(n - 1) * 2] : 0;
}

static void test_mute_bit14_spares_cd(void) {
    int16_t voice_only = mix_one(0xC000, 0, 0x7FFF);                 /* unmuted, no CD */
    CHECK(voice_only != 0, "voice audible when unmuted");
    int16_t muted_voice = mix_one(0x8001, 0, 0x7FFF);                /* bit14 = 0, CD on */
    CHECK(muted_voice == 0, "SPUCNT.14 = 0 silences the voice: %d", muted_voice);
    int16_t muted_cd = mix_one(0x8001, 8000, 0x7FFF);
    int16_t expect = (int16_t)((((8000 * 0x7FFF) >> 15) * 0x7FFE) >> 15);
    CHECK(muted_cd == expect, "and the CD still plays (:631 Don't care for CD Audio): %d want %d",
          muted_cd, expect);
}

static void test_cd_volume_zero_is_silence(void) {
    int16_t v0 = mix_one(0x8001, 8000, 0);
    CHECK(v0 == 0, "AVOL 0 is silence, not full volume (:442): %d", v0);
    int16_t vneg = mix_one(0x8001, 8000, (int16_t)0x8000);
    CHECK(vneg < 0, "AVOL is signed: -8000h inverts (%d)", vneg);
}

static void test_cd_matrix(void) {
    int16_t l = 1000, r = -3000;
    cdrom_audio_apply_output(&l, &r, false, 0x80, 0, 0, 0x80);
    CHECK(l == 1000 && r == -3000, "80h,0,0,80h is unity (cdromdrive.md:231-232)");
    l = 1000; r = 3000;
    cdrom_audio_apply_output(&l, &r, false, 0x40, 0x40, 0x40, 0x40);
    CHECK(l == 2000 && r == 2000, "40h x4 is mono (:232-233): %d %d", l, r);
    l = 30000; r = 30000;
    cdrom_audio_apply_output(&l, &r, false, 0x80, 0x80, 0x80, 0x80);
    CHECK(l == 32767 && r == 32767, "double volume saturates (:234-236)");
    l = 1000; r = 1000;
    cdrom_audio_apply_output(&l, &r, true, 0x80, 0, 0, 0x80);
    CHECK(l == 0 && r == 0, "mute forces 0 (:1020-1022)");
}

/* ---- F16: arithmetic identical to the code it replaces ---------------------- */

static uint32_t rng_state = 12345u;
static uint32_t rng(void) { rng_state = rng_state * 1664525u + 1013904223u; return rng_state >> 8; }

static void test_reverb_wrap_bit_exact(void) {
    reset_all();
    for (int i = 0; i < 200000; i++) {
        spu()->reverb_base = (uint16_t)rng();
        int32_t base = (int32_t)spu()->reverb_base * 4;
        int32_t end = SPU_RAM_SIZE / 2;
        spu()->reverb_current_addr = (uint32_t)(base + (int32_t)(rng() % (uint32_t)(end - base)));
        int32_t off = (int32_t)(rng() % 0xA0000u) - 0x50000;
        int32_t span = end - base;
        int32_t a = ((int32_t)spu()->reverb_current_addr + off - base) % span;
        if (a < 0) a += span;
        uint32_t ref = (uint32_t)(base + a);
        if (rev_addr(spu(), off) != ref) {
            CHECK(0, "rev_addr base=%d cur=%u off=%d: %u != %u", base,
                  spu()->reverb_current_addr, off, rev_addr(spu(), off), ref);
            break;
        }
    }
    g_checks++;
}

static void test_fir_bit_exact(void) {
    for (int k = 1; k < 39; k += 2)
        if (k != 19) CHECK(s_rev_fir[k] == 0, "odd tap %d is zero", k);
    int16_t ring[128];
    for (int t = 0; t < 20000; t++) {
        for (int i = 0; i < 64; i++) ring[i] = ring[i + 64] = (int16_t)rng();
        int pos = (int)(rng() & 0x3F) | 1;
        int base = (pos - 38) & 0x3F;
        int64_t acc = 0;
        for (int k = 0; k < 39; k++) acc += (int64_t)s_rev_fir[k] * ring[base + k];
        int32_t ref = clamp16((int32_t)(acc >> 15));
        if (rev_fir_down(ring, pos) != ref) { CHECK(0, "FIR mismatch at trial %d", t); break; }
    }
    g_checks++;
}

static void test_ring_target_default(void) {
    CHECK(SPU_RING_TARGET_SAMPLES == 2048, "ring target default 2048 without the env var: %d",
          SPU_RING_TARGET_SAMPLES);
    CHECK(SPU_RING_TARGET_MAX + 882 < SPU_SAMPLE_BUFFER_SIZE, "max target + one field fits the ring");
}

int main(void) {
    unsetenv("ZS1_SPU_RING_TARGET");
    test_loop_code3_with_loop_start();
    test_loop_code0_continues();
    test_kon_keeps_preset_lsax();
    test_code1_end_mute();
    test_lsax_write_then_loop_start();
    test_irq_at_loop_start_every_pass();
    test_irq_not_early_and_mid_block();
    test_no_irq_on_register_write();
    test_flag_ack_via_spucnt();
    test_silent_voice_irq();
    test_capture_into_ram();
    test_sweep_algorithm();
    test_main_volume_sweep();
    test_manual_fifo();
    test_mute_bit14_spares_cd();
    test_cd_volume_zero_is_silence();
    test_cd_matrix();
    test_reverb_wrap_bit_exact();
    test_fir_bit_exact();
    test_ring_target_default();
    printf("spu_test: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
