/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 ZioZoni95
 *
 * Part of ZoniStation One, a PlayStation 1 emulator.
 * See LICENSE for the full licence text and THIRD-PARTY.md for the
 * components of this project that have other authors.
 */
#ifndef CPU_H
#define CPU_H

#include <stdbool.h> // For bool type
#include <stdint.h>  // For uint32_t, int32_t etc.
#include "interconnect.h" // Needs definition of Interconnect for the pointer member
#include "gte.h" // GTE (Geometry Transformation Engine)

// Forward declaration for Cpu struct
// This is needed because handle_bios_syscall uses a Cpu* pointer before Cpu is fully defined.
typedef struct Cpu Cpu;

// Define Register Index type for clarity (can just be uint32_t if preferred)
// Using a distinct type can help prevent mixing indices and values, though not strictly enforced here.
typedef uint32_t RegisterIndex;

// Define constants for special register indices
#define REG_ZERO ((RegisterIndex)0)  // $zero GPR, always 0
#define REG_RA   ((RegisterIndex)31) // $ra GPR, Return Address for JAL/JALR

// --- BIOS Boot Stages ---
// Tracks what stage the BIOS is in for better debugging visibility
typedef enum {
    BOOT_STAGE_POWER_ON = 0,          // Initial power-on, BIOS starting
    BOOT_STAGE_BIOS_INIT,             // BIOS initializing hardware (RAM, controllers, etc.)
    BOOT_STAGE_LOGO_ANIMATION,        // PlayStation logo animation playing
    BOOT_STAGE_PATCH_CHECK,           // BIOS checking for game patches (can loop)
    BOOT_STAGE_CDROM_CHECK,           // BIOS checking for CD-ROM disc
    BOOT_STAGE_WAITING_INPUT,         // Waiting for controller input or disc
    BOOT_STAGE_BIOS_MENU,             // BIOS menu active (memory card management, etc.)
    BOOT_STAGE_GAME_BOOT,             // Booting game from CD-ROM
    BOOT_STAGE_GAME_RUNNING           // Game code is running
} BootStage;

// --- Exception Cause Codes ---
// MIPS Exception codes used in the Cause register (bits 6:2)
// Based on MIPS spec / Guide exception sections
typedef enum {
    EXCEPTION_INTERRUPT        = 0x00, // Hardware Interrupt requested (from I_STAT/I_MASK)
    EXCEPTION_LOAD_ADDRESS_ERROR = 0x04, // Data Load/Instruction Fetch Address Error (Alignment or Bus Error)
    EXCEPTION_STORE_ADDRESS_ERROR= 0x05, // Data Store Address Error (Alignment or Bus Error)
    EXCEPTION_SYSCALL          = 0x08, // Syscall instruction executed
    EXCEPTION_BREAK            = 0x09, // Break instruction executed
    EXCEPTION_ILLEGAL_INSTRUCTION= 0x0a, // CPU encountered an undefined/illegal instruction
    EXCEPTION_COPROCESSOR_ERROR= 0x0b, // Coprocessor Unusable (COP0/COP1/COP2/COP3 operation error)
    EXCEPTION_OVERFLOW         = 0x0c  // Arithmetic Overflow (ADD/ADDI/SUB instructions)
} ExceptionCause;


// ============================================================= //
// ==========>>>  INSTRUCTION CACHE PARTS  <<<========== //
// ============================================================= //

/**
 * @brief Constants defining the instruction cache geometry.
 * Based on Guide Section 8.1
 */
#define ICACHE_NUM_LINES 256       // 256 lines in the cache
#define ICACHE_LINE_WORDS 4        // 4 words (instructions) per cache line
#define ICACHE_SIZE_BYTES (ICACHE_NUM_LINES * ICACHE_LINE_WORDS * 4) // 4096 bytes total

/**
 * @brief Represents a single line in the instruction cache.
 * Contains the tag, valid bits for each word, and the cached instruction data.
 * Based on Guide Section 8.1
 */
typedef struct {
    /**
     * @brief The upper 20 bits of the physical address stored in this cache line.
     * Used to verify if the cached data matches the requested address.
     */
    uint32_t tag;
    /**
     * @brief Validity flag for each of the 4 words in the cache line.
     * True if the corresponding data word holds valid instruction data.
     */
    bool     valid[ICACHE_LINE_WORDS];
    /**
     * @brief The 4 cached instruction words (32-bit each).
     */
    uint32_t data[ICACHE_LINE_WORDS];
} ICacheLine;

// ============================================================= //
// ============================================================= //


