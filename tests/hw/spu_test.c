/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * SPU behaviour that streaming audio depends on: the voice IRQ at the start of a
 * looping buffer, a Loop End jumping to a repeat address set before Key On, the
 * documented manual-write sequence, and the main volume sweep.
 */
#include "hw.h"

#define SPU_VOICE(n, r) REG16(0xBF801C00u + (n) * 0x10u + (r))
#define SPU_MVOL_L  REG16(0xBF801D80)
#define SPU_MVOL_R  REG16(0xBF801D82)
#define SPU_KON_LO  REG16(0xBF801D88)
#define SPU_KOFF_LO REG16(0xBF801D8C)
#define SPU_ENDX_LO REG16(0xBF801D9C)
#define SPU_IRQA    REG16(0xBF801DA4)
#define SPU_TSA     REG16(0xBF801DA6)
#define SPU_FIFO    REG16(0xBF801DA8)
#define SPU_CNT     REG16(0xBF801DAA)
#define SPU_RAMCTL  REG16(0xBF801DAC)
#define SPU_STAT    REG16(0xBF801DAE)
#define SPU_MVOLX_L REG16(0xBF801DB8)

#define V_VOL_L 0x0
#define V_VOL_R 0x2
#define V_PITCH 0x4
#define V_START 0x6
#define V_ADSR1 0x8
#define V_ADSR2 0xA
#define V_ENVX  0xC
#define V_LSAX  0xE

#define CNT_ENABLE 0x8000u
#define CNT_IRQ9   0x0040u
#define CNT_MODE_MASK 0x0030u
#define CNT_MODE_MANUAL 0x0010u

/* One ADPCM block: header (shift/filter, flags) and 14 bytes of nibbles. */
static void make_block(uint16_t* hw, uint8_t flags) {
    hw[0] = (uint16_t)(0x00u | ((uint16_t)flags << 8));
    for (int i = 1; i < 8; i++) hw[i] = 0x7171u;
}

static void wait_mode(uint16_t mode) {
    uint32_t n = POLL_LIMIT;
    while ((SPU_STAT & 0x3Fu) != mode && --n) {}
}

static void wait_not_busy(void) {
    uint32_t n = POLL_LIMIT;
    while ((SPU_STAT & 0x0400u) && --n) {}
}

/* psx-spx spu/soundprocessingunitspu.md:715-720: Stop, address, FIFO, Manual. */
static void upload_documented(uint32_t addr, const uint16_t* hw, uint32_t count) {
    uint16_t base = (uint16_t)(SPU_CNT & ~CNT_MODE_MASK);
    for (uint32_t done = 0; done < count; done += 32) {
        SPU_CNT = base;
        wait_mode(base & 0x3Fu);
        SPU_TSA = (uint16_t)((addr + done * 2) >> 3);
        for (uint32_t i = done; i < count && i < done + 32; i++) SPU_FIFO = hw[i];
        SPU_CNT = base | CNT_MODE_MANUAL;
        wait_mode((base | CNT_MODE_MANUAL) & 0x3Fu);
        wait_not_busy();
    }
    SPU_CNT = base;
}

/* Address first, then the FIFO in Manual Write mode: what the emulator has
 * always accepted. Used for the data of the checks that are not about the
 * transfer itself, so one defect does not mask another. */
static void upload_direct(uint32_t addr, const uint16_t* hw, uint32_t count) {
    uint16_t base = (uint16_t)(SPU_CNT & ~CNT_MODE_MASK);
    SPU_CNT = base | CNT_MODE_MANUAL;
    wait_mode((base | CNT_MODE_MANUAL) & 0x3Fu);
    SPU_TSA = (uint16_t)(addr >> 3);
    for (uint32_t i = 0; i < count; i++) SPU_FIFO = hw[i];
    wait_not_busy();
    SPU_CNT = base;
}

static void voice_setup(int v, uint32_t start) {
    SPU_VOICE(v, V_VOL_L) = 0x3FFF;
    SPU_VOICE(v, V_VOL_R) = 0x3FFF;
    SPU_VOICE(v, V_PITCH) = 0x1000;                /* 44.1 kHz: one block per 28 samples */
    SPU_VOICE(v, V_START) = (uint16_t)(start >> 3);
    SPU_VOICE(v, V_ADSR1) = 0x000F;                /* fastest attack, sustain level max */
    SPU_VOICE(v, V_ADSR2) = 0x0000;
}

