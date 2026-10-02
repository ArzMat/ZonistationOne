/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 ZioZoni95
 *
 * Part of ZoniStation One, a PlayStation 1 emulator.
 * See LICENSE for the full licence text and THIRD-PARTY.md for the
 * components of this project that have other authors.
 */
/* include/cpu_mem.h: the CPU's RAM fast path.
 *
 * The fast path must take exactly the accesses bus.c would have served as a
 * plain RAM read (aligned, main RAM or a mirror of it, no read watchpoint) and
 * charge exactly the same stall; everything else has to reach the unchanged
 * interconnect_load*() call. The bus is stubbed here so each fallback is
 * counted. */
#include "log.h"
#include "cpu_mem.h"

#include <stdio.h>
#include <string.h>

/* What bus.c defines. Same table, same default stall. */
const uint32_t REGION_MASK[8] = {
    0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
    0x7fffffff, 0x1fffffff, 0xffffffff, 0xffffffff
};
uint32_t g_bus_ram_load_stall = 3;

LogLevel current_log_level = LOG_LEVEL_INFO;
void log_print(LogCategory category, LogLevel level, const char* format, ...) {
    (void)category; (void)level; (void)format;
}

/* The slow path, stubbed: count the fallbacks, return a value the RAM never
 * holds so a wrong fallback cannot pass for a right read. */
static int g_slow32, g_slow16, g_slow8;
uint32_t interconnect_load32(Interconnect* inter, uint32_t address) { (void)inter; (void)address; g_slow32++; return 0xDEADBEEFu; }
uint16_t interconnect_load16(Interconnect* inter, uint32_t address) { (void)inter; (void)address; g_slow16++; return 0xBEEFu; }
uint8_t  interconnect_load8(Interconnect* inter, uint32_t address)  { (void)inter; (void)address; g_slow8++;  return 0xEEu; }

static Interconnect g_inter;
static Ram          g_ram;

static int failures, checks;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* Expect a fast-path load: right value, one stall, no fallback. */
static void expect_fast32(uint32_t addr, uint32_t phys_off) {
    int slow = g_slow32; uint32_t stall = g_inter.cpu_mem_stall_cycles;
    uint32_t v = cpu_load32(&g_inter, addr);
    uint32_t want; memcpy(&want, &g_ram.data[phys_off], 4);   /* host is little-endian here */
    CHECK(g_slow32 == slow, "load32 %08X fell back", addr);
    CHECK(v == want, "load32 %08X = %08X, want %08X", addr, v, want);
    CHECK(g_inter.cpu_mem_stall_cycles == stall + 3, "load32 %08X stall +%u", addr,
          g_inter.cpu_mem_stall_cycles - stall);
}

static void expect_slow32(uint32_t addr) {
    int slow = g_slow32; uint32_t stall = g_inter.cpu_mem_stall_cycles;
    uint32_t v = cpu_load32(&g_inter, addr);
    CHECK(g_slow32 == slow + 1 && v == 0xDEADBEEFu, "load32 %08X did not fall back", addr);
    CHECK(g_inter.cpu_mem_stall_cycles == stall, "load32 %08X charged on the fast path", addr);
}

int main(void) {
    g_inter.ram = &g_ram;
    for (uint32_t i = 0; i < RAM_SIZE; i++) g_ram.data[i] = (uint8_t)(i * 7u + (i >> 9));

    /* --- bus_mask_region is mask_region --- */
    for (uint64_t a = 0; a <= 0xFFFFFFFFull; a += 0x00F0F0F1ull) {
        uint32_t addr = (uint32_t)a;
        CHECK(bus_mask_region(addr) == (addr & REGION_MASK[(addr >> 29) & 7]), "mask %08X", addr);
    }

    /* --- RAM, all three segments and the mirrors --- */
    expect_fast32(0x00001000u, 0x1000u);
    expect_fast32(0x80001000u, 0x1000u);
    expect_fast32(0xA0001000u, 0x1000u);
    expect_fast32(0x00201000u, 0x1000u);   /* 2 MB mirror */
    expect_fast32(0x807FFFFCu, 0x1FFFFCu); /* last word of the 8 MB window */

    /* --- everything that is not a plain RAM read goes to the bus --- */
    expect_slow32(0x00800000u);            /* past the RAM window */
    expect_slow32(0x1F800000u);            /* scratchpad */
    expect_slow32(0x1F801070u);            /* I_STAT */
    expect_slow32(0xBFC00000u);            /* BIOS ROM */
    expect_slow32(0xFFFE0130u);            /* cache control, KSEG2 */
    expect_slow32(0x80001002u);            /* misaligned: the bus raises the error */

    /* halfword and byte: alignment rules of their own */
    {
        int slow = g_slow16;
        uint16_t v = cpu_load16(&g_inter, 0x80000102u);
        CHECK(g_slow16 == slow && v == (uint16_t)(g_ram.data[0x102] | (g_ram.data[0x103] << 8)), "load16");
        cpu_load16(&g_inter, 0x80000103u);
        CHECK(g_slow16 == slow + 1, "odd load16 did not fall back");
        slow = g_slow8;
        uint8_t b = cpu_load8(&g_inter, 0xA0000103u);
        CHECK(g_slow8 == slow && b == g_ram.data[0x103], "load8 at an odd address is still RAM");
        cpu_load8(&g_inter, 0x1F800003u);
        CHECK(g_slow8 == slow + 1, "scratchpad load8 did not fall back");
    }

    /* --- an armed read watchpoint sends every load to the bus --- */
    g_inter.debugger.read_watchpoint_count = 1;
    expect_slow32(0x80001000u);
    g_inter.debugger.read_watchpoint_count = 0;
    /* a write watchpoint does not concern loads */
    g_inter.debugger.write_watchpoint_count = 1;
    expect_fast32(0x80001000u, 0x1000u);
    g_inter.debugger.write_watchpoint_count = 0;

    /* --- the stall follows ZS1_RAM_LOAD_STALL --- */
    g_bus_ram_load_stall = 0;
    {
        uint32_t stall = g_inter.cpu_mem_stall_cycles;
        (void)cpu_load32(&g_inter, 0x80000000u);
        CHECK(g_inter.cpu_mem_stall_cycles == stall, "stall 0 still charged");
    }
    g_bus_ram_load_stall = 3;

    printf("cpu_mem_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