// --- CPU State Structure ---
// Defines the internal state of the emulated MIPS R3000A-compatible CPU.
typedef struct Cpu {
    // --- Core Registers ---
    uint32_t pc;            // Program Counter: Address of the instruction currently being fetched.
    uint32_t next_pc;       // Address of the instruction *after* the delay slot (used for branch delay).
    uint32_t current_pc;    // Address of the instruction currently executing (used for exception EPC).

    /* --- General Purpose Registers (GPRs) ---
     *
     * One file, not two. There used to be a second `out_regs[32]` that every
     * write went to, with a 128-byte memcpy per instruction to commit it into
     * this one — the classic two-file way of modelling the load delay. It cost
     * 1.8% of all samples in a perf profile (3.5% once LTO had inlined the
     * cheap callees around it), which for ~20M instructions a second is the
     * single largest constant on the interpreter's hot path.
     *
     * Removing it is behaviour-preserving here, and that was checked rather
     * than assumed: every instruction handler in cpu_instructions.c reads its
     * source registers before writing its destination — the only reads that
     * appear "after" a write are arguments of the write itself
     * (`cpu_set_reg(rt, cpu_reg(rs) op imm)`), which C evaluates first. The
     * load delay does not depend on the second file either: it has its own
     * slot, load_reg_idx/load_value below. */
    uint32_t regs[32];      // R0 is hardwired to 0.

    /* --- Load Delay Slot ---
     *
     * "The loaded data is NOT available to the next opcode, ie. the target
     * register isn't updated until the next opcode has COMPLETED. So, if the
     * next opcode tries to read from the load destination register, then it
     * would (usually) receive the OLD value of that register"
     * (psx-spx-docs/docs/cpuspecifications.md:172-175).
     *
     * Two slots, because that sentence describes a two-stage pipeline:
     *
     *   load_reg_idx/load_value   the load THIS instruction just issued
     *   delay_load_reg/_value     the load the PREVIOUS instruction issued,
     *                             which lands when this one completes
     *
     * cpu_retire_load_delay() in cpu_execution.c rotates them, and it runs
     * after the instruction has executed — which is what makes the delay-slot
     * instruction read the old value. "Until the next opcode has completed"
     * also settles the collision: if that opcode writes the same register, its
     * write is the later one and wins, so cpu_set_reg cancels a delayed load
     * aimed at the register it is writing.
     *
     * The exception is spelled out too: "unless an IRQ occurs between the load
     * and next opcode, in that case the load would complete during IRQ
     * handling" (:175-177) — so cpu_exception() lands the delayed load on the
     * way in. */
    RegisterIndex load_reg_idx; // Target register of the load issued this instruction.
    uint32_t load_value;        // Value it will deliver.
    RegisterIndex delay_load_reg;   // Target register of the previous instruction's load.
    uint32_t delay_load_value;      // Value it will deliver when this instruction completes.


    // --- HI/LO Registers ---
    // Used for results of multiplication and division.
    uint32_t hi;            // Remainder (division), High 32 bits (multiplication).
    uint32_t lo;            // Quotient (division), Low 32 bits (multiplication).

    // --- Branch Delay Slot State ---
    bool branch_taken;      // True if the current instruction caused a jump/branch.
    bool in_delay_slot;     // True if the current instruction is executing in a branch delay slot.
    bool exception_pending; // Set to true if an exception is raised during instruction execution

    // --- Coprocessor 0 (System Control Coprocessor) Registers ---
    uint32_t sr;            // COP0 Reg 12: Status Register (Interrupt enables, Cache isolation, etc.).
    uint32_t cause;         // COP0 Reg 13: Cause Register (Exception code, pending interrupts, branch delay flag).
    uint32_t epc;           // COP0 Reg 14: Exception Program Counter (Address of instruction causing exception).
    uint32_t badvaddr;      // COP0 Reg 8: Bad Virtual Address (Address that caused address error exception).
    uint32_t prid;          // COP0 Reg 15: Processor Revision Identifier (Read-only, 0x00000002 for PSX).
    uint32_t cop0_tar;      // COP0 Reg 6: TAR / JUMPDEST (last branch target address).
    uint32_t cop0_dcic;     // COP0 Reg 7: DCIC (Debug/Cache Isolation Control).
    /* The four hardware-breakpoint registers. The breakpoint behaviour itself
     * is not implemented, but the registers are R/W (cpuspecifications.md:
     * 573-581) and at least one game uses them as general-purpose storage:
     * Dino Crisis (E) keeps its LibCrypt table pointer in BDAM and reads it
     * back with MFC0, which returned 0 while these were write-ignored. */
    uint32_t cop0_bpc;      // COP0 Reg 3:  BPC  (Breakpoint Program Counter).
    uint32_t cop0_bda;      // COP0 Reg 5:  BDA  (Breakpoint Data Address).
    uint32_t cop0_bdam;     // COP0 Reg 9:  BDAM (Breakpoint Data Address Mask).
    uint32_t cop0_bpcm;     // COP0 Reg 11: BPCM (Breakpoint PC Mask).

    // --- Connection to Memory System ---
    Interconnect* inter;    // Pointer to the interconnect module for memory accesses.

    // --- Instruction Cache ---
    ICacheLine icache[ICACHE_NUM_LINES];

    // --- GTE (Geometry Transformation Engine) ---
    Gte gte;                // GTE coprocessor state

    // --- Boot Stage Tracking ---
    BootStage boot_stage;   // Current BIOS/boot stage for debugging visibility

    // --- Cycle Accounting (event-scheduler downcount) ---
    int32_t  downcount;              // countdown to next event (decrements per instruction)
    uint32_t muldiv_completion_tick; // cycle when pending MULT/DIV finishes (for MFHI/MFLO stall)
    uint32_t gte_completion_tick;    // cycle when pending GTE op finishes (for MFC2/CFC2/next-op stall)

    // --- Execution Trace Ring Buffer ---
    /* 1024 entries, 8 KB. It was 8192 (64 KB), written on every instruction:
     * two stores walking a 64 KB window, which is larger than the L1 data
     * cache of the cores this runs on, so every sixteen instructions it pulled
     * two fresh lines in and pushed out the register file, the instruction
     * cache model, the scratchpad and the hot Interconnect fields. Its readers
     * are the Exec Trace tab and the crash dumps, which want the last few
     * hundred instructions before a fault, not the last eight thousand.
     * Changes sizeof(Cpu), which savestate.c writes raw as T_CPU: a state saved
     * by an earlier build is refused with a clean "different layout" error. */
#define EXEC_TRACE_SIZE 1024  // must be power-of-2
    uint32_t exec_trace_pc[EXEC_TRACE_SIZE];
    uint32_t exec_trace_instr[EXEC_TRACE_SIZE];
    uint32_t exec_trace_head;   // next write index
    uint32_t exec_trace_count;  // entries filled (capped at EXEC_TRACE_SIZE)
    bool     exec_trace_frozen; // stop writing once set (freeze on first crash)

} Cpu;


