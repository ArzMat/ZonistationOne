/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 ZioZoni95
 *
 * Part of ZoniStation One, a PlayStation 1 emulator.
 * See LICENSE for the full licence text and THIRD-PARTY.md for the
 * components of this project that have other authors.
 */
#include "cpu.h"
#include <string.h>
#include <stdio.h>
#include "interconnect.h"
#include "event_scheduler.h"
#include "gte.h"
#include "debugger.h"
#include "log.h"

// ============================================================================
// CPU Execution Loop
// ============================================================================

// Check for pending hardware interrupt - called once per instruction
static inline bool CheckPendingInterrupt(Cpu* cpu) {
    // Per PSX-SPX: interrupt pending if (I_STAT & I_MASK) != 0 AND SR.IE == 1
    uint16_t i_stat = cpu->inter->irq_status;
    uint16_t i_mask = cpu->inter->irq_mask;
    const uint32_t ip2 = ((i_stat & i_mask) != 0) ? (1u << 10) : 0u;

    // Update COP0 Cause bit 10 (IP2) based on current state - NOT a latch!
    // Written only when it changes: the old unconditional set-or-clear was a
    // read-modify-write of cpu->cause on every instruction for a bit that
    // changes a few hundred times a field. Same value either way.
    if ((cpu->cause ^ ip2) & (1u << 10)) cpu->cause ^= (1u << 10);

    // Check: SR.IEc && ((cause & sr) & 0xFF00) != 0
    bool sr_iec = (cpu->sr & 1) != 0;
    uint32_t sr_cause_masked = (cpu->sr & cpu->cause) & 0xFF00;
    bool has_interrupt = sr_iec && (sr_cause_masked != 0);
    
    if (has_interrupt) {
        /* PSX-SPX: do not take IRQ if the next instruction is a GTE opcode (COP2 data op).
           Defer until the GTE instruction completes to avoid EPC pointing into a GTE op. */
        uint32_t next_instr = cpu_icache_fetch(cpu, cpu->current_pc, false);
        if ((next_instr & 0xFE000000) == 0x4A000000) return false;
        cpu_exception(cpu, EXCEPTION_INTERRUPT);
        return true;
    }
    return false;
}

/* Advance the load-delay pipeline by one instruction.
 *
 * Called once the instruction has run, which is the whole point: "the target
 * register isn't updated until the next opcode has completed"
 * (psx-spx-docs/docs/cpuspecifications.md:172-174). The load issued by the
 * previous instruction lands here, and the one this instruction issued (if any)
 * takes its place, to land after the next one.
 *
 * Writing cpu->regs directly rather than through cpu_set_reg: that function
 * cancels a delayed load aimed at the register it writes, which is right for an
 * instruction's own write and wrong for the load's own retirement. A load
 * targeting R0 is a discard, and delay_load_reg is REG_ZERO when nothing is in
 * flight, so the same test covers both. */
static inline void cpu_retire_load_delay(Cpu* cpu) {
    if (cpu->delay_load_reg != REG_ZERO) {
        cpu->regs[cpu->delay_load_reg] = cpu->delay_load_value;
    }
    cpu->delay_load_reg   = cpu->load_reg_idx;
    cpu->delay_load_value = cpu->load_value;
    cpu->load_reg_idx     = REG_ZERO;
}

/* Land an in-flight load on the way into an exception handler.
 *
 * "unless an IRQ occurs between the load and next opcode, in that case the load
 * would complete during IRQ handling, and so, the next opcode would receive the
 * NEW value" (:175-177). The handler is that next opcode, so the delay is spent
 * by the time it runs. Both slots are drained: the instruction that was about to
 * execute never does, so nothing is left to retire it later. */
void cpu_flush_load_delay(Cpu* cpu) {
    if (cpu->delay_load_reg != REG_ZERO) {
        cpu->regs[cpu->delay_load_reg] = cpu->delay_load_value;
        cpu->delay_load_reg = REG_ZERO;
    }
    if (cpu->load_reg_idx != REG_ZERO) {
        cpu->regs[cpu->load_reg_idx] = cpu->load_value;
        cpu->load_reg_idx = REG_ZERO;
    }
}

/* One instruction. The body of cpu_run_next_instruction(), kept inline so the
 * frame loop below runs it without a call per instruction (system.c used to
 * call cpu_run_next_instruction() across a translation unit for every one). */
