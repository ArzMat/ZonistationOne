/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 ZioZoni95
 *
 * Part of ZoniStation One, a PlayStation 1 emulator.
 * See LICENSE for the full licence text and THIRD-PARTY.md for the
 * components of this project that have other authors.
 */
#ifndef CPU_MEM_H
#define CPU_MEM_H

/* The CPU's own view of memory: the bits of the bus that sit on the
 * interpreter's hot path, as static inline functions.
 *
 * Everything here is either an exact restatement of what bus.c does for the
 * same access, or a pure helper with no machine state at all. Nothing in this
 * file may be used by the DMA controller: the DMA loops reach RAM through
 * interconnect_load32() on purpose, because that call charges the CPU a load
 * stall per word and that stall is, today, the whole of the emulated cost of a
 * DMA (docs/ANALISI_PERF_AUDIO_FMV_2026-10-02.md section 5.1). Short-cutting
 * those reads would change emulated timing; short-cutting the CPU's own does
 * not, provided the stall below is the same number. */

#include <stdbool.h>
#include <stdint.h>
#include "interconnect.h"
#include "ram.h"

/* Defined in bus.c. KUSEG and KSEG2 pass through, KSEG0 drops bit 31, KSEG1
 * drops bits 31-29. */
extern const uint32_t REGION_MASK[8];

/* Extra cycles a CPU data load from main RAM costs: bus.c's ram_load_stall(),
 * which reads ZS1_RAM_LOAD_STALL once, primed in bus_hw_tables_init() before
 * the first instruction runs. */
extern uint32_t g_bus_ram_load_stall;

/* Main RAM, mirrored four times across the first 8 MB of the physical map. The
 * same bound bus.c uses for both the data access and its stall charge. */
#define CPU_MEM_RAM_WINDOW_END 0x00800000u

/* Identical to mask_region() in bus.c (which interconnect.h declares out of
 * line); here so the instruction fetch and the RAM fast path do not pay a
 * cross-unit call for a single AND. `addr >> 29` is already 0..7. */
static inline uint32_t bus_mask_region(uint32_t addr) {
    return addr & REGION_MASK[addr >> 29];
}

/* --- RAM fast path for CPU loads ---
 *
 * interconnect_load32/16/8() spend most of their time deciding that an access
 * is a plain RAM read: an alignment test, a call into the debugger's read
 * watchpoint filter, the region mask, the stall charge, then ram_load*() in
 * another unit with a bounds check of its own. For the common case all of that
 * collapses to the five things below, and the result is the same value and
 * the same stall:
 *
 *   - misaligned: falls back, so the slow path raises the address error;
 *   - a read watchpoint is armed: falls back, so the debugger sees the access.
 *     read_watchpoint_count is the flag; debugger.c keeps it current whenever
 *     the list changes, and with it at zero the slow path's filter is empty and
 *     debugger_check_read_watchpoint() returns without doing anything;
 *   - not main RAM: falls back (scratchpad, I/O, ROM, expansion, unmapped);
 *   - otherwise: charge g_bus_ram_load_stall exactly as bus_charge_cpu_load()
 *     does for phys < 8 MB, and read the mirrored offset directly. ram_load*()
 *     can never reject an aligned offset below RAM_SIZE, so skipping it skips
 *     nothing.
 *
 * Callers keep their cache-isolation test (SR bit 16) in front of this, as
 * they had it in front of interconnect_load*(). Returns false when the caller
 * must take the slow path, which is then unchanged. */
static inline bool cpu_ram_fast_ok(const Interconnect* inter, uint32_t addr,
                                   uint32_t align_mask, uint32_t* off_out) {
    const uint32_t phys = bus_mask_region(addr);
    if (__builtin_expect(((addr & align_mask) != 0) |
                         (inter->debugger.read_watchpoint_count != 0) |
                         (phys >= CPU_MEM_RAM_WINDOW_END), 0))
        return false;
    *off_out = phys & (RAM_SIZE - 1);
    return true;
}

static inline bool cpu_ram_try_load32(Interconnect* inter, uint32_t addr, uint32_t* out) {
    uint32_t off;
    if (!cpu_ram_fast_ok(inter, addr, 3u, &off)) return false;
    inter->cpu_mem_stall_cycles += g_bus_ram_load_stall;
    const uint8_t* p = &inter->ram->data[off];
    *out = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    return true;
}

static inline bool cpu_ram_try_load16(Interconnect* inter, uint32_t addr, uint16_t* out) {
    uint32_t off;
    if (!cpu_ram_fast_ok(inter, addr, 1u, &off)) return false;
    inter->cpu_mem_stall_cycles += g_bus_ram_load_stall;
    const uint8_t* p = &inter->ram->data[off];
    *out = (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
    return true;
}

static inline bool cpu_ram_try_load8(Interconnect* inter, uint32_t addr, uint8_t* out) {
    uint32_t off;
    if (!cpu_ram_fast_ok(inter, addr, 0u, &off)) return false;
    inter->cpu_mem_stall_cycles += g_bus_ram_load_stall;
    *out = inter->ram->data[off];
    return true;
}

/* The CPU loads, each one the fast path or exactly the call it replaces. */
static inline uint32_t cpu_load32(Interconnect* inter, uint32_t addr) {
    uint32_t v;
    return cpu_ram_try_load32(inter, addr, &v) ? v : interconnect_load32(inter, addr);
}
static inline uint16_t cpu_load16(Interconnect* inter, uint32_t addr) {
    uint16_t v;
    return cpu_ram_try_load16(inter, addr, &v) ? v : interconnect_load16(inter, addr);
}
static inline uint8_t cpu_load8(Interconnect* inter, uint32_t addr) {
    uint8_t v;
    return cpu_ram_try_load8(inter, addr, &v) ? v : interconnect_load8(inter, addr);
}

#endif /* CPU_MEM_H */
