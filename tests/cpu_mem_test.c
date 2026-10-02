/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 ZioZoni95
 *
 * Part of ZoniStation One, a PlayStation 1 emulator.
 * See LICENSE for the full licence text and THIRD-PARTY.md for the
 * components of this project that have other authors.
 */
/* include/cpu_mem.h: the CPU's RAM fast path and the SWL/SWR helpers.
 *
 * The fast path must take exactly the accesses bus.c would have served as a
 * plain RAM read (aligned, main RAM or a mirror of it, no read watchpoint) and
 * charge exactly the same stall; everything else has to reach the unchanged
 * interconnect_load*() call. The bus is stubbed here so each fallback is
 * counted.
 *
 * SWL/SWR are checked against the table in psx-spx
 * ps1/cpu/cpuspecifications.md:270-282, rebuilt independently from the doc's
 * wording ("transfer upper N bits of Rt to [N*4+0..k]", "lower N bits to
 * [N*4+k..3]", the other bytes intact), for all four alignments and a spread of
 * values, both as a merged word and as the byte/halfword store plan. */
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
bool g_bus_dma_window_open = false;   /* ZS1_DMA_STALL=doc window, bus.c */

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

/* psx-spx's table, as words: byte j of the aligned word after the store. */
static uint32_t doc_swl(uint32_t mem, uint32_t rt, uint32_t k) {
    uint32_t out = mem;
    for (uint32_t j = 0; j <= k; j++) {               /* [N*4+0..k] <- upper (k+1) bytes */
        uint32_t byte = (rt >> (8u * (3u - k + j))) & 0xFFu;
        out = (out & ~(0xFFu << (8u * j))) | (byte << (8u * j));
    }
    return out;
}
static uint32_t doc_swr(uint32_t mem, uint32_t rt, uint32_t k) {
    uint32_t out = mem;
    for (uint32_t j = k; j <= 3; j++) {               /* [N*4+k..3] <- lower (4-k) bytes */
        uint32_t byte = (rt >> (8u * (j - k))) & 0xFFu;
        out = (out & ~(0xFFu << (8u * j))) | (byte << (8u * j));
    }
    return out;
}

static uint32_t apply_plan(uint32_t mem, CpuPartialStore p) {
    uint8_t b[4] = { (uint8_t)mem, (uint8_t)(mem >> 8), (uint8_t)(mem >> 16), (uint8_t)(mem >> 24) };
    for (uint32_t i = 0; i < p.count; i++) {
        CHECK(p.size[i] == 1 || p.size[i] == 2 || p.size[i] == 4, "plan size %u", p.size[i]);
        CHECK(p.off[i] % p.size[i] == 0, "plan store at +%u size %u is not naturally aligned",
              p.off[i], p.size[i]);
        CHECK(p.off[i] + p.size[i] <= 4, "plan store leaves the word");
        for (uint32_t j = 0; j < p.size[i] && p.off[i] + j < 4; j++)
            b[p.off[i] + j] = (uint8_t)(p.value[i] >> (8u * j));
    }
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
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

    /* --- ZS1_DMA_STALL=doc: an open DMA window sends RAM loads to the bus,
     *     where they wait for the transfer (bus.c, dma_doc_cpu_read) --- */
    g_bus_dma_window_open = true;
    expect_slow32(0x80001000u);
    g_bus_dma_window_open = false;
    expect_fast32(0x80001000u, 0x1000u);

    /* --- the stall follows ZS1_RAM_LOAD_STALL --- */
    g_bus_ram_load_stall = 0;
    {
        uint32_t stall = g_inter.cpu_mem_stall_cycles;
        (void)cpu_load32(&g_inter, 0x80000000u);
        CHECK(g_inter.cpu_mem_stall_cycles == stall, "stall 0 still charged");
    }
    g_bus_ram_load_stall = 3;

    /* --- SWL/SWR against the psx-spx table --- */
    {
        /* The doc's own wording, one fixed case per alignment, spelled out. */
        const uint32_t mem = 0x11223344u, rt = 0xAABBCCDDu;
        static const uint32_t want_swl[4] = { 0x112233AAu, 0x1122AABBu, 0x11AABBCCu, 0xAABBCCDDu };
        static const uint32_t want_swr[4] = { 0xAABBCCDDu, 0xBBCCDD44u, 0xCCDD3344u, 0xDD223344u };
        for (uint32_t k = 0; k < 4; k++) {
            CHECK(doc_swl(mem, rt, k) == want_swl[k], "doc_swl +%u", k);
            CHECK(doc_swr(mem, rt, k) == want_swr[k], "doc_swr +%u", k);
            CHECK(cpu_swl_merge(mem, rt, k) == want_swl[k], "swl merge +%u = %08X", k, cpu_swl_merge(mem, rt, k));
            CHECK(cpu_swr_merge(mem, rt, k) == want_swr[k], "swr merge +%u = %08X", k, cpu_swr_merge(mem, rt, k));
            CHECK(apply_plan(mem, cpu_swl_plan(rt, k)) == want_swl[k], "swl plan +%u", k);
            CHECK(apply_plan(mem, cpu_swr_plan(rt, k)) == want_swr[k], "swr plan +%u", k);
        }
        /* And a spread of values: merge, plan and table must agree. */
        uint32_t x = 0x12345678u;
        for (int n = 0; n < 2000; n++) {
            x = x * 1664525u + 1013904223u; uint32_t m = x;
            x = x * 1664525u + 1013904223u; uint32_t r = x;
            for (uint32_t k = 0; k < 4; k++) {
                uint32_t wl = doc_swl(m, r, k), wr = doc_swr(m, r, k);
                if (cpu_swl_merge(m, r, k) != wl || apply_plan(m, cpu_swl_plan(r, k)) != wl ||
                    cpu_swr_merge(m, r, k) != wr || apply_plan(m, cpu_swr_plan(r, k)) != wr) {
                    failures++;
                    printf("FAIL mem %08X rt %08X +%u\n", m, r, k);
                }
                checks++;
            }
        }
        /* A store plan writes exactly the bytes the table names: a full-word
         * case is one store, the others never touch the bytes they keep. */
        CHECK(cpu_swl_plan(rt, 3).count == 1 && cpu_swl_plan(rt, 3).size[0] == 4, "swl +3 is one word store");
        CHECK(cpu_swr_plan(rt, 0).count == 1 && cpu_swr_plan(rt, 0).size[0] == 4, "swr +0 is one word store");
    }

    printf("cpu_mem_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