static inline __attribute__((always_inline)) void cpu_step(Cpu* cpu) {
    // exception_pending is per-instruction state; clear it before running this step.
    cpu->exception_pending = false;

    /* --- 1. (was: commit the pending load here, before executing) ---
     *
     * Moved to cpu_retire_load_delay(), after the instruction executes. Landing
     * it here made the loaded value visible to the very next opcode, which is
     * precisely what the R3000A does not do: "The loaded data is NOT available
     * to the next opcode, ie. the target register isn't updated until the next
     * opcode has completed" (psx-spx-docs/docs/cpuspecifications.md:172-174).
     * The delay slot was therefore not modelled at all, and the LWL/LWR code
     * that merges with a load still in flight could never fire, because this
     * cleared the slot before any following instruction could see it. */

    // Establish current-instruction context before any potential interrupt exception.
    // This matches R3000A behavior where IRQ is taken between instructions, and BD/EPC
    // are derived from the instruction about to execute.
    cpu->current_pc = cpu->pc;
    cpu->in_delay_slot = cpu->branch_taken;

    // --- 2. Check for pending interrupt ---
    if (CheckPendingInterrupt(cpu)) {
        return; // Exception raised, PC already updated
    }

    // --- 3. Fetch Instruction ---
    
    // Check PC alignment
    if (cpu->current_pc % 4 != 0) {
        cpu_exception(cpu, EXCEPTION_LOAD_ADDRESS_ERROR);
        return;
    }
    
    uint32_t instruction = cpu_icache_fetch(cpu, cpu->current_pc, true);

    // Record into execution trace ring buffer (frozen after first crash dump)
    if (!cpu->exec_trace_frozen) {
        cpu->exec_trace_pc[cpu->exec_trace_head]    = cpu->current_pc;
        cpu->exec_trace_instr[cpu->exec_trace_head] = instruction;
        cpu->exec_trace_head = (cpu->exec_trace_head + 1) & (EXEC_TRACE_SIZE - 1);
        if (cpu->exec_trace_count < EXEC_TRACE_SIZE) cpu->exec_trace_count++;
    }

    // --- 4. Update Branch State ---
    cpu->branch_taken = false;
    
    // Advance PC
    cpu->pc = cpu->next_pc;
    cpu->next_pc = cpu->pc + 4;
    
    /* --- 5. (was: commit the second register file into the first) ---
     *
     * Gone. Writes land in cpu->regs directly now, so there is nothing to
     * commit; see the comment on Cpu::regs in cpu.h for why that is
     * behaviour-preserving. What used to be here was
     * `memcpy(cpu->regs, cpu->out_regs, sizeof(cpu->regs))` — 128 bytes per
     * instruction, and the largest single constant on this path. */

    // --- Breakpoint check (before executing the instruction, so cpu->regs
    // reflects fully-settled state — a debugger callback that runs any earlier
    // shows register values stale by one instruction for whatever the
    // immediately preceding instruction just wrote). ---
    //
    // Gated on the three things that can make it do anything: a breakpoint in
    // the list, a pending step-off, or a pause already raised during this
    // instruction (a read watchpoint can fire on the fetch above). With all
    // three clear, debugger_check_breakpoint() finds the filter empty and
    // returns, and `paused` is false, so skipping the call changes nothing;
    // it was a cross-unit call and a hash per instruction, ~1% of samples.
    {
        Debugger* dbg = &cpu->inter->debugger;
        if (__builtin_expect((dbg->breakpoint_count != 0) | dbg->step_skip_bp | dbg->paused, 0)) {
            if (!dbg->step_skip_bp) {
                debugger_check_breakpoint(dbg, cpu);
                if (dbg->paused) return;
            } else {
                dbg->step_skip_bp = false;
            }
        }
    }

    // --- BIOS syscall side-channel capture (A0h/B0h/C0h) ---
    // Must fire here, once control actually reaches the vector address, NOT
    // inside op_jr's own handler: the real calling convention sets $t1 (the
    // function-select register) in the JR's delay-slot instruction (e.g.
    // "jr $10 ; addiu $9,$0,0xA1", confirmed via disassembly trace), so $t1
    // is only valid after that delay-slot instruction has committed — which
    // happens via the regs commit just above, one cpu_run_next_instruction
    // call after the JR itself. Intercepting inside op_jr would read $t1 one
    // instruction too early (stale, misattributing calls in debug logs).
    if (cpu->current_pc == 0x000000A0 || cpu->current_pc == 0x000000B0 || cpu->current_pc == 0x000000C0) {
        bool hle = false;
        if (cpu->current_pc == 0x000000A0)
            hle = handle_a0_syscall(cpu);
        else if (cpu->current_pc == 0x000000B0)
            handle_b0_syscall(cpu);
        else
            handle_c0_syscall(cpu);
        /* LLE: real BIOS code executes normally unless HLE'd. */
        if (hle) {
            /* This stands in for a whole BIOS call, so it counts as a completed
             * opcode for the load pipeline too — skipping the rotation would
             * hold an in-flight load one instruction longer than hardware. */
            cpu_retire_load_delay(cpu);
            cpu->pc = cpu->regs[31];  // $ra: return to caller, as if the call fully executed
            cpu->next_pc = cpu->pc + 4;
            cpu->branch_taken = true;
            return;
        }
    }

    // --- 6. Decode and Execute ---
    decode_and_execute(cpu, instruction);
    if (cpu->exception_pending) {
        /* cpu_exception() already drained the pipeline: the load completes
         * during handling (cpuspecifications.md:175-177), and this instruction
         * never completed, so there is nothing to rotate. */
        return;
    }

    // --- 7. Finalize ---
    /* The instruction has completed, so the previous one's load lands now and
     * this one's takes its place. Anything this instruction wrote to the same
     * register already cancelled the pending load inside cpu_set_reg. */
    cpu_retire_load_delay(cpu);
    cpu->regs[REG_ZERO] = 0;   /* cheap belt-and-braces; cpu_set_reg drops R0 writes */

    // --- 8. Advance Cycle Counters ---
    // Base cost is one cycle, plus whatever this instruction's data access(es)
    // owed. The bus accumulated that during execute (bus_charge_cpu_data_access);
    // charging it here rather than inside the access keeps a single instruction's
    // cost atomic, so an event scheduled mid-instruction cannot fire between an
    // instruction's memory stall and its retirement.
    uint32_t stall = cpu->inter->cpu_mem_stall_cycles;
    cpu->inter->cpu_mem_stall_cycles = 0;
    cpu->inter->cpu_cycle_counter += 1u + stall;
    cpu->inter->instructions_retired++;
    cpu->downcount -= (int32_t)(1u + stall);

    // --- 9. Dispatch Events (event-scheduler downcount) ---
    if (cpu->downcount <= 0) {
        eventq_dispatch_due(cpu->inter);
        // Recalculate downcount = cycles until next scheduled event
        uint32_t next = cpu->inter->evq_next_cycle;
        uint32_t now  = cpu->inter->cpu_cycle_counter;
        cpu->downcount = (next != UINT32_MAX && (int32_t)(next - now) > 0)
                       ? (int32_t)(next - now) : 1;
    }
}

