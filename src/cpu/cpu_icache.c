/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 ZioZoni95
 *
 * Part of ZoniStation One, a PlayStation 1 emulator.
 * See LICENSE for the full licence text and THIRD-PARTY.md for the
 * components of this project that have other authors.
 */
#include "cpu.h"
#include "cpu_mem.h"
#include "interconnect.h"

// --- Instruction Cache ---
/**
 * @brief Fetches an instruction word from memory, checking the instruction cache first.
 * Handles cache lookup, hit/miss logic, and fetching from interconnect on miss.
 * Based on Guide Section 8.1 and 8.2 principles.
 * @param cpu Pointer to the Cpu state (containing the cache).
 * @param vaddr The virtual address of the instruction to fetch.
 * @return The 32-bit instruction word.
 */
// BIOS ROM occupies physical 0x1FC00000-0x1FC7FFFF (512 KB) — see CLAUDE.md memory map.
#define BIOS_ROM_PHYS_START 0x1FC00000u
#define BIOS_ROM_PHYS_END   0x1FC80000u

// Per-instruction BIOS ROM wait-state costing: ~24 extra cycles per word, from the
// MEMCTRL delay registers the guest itself programs (bus_memctrl_recalculate() in bus.c).
//
// ON since 2026-08-17. It was disabled for a long time because enabling it hung boot
// (the drive never got past its early reads), and the note here blamed an unidentified
// BIOS-interrupt / CD second-response race. That was the right suspicion in the wrong
// place: the CPU cost was correct all along and the drive was broken, in two ways that
// only a slow BIOS could expose (both fixed in cdrom.c / cdrom_commands.c).
//   1. Acknowledging an interrupt re-armed any deferred response at CDROM_MIN_INT_DELAY,
//      throwing its real deadline away — every seek, spin-up and read start answered
//      ~30 us after its command. With ROM-resident code running 25x slower, that reply
//      landed before the BIOS had finished the routine that issued the command.
//   2. Init charged an undocumented spin-down, putting its second response 821 ms out,
//      past the ~415 ms after which the BIOS retries Init — an endless retry loop.
//
// The measurement that decides whether this belongs on: boot milestones in emulated
// fields (the TTY lines carry the log's f-stamp) against a DuckStation Devel-build run
// of the same disc. Off, this machine reached `Execute !` at field 444 against
// DuckStation's 819 — 1.8x too fast, and up to 7x on the ROM-only phases. On, it reaches
// it at 765, and every milestone lands within ~4%.
#define ZS1_ENABLE_BIOS_ROM_CYCLE_COST 1

static inline void charge_bios_rom_cycles(Cpu* cpu, uint32_t paddr, uint32_t word_count, bool count_cycles) {
#if ZS1_ENABLE_BIOS_ROM_CYCLE_COST
    if (!count_cycles) return;
    if (paddr < BIOS_ROM_PHYS_START || paddr >= BIOS_ROM_PHYS_END) return;
    uint32_t extra = cpu->inter->bios_access_cycles * word_count;
    cpu->inter->cpu_cycle_counter += extra;
    cpu->downcount -= (int32_t)extra;
#else
    (void)cpu; (void)paddr; (void)word_count; (void)count_cycles;
#endif
}

/* Instruction fetches go through interconnect_load32, which now charges the CPU
 * data-access cost (bus.c). That cost models a *data* load: the instruction side
 * has its own model — the cache above plus charge_bios_rom_cycles — and paying
 * both would charge one fetch twice. So the accumulator is held across the fetch
 * and put back, keeping the data-access cost strictly about data. */
#define ICACHE_FETCH_NO_DATA_STALL(cpu, expr)                       \
    do {                                                            \
        uint32_t _saved = (cpu)->inter->cpu_mem_stall_cycles;       \
        (expr);                                                     \
        (cpu)->inter->cpu_mem_stall_cycles = _saved;                \
    } while (0)

