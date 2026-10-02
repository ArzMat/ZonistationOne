/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 ZioZoni95
 * SPDX-FileCopyrightText: The PCSX-Redux authors
 *
 * Part of ZoniStation One, a PlayStation 1 emulator.
 * See LICENSE for the full licence text and THIRD-PARTY.md for the
 * components of this project that have other authors.
 */
#include "cpu.h"

/* Instruction dispatch.
 *
 * This was two tables of function pointers: s_op_table on the primary opcode
 * (bits 31:26, pcsx-redux's s_psxBSC layout), and for SPECIAL a second call
 * through op_special() into s_special_table on bits 5:0 (s_psxSPC). The most
 * common instructions (ADDU, OR, SLL, JR, ...) are SPECIAL, so they paid two
 * indirect calls, and no handler could be inlined into the dispatcher.
 *
 * Now it is one switch per level, which the compiler turns into a jump table
 * with direct calls the optimiser can see through. The mapping is exactly the
 * old tables', slot for slot: every case below is a slot that table filled,
 * and every slot it left NULL falls to default -> op_illegal(), as the NULL
 * check did. tests/cpu_decode_test.c keeps the old tables and checks all 64
 * primary opcodes and all 64 SPECIAL functions against this function. */

static inline void op_special(Cpu* cpu, uint32_t instruction) {
    switch (instruction & 0x3F) {
        case 0x00: op_sll(cpu, instruction);     break;
        case 0x02: op_srl(cpu, instruction);     break;
        case 0x03: op_sra(cpu, instruction);     break;
        case 0x04: op_sllv(cpu, instruction);    break;
        case 0x06: op_srlv(cpu, instruction);    break;
        case 0x07: op_srav(cpu, instruction);    break;
        case 0x08: op_jr(cpu, instruction);      break;
        case 0x09: op_jalr(cpu, instruction);    break;
        case 0x0C: op_syscall(cpu, instruction); break;
        case 0x0D: op_break(cpu, instruction);   break;
        case 0x10: op_mfhi(cpu, instruction);    break;
        case 0x11: op_mthi(cpu, instruction);    break;
        case 0x12: op_mflo(cpu, instruction);    break;
        case 0x13: op_mtlo(cpu, instruction);    break;
        case 0x18: op_mult(cpu, instruction);    break;
        case 0x19: op_multu(cpu, instruction);   break;
        case 0x1A: op_div(cpu, instruction);     break;
        case 0x1B: op_divu(cpu, instruction);    break;
        case 0x20: op_add(cpu, instruction);     break;
        case 0x21: op_addu(cpu, instruction);    break;
        case 0x22: op_sub(cpu, instruction);     break;
        case 0x23: op_subu(cpu, instruction);    break;
        case 0x24: op_and(cpu, instruction);     break;
        case 0x25: op_or(cpu, instruction);      break;
        case 0x26: op_xor(cpu, instruction);     break;
        case 0x27: op_nor(cpu, instruction);     break;
        case 0x2A: op_slt(cpu, instruction);     break;
        case 0x2B: op_sltu(cpu, instruction);    break;
        default:   op_illegal(cpu, instruction); break;
    }
}

void decode_and_execute(Cpu* cpu, uint32_t instruction) {
    switch (instruction >> 26) {
        case 0x00: op_special(cpu, instruction); break;  /* SPECIAL, on bits 5:0 */
        case 0x01: op_bxx(cpu, instruction);     break;  /* REGIMM (BGEZ/BLTZ/BGEZAL/BLTZAL) */
        case 0x02: op_j(cpu, instruction);       break;
        case 0x03: op_jal(cpu, instruction);     break;
        case 0x04: op_beq(cpu, instruction);     break;
        case 0x05: op_bne(cpu, instruction);     break;
        case 0x06: op_blez(cpu, instruction);    break;
        case 0x07: op_bgtz(cpu, instruction);    break;
        case 0x08: op_addi(cpu, instruction);    break;
        case 0x09: op_addiu(cpu, instruction);   break;
        case 0x0A: op_slti(cpu, instruction);    break;
        case 0x0B: op_sltiu(cpu, instruction);   break;
        case 0x0C: op_andi(cpu, instruction);    break;
        case 0x0D: op_ori(cpu, instruction);     break;
        case 0x0E: op_xori(cpu, instruction);    break;
        case 0x0F: op_lui(cpu, instruction);     break;
        case 0x10: op_cop0(cpu, instruction);    break;
        case 0x11: op_cop1(cpu, instruction);    break;  /* COP1 -> exception (FPU absent) */
        case 0x12: op_cop2(cpu, instruction);    break;  /* COP2 = GTE */
        case 0x13: op_cop3(cpu, instruction);    break;  /* COP3 -> exception */
        case 0x20: op_lb(cpu, instruction);      break;
        case 0x21: op_lh(cpu, instruction);      break;
        case 0x22: op_lwl(cpu, instruction);     break;
        case 0x23: op_lw(cpu, instruction);      break;
        case 0x24: op_lbu(cpu, instruction);     break;
        case 0x25: op_lhu(cpu, instruction);     break;
        case 0x26: op_lwr(cpu, instruction);     break;
        case 0x28: op_sb(cpu, instruction);      break;
        case 0x29: op_sh(cpu, instruction);      break;
        case 0x2A: op_swl(cpu, instruction);     break;
        case 0x2B: op_sw(cpu, instruction);      break;
        case 0x2E: op_swr(cpu, instruction);     break;
        case 0x30: op_lwc0(cpu, instruction);    break;
        case 0x31: op_lwc1(cpu, instruction);    break;
        case 0x32: op_lwc2(cpu, instruction);    break;  /* LWC2 = GTE load */
        case 0x33: op_lwc3(cpu, instruction);    break;
        case 0x38: op_swc0(cpu, instruction);    break;
        case 0x39: op_swc1(cpu, instruction);    break;
        case 0x3A: op_swc2(cpu, instruction);    break;  /* SWC2 = GTE store */
        case 0x3B: op_swc3(cpu, instruction);    break;
        default:   op_illegal(cpu, instruction); break;
    }
}