// Main execution cycle - called for each instruction
void cpu_run_next_instruction(Cpu* cpu) {
    cpu_step(cpu);
}

/* Run until the VBlank event ends the field, the debugger pauses, or the safety
 * cap is reached: the loop system_run_frame() used to drive one call at a
 * time, with the same three tests after every instruction in the same order,
 * so a breakpoint, a watchpoint or a Lua pause still stops the machine on the
 * exact instruction it did before. Returns false when the stop was a pause,
 * so the caller can skip its end-of-field work as it always has. */
bool cpu_run_frame(Cpu* cpu, uint32_t start_cycle, uint32_t cap_cycles) {
    Interconnect* inter = cpu->inter;
    while (!inter->frame_complete) {
        cpu_step(cpu);
        if (inter->debugger.paused) return false;                  /* breakpoint hit mid-frame */
        if (inter->cpu_cycle_counter - start_cycle >= cap_cycles) break;  /* safety */
    }
    return true;
}

// Safe memory peek for trace output — no bus side effects, no exceptions.
static uint32_t trace_peek32(const Interconnect* inter, uint32_t vaddr) {
    uint32_t phys = vaddr & 0x1FFFFFFF; // strip KUSEG/KSEG0/KSEG1 prefix
    if (phys < RAM_SIZE - 3)
        return (uint32_t)inter->ram->data[phys]       |
               ((uint32_t)inter->ram->data[phys+1]<<8) |
               ((uint32_t)inter->ram->data[phys+2]<<16)|
               ((uint32_t)inter->ram->data[phys+3]<<24);
    if (phys >= 0x1FC00000 && phys <= 0x1FC7FFFC) {
        uint32_t off = phys - 0x1FC00000;
        return (uint32_t)inter->bios->data[off]       |
               ((uint32_t)inter->bios->data[off+1]<<8) |
               ((uint32_t)inter->bios->data[off+2]<<16)|
               ((uint32_t)inter->bios->data[off+3]<<24);
    }
    if (phys >= 0x1F800000 && phys <= 0x1F8003FC) {
        uint32_t off = phys - 0x1F800000;
        return (uint32_t)inter->scratchpad[off]       |
               ((uint32_t)inter->scratchpad[off+1]<<8) |
               ((uint32_t)inter->scratchpad[off+2]<<16)|
               ((uint32_t)inter->scratchpad[off+3]<<24);
    }
    return 0xDEADBEEF;
}