// --- Helper Macros/Functions for Instruction Decoding ---
// Static inline functions for efficient extraction of instruction fields.

static inline uint32_t instr_function(uint32_t i) { return i >> 26; } // Opcode
static inline uint32_t instr_s(uint32_t i) { return (i >> 21) & 0x1F; } // Reg rs
static inline uint32_t instr_t(uint32_t i) { return (i >> 16) & 0x1F; } // Reg rt
static inline uint32_t instr_d(uint32_t i) { return (i >> 11) & 0x1F; } // Reg rd
static inline uint32_t instr_imm(uint32_t i) { return i & 0xFFFF; } // Imm (zero-extended)
static inline uint32_t instr_imm_se(uint32_t i) { return (uint32_t)(int32_t)(int16_t)(i & 0xFFFF); } // Imm (sign-extended)
static inline uint32_t instr_shift(uint32_t i) { return (i >> 6) & 0x1F; } // Shift amount
static inline uint32_t instr_subfunction(uint32_t i) { return i & 0x3F; } // Sub-opcode (R-Type)
static inline uint32_t instr_imm_jump(uint32_t i) { return i & 0x03FFFFFF; } // Jump target
// Helper for COP0/COPz opcodes (uses 's' field bits)
static inline uint32_t instr_cop_opcode(uint32_t i) { return (i >> 21) & 0x1F; }


// --- Function Declarations (Prototypes) ---

/**
 * @brief Initializes the CPU state to power-on defaults.
 * Sets PC to BIOS entry, clears registers, sets initial COP0 state.
 * @param cpu Pointer to the Cpu struct to initialize.
 * @param inter Pointer to the initialized Interconnect struct.
 */
void cpu_init(Cpu* cpu, Interconnect* inter);

/**
 * @brief Executes a single CPU instruction cycle.
 * Checks for interrupts, handles load delay slot, fetches, decodes, executes,
 * and updates PC/state for the next cycle.
 * @param cpu Pointer to the Cpu state.
 */
void cpu_run_next_instruction(Cpu* cpu);

/**
 * @brief Runs instructions until the field ends (inter->frame_complete), the
 * debugger pauses, or cpu_cycle_counter - start_cycle reaches cap_cycles.
 * Checks the three conditions after every instruction, exactly as
 * system_run_frame() did around cpu_run_next_instruction().
 * @return false if it stopped because the debugger paused.
 */
