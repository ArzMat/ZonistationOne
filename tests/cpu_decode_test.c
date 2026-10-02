/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 ZioZoni95
 * SPDX-FileCopyrightText: The PCSX-Redux authors
 *
 * Part of ZoniStation One, a PlayStation 1 emulator.
 * See LICENSE for the full licence text and THIRD-PARTY.md for the
 * components of this project that have other authors.
 */
/* src/cpu/cpu_decode.c against the dispatch tables it replaced.
 *
 * decode_and_execute() used to be two tables of function pointers (pcsx-redux's
 * s_psxBSC / s_psxSPC layout) with NULL meaning op_illegal. It is a switch now,
 * and the only acceptable difference is speed. The old tables are kept here
 * verbatim, every handler is a stub that records its own name, and every
 * primary opcode and every SPECIAL function is decoded both ways, each with a
 * few different patterns in the other 26 bits so a switch that accidentally
 * looked at the wrong field would show up. */
#include "log.h"
#include "cpu.h"

#include <stdio.h>
#include <string.h>

LogLevel current_log_level = LOG_LEVEL_INFO;
void log_print(LogCategory category, LogLevel level, const char* format, ...) {
    (void)category; (void)level; (void)format;
}

static const char* g_hit;
static int         g_hits;

#define STUB(name) void name(Cpu* cpu, uint32_t instruction) { \
    (void)cpu; (void)instruction; g_hit = #name; g_hits++; }

STUB(op_lui) STUB(op_ori) STUB(op_sw) STUB(op_sll) STUB(op_addiu) STUB(op_j)
STUB(op_or) STUB(op_cop0) STUB(op_mtc0) STUB(op_rfe) STUB(op_bne) STUB(op_addi)
STUB(op_lw) STUB(op_sltu) STUB(op_addu) STUB(op_sh) STUB(op_jal) STUB(op_andi)
STUB(op_sb) STUB(op_jr) STUB(op_lb) STUB(op_beq) STUB(op_mfc0) STUB(op_and)
STUB(op_add) STUB(op_bgtz) STUB(op_blez) STUB(op_lbu) STUB(op_jalr) STUB(op_bxx)
STUB(op_slti) STUB(op_subu) STUB(op_sra) STUB(op_div) STUB(op_divu) STUB(op_mflo)
STUB(op_srl) STUB(op_sltiu) STUB(op_slt) STUB(op_mfhi) STUB(op_syscall) STUB(op_nor)
STUB(op_mtlo) STUB(op_mthi) STUB(op_lhu) STUB(op_lh) STUB(op_sllv) STUB(op_srav)
STUB(op_srlv) STUB(op_multu) STUB(op_xor) STUB(op_break) STUB(op_mult) STUB(op_sub)
STUB(op_xori) STUB(op_cop1) STUB(op_cop2) STUB(op_cop3) STUB(op_lwl) STUB(op_lwr)
STUB(op_swl) STUB(op_swr) STUB(op_lwc0) STUB(op_lwc1) STUB(op_lwc2) STUB(op_lwc3)
STUB(op_swc0) STUB(op_swc1) STUB(op_swc2) STUB(op_swc3) STUB(op_illegal)

#include "../src/cpu/cpu_decode.c"

/* --- The reference: the tables as they were before the switch --- */
static const cpu_handler_t ref_special[64] = {
    [0x00] = op_sll,  [0x02] = op_srl,   [0x03] = op_sra,     [0x04] = op_sllv,
    [0x06] = op_srlv, [0x07] = op_srav,  [0x08] = op_jr,      [0x09] = op_jalr,
    [0x0C] = op_syscall, [0x0D] = op_break,
    [0x10] = op_mfhi, [0x11] = op_mthi,  [0x12] = op_mflo,    [0x13] = op_mtlo,
    [0x18] = op_mult, [0x19] = op_multu, [0x1A] = op_div,     [0x1B] = op_divu,
    [0x20] = op_add,  [0x21] = op_addu,  [0x22] = op_sub,     [0x23] = op_subu,
    [0x24] = op_and,  [0x25] = op_or,    [0x26] = op_xor,     [0x27] = op_nor,
    [0x2A] = op_slt,  [0x2B] = op_sltu,
};

static void ref_op_special(Cpu* cpu, uint32_t instruction) {
    cpu_handler_t h = ref_special[instruction & 0x3F];
    if (h) h(cpu, instruction); else op_illegal(cpu, instruction);
}

static const cpu_handler_t ref_op[64] = {
    [0x00] = ref_op_special, [0x01] = op_bxx,  [0x02] = op_j,     [0x03] = op_jal,
    [0x04] = op_beq,   [0x05] = op_bne,   [0x06] = op_blez,  [0x07] = op_bgtz,
    [0x08] = op_addi,  [0x09] = op_addiu, [0x0A] = op_slti,  [0x0B] = op_sltiu,
    [0x0C] = op_andi,  [0x0D] = op_ori,   [0x0E] = op_xori,  [0x0F] = op_lui,
    [0x10] = op_cop0,  [0x11] = op_cop1,  [0x12] = op_cop2,  [0x13] = op_cop3,
    [0x20] = op_lb,    [0x21] = op_lh,    [0x22] = op_lwl,   [0x23] = op_lw,
    [0x24] = op_lbu,   [0x25] = op_lhu,   [0x26] = op_lwr,
    [0x28] = op_sb,    [0x29] = op_sh,    [0x2A] = op_swl,   [0x2B] = op_sw,
    [0x2E] = op_swr,
    [0x30] = op_lwc0,  [0x31] = op_lwc1,  [0x32] = op_lwc2,  [0x33] = op_lwc3,
    [0x38] = op_swc0,  [0x39] = op_swc1,  [0x3A] = op_swc2,  [0x3B] = op_swc3,
};

static void ref_decode(Cpu* cpu, uint32_t instruction) {
    cpu_handler_t h = ref_op[instruction >> 26];
    if (h) h(cpu, instruction); else op_illegal(cpu, instruction);
}

static Cpu g_cpu;

int main(void) {
    /* Patterns for the bits that are not the field being decoded. */
    static const uint32_t fill[] = { 0x00000000u, 0x03FFFFFFu, 0x0155AA55u, 0x02AA55AAu, 0x01234567u };
    const int nfill = (int)(sizeof(fill) / sizeof(fill[0]));
    int checks = 0, failures = 0;

    for (uint32_t op = 0; op < 64; op++) {
        for (int f = 0; f < nfill; f++) {
            /* SPECIAL: walk all 64 functions; everything else: the opcode with
             * arbitrary low bits. */
            uint32_t nfunct = (op == 0) ? 64 : 1;
            for (uint32_t fn = 0; fn < nfunct; fn++) {
                uint32_t instr = (op << 26) | (fill[f] & 0x03FFFFFFu);
                if (op == 0) instr = (instr & ~0x3Fu) | fn;

                g_hit = NULL; g_hits = 0;
                ref_decode(&g_cpu, instr);
                const char* want = g_hit; int want_n = g_hits;

                g_hit = NULL; g_hits = 0;
                decode_and_execute(&g_cpu, instr);
                const char* got = g_hit; int got_n = g_hits;

                checks++;
                if (!want || !got || strcmp(want, got) != 0 || want_n != 1 || got_n != 1) {
                    failures++;
                    printf("FAIL instr %08X: reference %s (x%d), switch %s (x%d)\n",
                           instr, want ? want : "(none)", want_n, got ? got : "(none)", got_n);
                }
            }
        }
    }

    printf("cpu_decode_test: %d decodes compared, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