static uint16_t g_hw[3 * 8];

/* Blocks 0x1000 (plain), 0x1010 (Loop Start), 0x1020 (Loop End + Repeat):
 * the voice plays 0,1,2,1,2,... IRQA on 0x1010 must fire every time block 1
 * is read (soundprocessingunitspu.md:824), not once. */
static void test_irq_at_loop_start(void) {
    make_block(&g_hw[0], 0x00);
    make_block(&g_hw[8], 0x04);
    make_block(&g_hw[16], 0x03);
    upload_direct(0x1000, g_hw, 24);
    voice_setup(0, 0x1000);
    SPU_IRQA = 0x1010 >> 3;
    SPU_CNT = CNT_ENABLE | CNT_IRQ9;
    SPU_KON_LO = 0x0001;

    uint32_t irqs = 0;
    for (uint32_t t = 0; t < 400000 && irqs < 6; t++) {
        if (SPU_STAT & 0x0040u) {
            irqs++;
            SPU_CNT = CNT_ENABLE;                      /* acknowledge: ATTR.6 = 0 */
            I_STAT = ~0x0200u;                         /* and the CPU-side bit */
            SPU_CNT = CNT_ENABLE | CNT_IRQ9;
        }
    }
    SPU_KOFF_LO = 0x0001;
    SPU_CNT = CNT_ENABLE;
    check("irq_every_loop_at_loop_start", irqs >= 6, irqs, 6);
}

/* Blocks 0x2000 (plain), 0x2010 (Loop End + Repeat), no Loop Start anywhere,
 * repeat address written before Key On: the voice loops to 0x2000 forever
 * (soundprocessingunitspu.md:133-138, :173) and keeps its envelope. */
static void test_loop_end_uses_preset_repeat_address(void) {
    make_block(&g_hw[0], 0x00);
    make_block(&g_hw[8], 0x03);
    upload_direct(0x2000, g_hw, 16);
    voice_setup(1, 0x2000);
    SPU_VOICE(1, V_LSAX) = 0x2000 >> 3;
    SPU_KON_LO = 0x0002;
    spin(300000);
    uint16_t envx = SPU_VOICE(1, V_ENVX);
    uint16_t endx = SPU_ENDX_LO;
    SPU_KOFF_LO = 0x0002;
    check("loop_end_jumps_to_preset_lsax", envx != 0 && (endx & 0x0002u), envx, 0x7FFF);
}

/* One block with Loop End at 0x3000, written with the documented sequence.
 * If the FIFO writes made in Stop mode are lost, the voice reads zeroes and
 * never reaches a Loop End, so ENDX stays clear. */
static void test_manual_write_sequence(void) {
    make_block(&g_hw[0], 0x01);
    upload_documented(0x3000, g_hw, 8);
    voice_setup(2, 0x3000);
    SPU_KON_LO = 0x0004;
    spin(100000);
    uint16_t endx = SPU_ENDX_LO;
    check("documented_manual_write_lands", (endx & 0x0004u) != 0, endx, 0x0004);
}

/* Main volume in sweep mode (soundprocessingunitspu.md:405-432): from a fixed
 * 0, a fast linear increase must raise the current level read from MVOLXL. */
static void test_main_volume_sweep(void) {
    SPU_MVOL_L = 0x0000;
    SPU_MVOL_R = 0x0000;
    spin(20000);
    SPU_MVOL_L = 0x8000;                           /* sweep, linear, increase, fastest */
    SPU_MVOL_R = 0x8000;
    spin(200000);
    uint16_t level = SPU_MVOLX_L;
    check("main_volume_sweep_moves", level >= 0x1000 && level < 0x8000, level, 0x7FFF);
    SPU_MVOL_L = 0x3FFF;
    SPU_MVOL_R = 0x3FFF;
}

int main(void) {
    suite_begin("spu");
    SPU_CNT = 0;
    SPU_RAMCTL = 0x0004;
    SPU_CNT = CNT_ENABLE;
    SPU_MVOL_L = 0x3FFF;
    SPU_MVOL_R = 0x3FFF;
    test_irq_at_loop_start();
    test_loop_end_uses_preset_repeat_address();
    test_manual_write_sequence();
    test_main_volume_sweep();
    suite_end();
    return 0;
}