bool cpu_run_frame(Cpu* cpu, uint32_t start_cycle, uint32_t cap_cycles);

// Dump last EXEC_TRACE_SIZE instructions to file (call on shutdown/crash).
void cpu_dump_exec_trace(const Cpu* cpu, const char* path);

/**
 * @brief Decodes the fetched instruction and calls the appropriate handler function.
 * @param cpu Pointer to the Cpu state.
 * @param instruction The 32-bit instruction word to decode and execute.
 */
void decode_and_execute(Cpu* cpu, uint32_t instruction);

/**
 * @brief Triggers a CPU exception.
 * Saves current state (EPC, Cause, SR), updates SR mode bits,
 * and jumps to the appropriate exception handler vector.
 * @param cpu Pointer to the Cpu state.
 * @param cause The reason for the exception (from ExceptionCause enum).
 */
void cpu_exception(Cpu* cpu, ExceptionCause cause);

// --- BIOS SYSCALL interceptors (side-channel capture) ---
bool handle_a0_syscall(Cpu* cpu);  /* returns true if HLE'd (caller must skip native jump) */
void handle_b0_syscall(Cpu* cpu);
void handle_c0_syscall(Cpu* cpu);


// --- Register Access ---
/* Both are static inline here rather than out of line in cpu_registers.c.
 *
 * They are called two or three times by almost every instruction, and a perf
 * profile put cpu_reg plus mask_region at 1.90% of all samples in call
 * overhead alone (Makefile, LTO note). Out of line they were only inlined when
 * LTO happened to be on; here they always are.
 *
 * The old `index >= 32` guard (with a logger call on the error path) is gone:
 * every caller passes a 5-bit instruction field (instr_s/_t/_d) or a constant,
 * so the index cannot exceed 31 and the guard was a dead compare-and-branch on
 * the hottest path. regs[] is exactly 32 entries. */

/**
 * @brief Reads a General Purpose Register. $zero reads 0 because regs[0] is
 * never written (cpu_set_reg drops R0 writes).
 * @param index 0-31, from a 5-bit instruction field.
 */
static inline uint32_t cpu_reg(const Cpu* cpu, RegisterIndex index) {
    return cpu->regs[index];
}

/**
 * @brief Writes a General Purpose Register.
 * Ignores writes to $zero (index 0), ensuring it remains 0, and cancels a load
 * still in flight for the same register: that load is the earlier write. Its
 * data "isn't updated until the next opcode has completed"
 * (psx-spx-docs/docs/cpuspecifications.md:172-174), and this write belongs to
 * that next opcode, so it is the later one and stands. Without the cancel the
 * load would land afterwards and quietly undo it.
 * @param index 0-31, from a 5-bit instruction field.
 */
static inline void cpu_set_reg(Cpu* cpu, RegisterIndex index, uint32_t value) {
    if (index != REG_ZERO) {
        cpu->regs[index] = value;
        if (index == cpu->delay_load_reg)
            cpu->delay_load_reg = REG_ZERO;
    }
}

/**
 * @brief Lands any in-flight load immediately, for exception entry.
 * See the load-delay commentary on the Cpu struct and
 * psx-spx-docs/docs/cpuspecifications.md:175-177.
 */
void cpu_flush_load_delay(Cpu* cpu);

// --- Branch/Jump Helper ---
/**
 * @brief Updates the next_pc for a branch instruction.
 * Calculates target address based on current PC and sign-extended offset.
 * NOTE: Does NOT set the cpu->branch_taken flag, the caller instruction must do that.
 * @param cpu Pointer to the Cpu state.
 * @param offset_se Sign-extended 16-bit branch offset (*not* shifted).
 */
void cpu_branch(Cpu* cpu, uint32_t offset_se);


/**
 * @brief Fetches an instruction word from memory, using the instruction cache.
 * Handles cache lookup, hit/miss logic, and fetching from interconnect on miss.
 * @param cpu Pointer to the Cpu state (containing the cache).
 * @param vaddr The virtual address of the instruction to fetch.
 * @return The 32-bit instruction word.
 */
uint32_t cpu_icache_fetch(Cpu* cpu, uint32_t vaddr, bool count_cycles);

// --- Disassembler ---
const char* disassemble_mips(uint32_t instruction, uint32_t pc);

// --- Instruction dispatch handler type ---
typedef void (*cpu_handler_t)(Cpu*, uint32_t);