uint32_t cpu_icache_fetch(Cpu* cpu, uint32_t vaddr, bool count_cycles) {
    // --- Cache Bypass Check ---
    // KSEG1 region (0xA0000000 - 0xBFFFFFFF) is un-cached.
    // Check the top 3 bits. If they are 101 (binary), it's KSEG1.
    if ((vaddr >> 29) == 0b101) {
        // KSEG1: Bypass cache, fetch directly from interconnect
        // printf("~ I-Cache Bypass (KSEG1 address: 0x%08x)\n", vaddr); // Optional debug
        uint32_t instruction;
        ICACHE_FETCH_NO_DATA_STALL(cpu, instruction = interconnect_load32(cpu->inter, vaddr));
        charge_bios_rom_cycles(cpu, vaddr & 0x1FFFFFFFu, 1, count_cycles);
        return instruction;
    }
    // SR.IsC/SwC ("isolate/swap cache") are data-cache concerns per real R3000A semantics
    // (the isolate/swap bits swap only the data LOAD/STORE handler tables, never the
    // instruction-fetch path) — already handled at the data load/store level (see the
    // `cpu->sr & 0x10000` checks in cpu_instructions.c).
    // Instruction fetch is correctly unaffected by either bit; no fetch-path change needed.


    // --- Address Calculation ---
    // The cache uses physical addresses for tags and indexing. The inline copy
    // of mask_region (cpu_mem.h): this runs once per instruction.
    uint32_t paddr = bus_mask_region(vaddr);

    // Extract cache components from physical address (based on 4KB, 4-word lines)
    // Tag:          Bits [31:12] of paddr
    // Line Index:   Bits [11:4] of paddr (determines which of the 256 lines)
    // Word Index:   Bits [3:2]  of paddr (determines which word within the line)
    //
    uint32_t tag        = paddr >> 12;
    uint32_t line_index = (paddr >> 4) & (ICACHE_NUM_LINES - 1); // Mask for 256 lines (0xFF)
    uint32_t word_index = (paddr >> 2) & (ICACHE_LINE_WORDS - 1); // Mask for 4 words (0x3)

    // Get pointer to the relevant cache line
    ICacheLine* line = &cpu->icache[line_index];

    // --- Cache Lookup ---
    if (line->tag == tag && line->valid[word_index]) {
        // Cache Hit! Tags match and the specific word is valid.
        // printf("~ I-Cache Hit:  0x%08x (Line: %u, Word: %u)\n", vaddr, line_index, word_index); // Optional debug
        return line->data[word_index];
    }

    // --- Cache Miss ---
    // printf("~ I-Cache Miss: 0x%08x (Line: %u, Word: %u)\n", vaddr, line_index, word_index); // Optional debug

    // Fetch the required block from memory.
    // According to the guide, on a miss for word N,
    // words N through 3 of that cache line are fetched from memory.
    // Words 0 through N-1 are not fetched in this operation.

    // Calculate the physical address corresponding to the start of the cache line.
    uint32_t line_paddr_start = paddr & ~((ICACHE_LINE_WORDS * 4) - 1); // Align down to 16-byte boundary (mask low 4 bits)

    // Update the tag for the cache line (this happens even on miss)
    line->tag = tag;

    // Invalidate words in the line *before* the one we are fetching,
    // as the tag has changed, making any previous data for a different tag invalid.
    for (uint32_t j = 0; j < word_index; ++j) {
        line->valid[j] = false;
    }

    // Fetch words from memory starting at the missed word's index up to the end of the line.
    for (uint32_t j = word_index; j < ICACHE_LINE_WORDS; ++j) {
        // Calculate the physical address for this word
        uint32_t fetch_paddr = line_paddr_start + (j * 4);
        // Fetch from interconnect (bypassing cache itself - interconnect doesn't call back here)
        uint32_t instruction_data;
        ICACHE_FETCH_NO_DATA_STALL(cpu, instruction_data = interconnect_load32(cpu->inter, fetch_paddr));
        // Store fetched data in the cache line
        line->data[j] = instruction_data;
        // Mark this word as valid
        line->valid[j] = true;
    }
    charge_bios_rom_cycles(cpu, line_paddr_start + (word_index * 4), ICACHE_LINE_WORDS - word_index, count_cycles);

    // Return the instruction data for the originally requested word index
    return line->data[word_index];
}