static bool trace_is_code_addr(uint32_t v) {
    if ((v & 3) != 0) return false; // must be aligned
    uint32_t phys = v & 0x1FFFFFFF;
    // RAM: 0x80–0x1FFFFF, BIOS ROM: 0x1FC00000–0x1FC7FFFF
    return (phys >= 0x80 && phys < 0x200000) ||
           (phys >= 0x1FC00000 && phys < 0x1FC80000);
}

void cpu_dump_exec_trace(const Cpu* cpu, const char* path) {
    FILE* f = fopen(path, "w");
    if (!f) return;
    const Interconnect* inter = cpu->inter;

    // --- 1. Register snapshot ---
    fprintf(f, "=== CPU Register Snapshot ===\n");
    fprintf(f, "PC=%08X  nPC=%08X  curPC=%08X\n",
            cpu->pc, cpu->next_pc, cpu->current_pc);
    static const char* const rn[] = {
        "$0","at","v0","v1","a0","a1","a2","a3",
        "t0","t1","t2","t3","t4","t5","t6","t7",
        "s0","s1","s2","s3","s4","s5","s6","s7",
        "t8","t9","k0","k1","gp","sp","fp","ra"
    };
    for (int i = 0; i < 32; i++) {
        fprintf(f, " %s=%08X", rn[i], cpu->regs[i]);
        if ((i & 3) == 3) fputc('\n', f);
    }
    fprintf(f, " HI=%08X  LO=%08X\n", cpu->hi, cpu->lo);
    fprintf(f, " SR=%08X  Cause=%08X  EPC=%08X\n\n", cpu->sr, cpu->cause, cpu->epc);

    // --- 2. $ra context (call site) ---
    uint32_t ra = cpu->regs[31];
    fprintf(f, "=== $ra = 0x%08X (return site) ===\n", ra);
    if (inter && trace_is_code_addr(ra)) {
        for (int d = -3; d <= 2; d++) {
            uint32_t a = (uint32_t)((int32_t)ra + d * 4);
            uint32_t w = trace_peek32(inter, a);
            const char* dis = disassemble_mips(w, a);
            fprintf(f, " %s%08X  %08X  %s\n",
                    (d == -1) ? "CALL> " : "      ", a, w, dis ? dis : "?");
        }
    } else {
        fprintf(f, " (invalid or unavailable)\n");
    }
    fputc('\n', f);

    // --- 3. Stack walk — find saved return addresses ---
    uint32_t sp = cpu->regs[29];
    fprintf(f, "=== Stack Walk ($sp=0x%08X, scanning 64 words upward) ===\n", sp);
    int found = 0;
    if (inter) {
        for (int i = 0; i < 64; i++) {
            uint32_t addr = sp + (uint32_t)(i * 4);
            if ((addr & 3) != 0) continue;
            uint32_t val = trace_peek32(inter, addr);
            if (val == 0xDEADBEEF || !trace_is_code_addr(val)) continue;
            // Bonus: check if instruction at val-8 looks like JAL/JALR
            uint32_t caller_instr = trace_peek32(inter, val - 8);
            uint32_t op = caller_instr >> 26;
            uint32_t fn = caller_instr & 0x3F;
            bool is_call = (op == 3) || (op == 0 && fn == 9); // JAL or JALR
            const char* dis = disassemble_mips(caller_instr, val - 8);
            fprintf(f, " SP+%03X [%08X] => %08X%s  %08X  %s\n",
                    i * 4, addr, val, is_call ? " (JAL)" : "      ",
                    caller_instr, dis ? dis : "?");
            found++;
        }
    }
    if (found == 0) fprintf(f, " (no code pointers found)\n");
    fputc('\n', f);

    // --- 4. Execution trace ---
    uint32_t count = cpu->exec_trace_count;
    uint32_t head  = cpu->exec_trace_head;
    uint32_t start = (head - count) & (EXEC_TRACE_SIZE - 1);
    fprintf(f, "=== Execution Trace (last %u instructions) ===\n", count);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t idx = (start + i) & (EXEC_TRACE_SIZE - 1);
        uint32_t pc  = cpu->exec_trace_pc[idx];
        uint32_t ins = cpu->exec_trace_instr[idx];
        const char* dis = disassemble_mips(ins, pc);
        fprintf(f, "%s[%4u] %08X  %08X  %s\n",
                (i == count - 1) ? ">>> " : "    ",
                i, pc, ins, dis ? dis : "?");
    }
    fclose(f);
}