// --- Instruction Handler Prototypes (Internal linkage) ---
// These functions implement the behavior of individual MIPS instructions.
void op_lui(Cpu* cpu, uint32_t instruction);
void op_ori(Cpu* cpu, uint32_t instruction);
void op_sw(Cpu* cpu, uint32_t instruction);
void op_sll(Cpu* cpu, uint32_t instruction);
void op_addiu(Cpu* cpu, uint32_t instruction);
void op_j(Cpu* cpu, uint32_t instruction);
void op_or(Cpu* cpu, uint32_t instruction);
void op_cop0(Cpu* cpu, uint32_t instruction);
void op_mtc0(Cpu* cpu, uint32_t instruction);
void op_rfe(Cpu* cpu, uint32_t instruction);
void op_bne(Cpu* cpu, uint32_t instruction);
void op_addi(Cpu* cpu, uint32_t instruction);
void op_lw(Cpu* cpu, uint32_t instruction);
void op_sltu(Cpu* cpu, uint32_t instruction);
void op_addu(Cpu* cpu, uint32_t instruction);
void op_sh(Cpu* cpu, uint32_t instruction);
void op_jal(Cpu* cpu, uint32_t instruction);
void op_andi(Cpu* cpu, uint32_t instruction);
void op_sb(Cpu* cpu, uint32_t instruction);
void op_jr(Cpu* cpu, uint32_t instruction);
void op_lb(Cpu* cpu, uint32_t instruction);
void op_beq(Cpu* cpu, uint32_t instruction);
void op_mfc0(Cpu* cpu, uint32_t instruction);
void op_and(Cpu* cpu, uint32_t instruction);
void op_add(Cpu* cpu, uint32_t instruction);
void op_bgtz(Cpu* cpu, uint32_t instruction);
void op_blez(Cpu* cpu, uint32_t instruction);
void op_lbu(Cpu* cpu, uint32_t instruction);
void op_jalr(Cpu* cpu, uint32_t instruction);
void op_bxx(Cpu* cpu, uint32_t instruction);
void op_slti(Cpu* cpu, uint32_t instruction);
void op_subu(Cpu* cpu, uint32_t instruction);
void op_sra(Cpu* cpu, uint32_t instruction);
void op_div(Cpu* cpu, uint32_t instruction);
void op_divu(Cpu* cpu, uint32_t instruction);
void op_mflo(Cpu* cpu, uint32_t instruction);
void op_srl(Cpu* cpu, uint32_t instruction);
void op_sltiu(Cpu* cpu, uint32_t instruction);
void op_slt(Cpu* cpu, uint32_t instruction);
void op_mfhi(Cpu* cpu, uint32_t instruction);
void op_syscall(Cpu* cpu, uint32_t instruction);
void op_nor(Cpu* cpu, uint32_t instruction);
void op_mtlo(Cpu* cpu, uint32_t instruction);
void op_mthi(Cpu* cpu, uint32_t instruction);
void op_lhu(Cpu* cpu, uint32_t instruction);
void op_lh(Cpu* cpu, uint32_t instruction);
void op_sllv(Cpu* cpu, uint32_t instruction);
void op_srav(Cpu* cpu, uint32_t instruction);
void op_srlv(Cpu* cpu, uint32_t instruction);
void op_multu(Cpu* cpu, uint32_t instruction);
void op_xor(Cpu* cpu, uint32_t instruction);
void op_break(Cpu* cpu, uint32_t instruction);
void op_mult(Cpu* cpu, uint32_t instruction);
void op_sub(Cpu* cpu, uint32_t instruction);
void op_xori(Cpu* cpu, uint32_t instruction);
void op_cop1(Cpu* cpu, uint32_t instruction);
void op_cop2(Cpu* cpu, uint32_t instruction);
void op_cop3(Cpu* cpu, uint32_t instruction);
void op_lwl(Cpu* cpu, uint32_t instruction);
void op_lwr(Cpu* cpu, uint32_t instruction);
void op_swl(Cpu* cpu, uint32_t instruction);
void op_swr(Cpu* cpu, uint32_t instruction);
void op_lwc0(Cpu* cpu, uint32_t instruction);
void op_lwc1(Cpu* cpu, uint32_t instruction);
void op_lwc2(Cpu* cpu, uint32_t instruction);
void op_lwc3(Cpu* cpu, uint32_t instruction);
void op_swc0(Cpu* cpu, uint32_t instruction);
void op_swc1(Cpu* cpu, uint32_t instruction);
void op_swc2(Cpu* cpu, uint32_t instruction);
void op_swc3(Cpu* cpu, uint32_t instruction);
void op_illegal(Cpu* cpu, uint32_t instruction);

#endif // CPU_H