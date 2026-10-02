/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 ZioZoni95
 *
 * Part of ZoniStation One, a PlayStation 1 emulator.
 * See LICENSE for the full licence text and THIRD-PARTY.md for the
 * components of this project that have other authors.
 */
#include "interconnect.h"
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include "log.h"
#include "dma.h"
#include "gpu.h"
#include "ram.h"
#include "cpu.h"
#include "mdec.h"
#include "debugger.h"
#include "lua_debug.h"
#include "frame_events.h"

// --- Memory Region Masking ---
const uint32_t REGION_MASK[8] = {
    0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, // KUSEG (no mask)
    0x7fffffff,                                      // KSEG0 (strip bit 31)
    0x1fffffff,                                      // KSEG1 (strip bits 31-29)
    0xffffffff, 0xffffffff                           // KSEG2 (no mask)
};

uint32_t mask_region(uint32_t addr) {
    return addr & REGION_MASK[(addr >> 29) & 7];
}

// --- Forward declarations ---
static void interconnect_perform_dma(Interconnect* inter, uint32_t channel_index);
static uint32_t dma_get_transfer_size_words(DmaChannel* ch);
static bool dma_slice_in_flight(const Dma* dma, uint32_t channel_index);

// =============================================================================
// HW DISPATCH TABLE  (indexed by 16-byte block)
//
// Hardware registers occupy 0x1F801000-0x1F801FFF (4 KB).
// We index by (phys >> 4) & 0xFF — one entry per 16-byte block (256 entries).
// Each region gets its own read/write handler that handles all access sizes.
// =============================================================================

typedef enum { BUS_BYTE = 1, BUS_HWORD = 2, BUS_WORD = 4 } BusSize;
typedef uint32_t (*HWReadFn) (Interconnect*, uint32_t addr, BusSize);
typedef void     (*HWWriteFn)(Interconnect*, uint32_t addr, uint32_t val, BusSize);

static HWReadFn  g_hw_read[256];
static HWWriteFn g_hw_write[256];

// ---------------------------------------------------------------------------
// Per-region handler prototypes
// ---------------------------------------------------------------------------
static uint32_t hw_memctrl_read  (Interconnect*, uint32_t, BusSize);
static void     hw_memctrl_write (Interconnect*, uint32_t, uint32_t, BusSize);
static uint32_t hw_sio_read      (Interconnect*, uint32_t, BusSize);
static void     hw_sio_write     (Interconnect*, uint32_t, uint32_t, BusSize);
static uint32_t hw_memctrl2_read (Interconnect*, uint32_t, BusSize);
static void     hw_memctrl2_write(Interconnect*, uint32_t, uint32_t, BusSize);
static uint32_t hw_irq_read      (Interconnect*, uint32_t, BusSize);
static void     hw_irq_write     (Interconnect*, uint32_t, uint32_t, BusSize);
static uint32_t hw_dma_read      (Interconnect*, uint32_t, BusSize);
static void     hw_dma_write     (Interconnect*, uint32_t, uint32_t, BusSize);
static uint32_t hw_timers_read   (Interconnect*, uint32_t, BusSize);
static void     hw_timers_write  (Interconnect*, uint32_t, uint32_t, BusSize);
static uint32_t hw_cdrom_read    (Interconnect*, uint32_t, BusSize);
static void     hw_cdrom_write   (Interconnect*, uint32_t, uint32_t, BusSize);
static uint32_t hw_gpu_read      (Interconnect*, uint32_t, BusSize);
static void     hw_gpu_write     (Interconnect*, uint32_t, uint32_t, BusSize);
static uint32_t hw_mdec_read     (Interconnect*, uint32_t, BusSize);
static void     hw_mdec_write    (Interconnect*, uint32_t, uint32_t, BusSize);
static uint32_t hw_spu_read      (Interconnect*, uint32_t, BusSize);
static void     hw_spu_write     (Interconnect*, uint32_t, uint32_t, BusSize);
static uint32_t hw_unmapped_read (Interconnect*, uint32_t, BusSize);
static void     hw_unmapped_write(Interconnect*, uint32_t, uint32_t, BusSize);

// ---------------------------------------------------------------------------
// Table initialisation
// ---------------------------------------------------------------------------
#define HW_SET(start, end, rfn, wfn) do { \
    for (uint32_t _a = (start); _a < (end); _a += 16) { \
        g_hw_read[(_a >> 4) & 0xFF]  = (rfn); \
        g_hw_write[(_a >> 4) & 0xFF] = (wfn); \
    } \
} while (0)

void bus_hw_tables_init(void) {
    for (int i = 0; i < 256; i++) {
        g_hw_read[i]  = hw_unmapped_read;
        g_hw_write[i] = hw_unmapped_write;
    }
    // MemCtrl  0x1F801000-0x1F80103F  (indices 0x00-0x03)
    HW_SET(0x1F801000, 0x1F801040, hw_memctrl_read,   hw_memctrl_write);
    // PAD/SIO  0x1F801040-0x1F80105F  (indices 0x04-0x05)
    HW_SET(0x1F801040, 0x1F801060, hw_sio_read,       hw_sio_write);
    // RAM_SIZE 0x1F801060-0x1F80106F  (index  0x06)
    HW_SET(0x1F801060, 0x1F801070, hw_memctrl2_read,  hw_memctrl2_write);
    // IRQ      0x1F801070-0x1F80107F  (index  0x07)
    HW_SET(0x1F801070, 0x1F801080, hw_irq_read,       hw_irq_write);
    // DMA      0x1F801080-0x1F8010FF  (indices 0x08-0x0F)
    HW_SET(0x1F801080, 0x1F801100, hw_dma_read,       hw_dma_write);
    // Timers   0x1F801100-0x1F80112F  (indices 0x10-0x12)
    HW_SET(0x1F801100, 0x1F801130, hw_timers_read,    hw_timers_write);
    // CDROM    0x1F801800-0x1F80180F  (index  0x80)
    HW_SET(0x1F801800, 0x1F801810, hw_cdrom_read,     hw_cdrom_write);
    // GPU      0x1F801810-0x1F80181F  (index  0x81)
    HW_SET(0x1F801810, 0x1F801820, hw_gpu_read,       hw_gpu_write);
    // MDEC     0x1F801820-0x1F80182F  (index  0x82)
    HW_SET(0x1F801820, 0x1F801830, hw_mdec_read,      hw_mdec_write);
    // SPU      0x1F801C00-0x1F801E7F  (indices 0xC0-0xE7)
    HW_SET(0x1F801C00, 0x1F801E80, hw_spu_read,       hw_spu_write);
}

// =============================================================================
// PER-REGION HANDLERS
// =============================================================================

// --- MemCtrl (0x1F801000-0x1F80103F) ---
// Real per-region access-delay registers. BIOS/EXP1/CDROM/SPU accesses cost
// several cycles each, not the flat 1 cycle/instruction assumed everywhere else.
//
// The 1ST/SEQ/MIN arithmetic below is the pseudocode published in
// `DOCS/memorycontrol.md:136-145`, transcribed line for line; the register field
// positions come from `:37-53` (delay registers) and `:126-132` (COM_DELAY).
// The documentation's own hedge — "Works (somehow) like so" — applies to the
// model, not to this transcription of it.
static uint32_t calc_memory_timing_word_cycles(uint32_t mem_delay, uint32_t common_delay) {
    int32_t access_time    = (int32_t)((mem_delay >> 4) & 0xF);
    bool use_com0          = (mem_delay >> 8)  & 1;
    bool use_com2          = (mem_delay >> 10) & 1;
    bool use_com3          = (mem_delay >> 11) & 1;
    bool data_bus_16bit    = (mem_delay >> 12) & 1;

    int32_t com0 = (int32_t)(common_delay & 0xF);
    int32_t com2 = (int32_t)((common_delay >> 8)  & 0xF);
    int32_t com3 = (int32_t)((common_delay >> 12) & 0xF);

    int32_t first = 0, seq = 0, min_cycles = 0;
    if (use_com0) { first += com0 - 1; seq += com0 - 1; }
    if (use_com2) { first += com2; seq += com2; }
    if (use_com3) { min_cycles = com3; }
    if (first < 6) first++;
    first = first + access_time + 2;
    seq   = seq   + access_time + 2;
    if (first < min_cycles + 6) first = min_cycles + 6;
    if (seq   < min_cycles + 2) seq   = min_cycles + 2;

    int32_t word_time = data_bus_16bit ? (first + seq) : (first + seq + seq + seq);
    return (uint32_t)(word_time - 1 > 0 ? word_time - 1 : 0);
}

void bus_memctrl_recalculate(Interconnect* inter) {
    inter->bios_access_cycles = calc_memory_timing_word_cycles(inter->memctrl_regs[4], inter->memctrl_regs[8]);
    LOG_INTERCONNECT_DEBUG("[BUS] Memory timing recalculated: BIOS word access = %u extra cycles",
                            inter->bios_access_cycles);
}

void bus_memctrl_init(Interconnect* inter) {
    static const uint32_t defaults[9] = {
        0x1F000000, 0x1F802000, 0x0013243F, 0x00003022,
        0x0013243F, 0x200931E1, 0x00020843, 0x00070777, 0x00031125,
    };
    memcpy(inter->memctrl_regs, defaults, sizeof(defaults));
    bus_memctrl_recalculate(inter);
}

static uint32_t hw_memctrl_read(Interconnect* inter, uint32_t addr, BusSize sz) {
    uint32_t off = addr - 0x1F801000;
    uint32_t idx = off >> 2;
    uint32_t v32 = (idx < 9) ? inter->memctrl_regs[idx] : 0;
    if (sz == BUS_WORD)  return v32;
    if (sz == BUS_HWORD) return (uint16_t)(v32 >> ((off & 2) << 3));
    return (uint8_t)(v32 >> ((off & 3) << 3));
}

static void hw_memctrl_write(Interconnect* inter, uint32_t addr, uint32_t val, BusSize sz) {
    (void)sz;
    if      (addr == 0x1F801000 && val != 0x1F000000)
        LOG_INTERCONNECT_WARN("[BUS] Bad EXP1 base write: 0x%08x", val);
    else if (addr == 0x1F801004 && val != 0x1F802000)
        LOG_INTERCONNECT_WARN("[BUS] Bad EXP2 base write: 0x%08x", val);

    uint32_t idx = (addr - 0x1F801000) >> 2;
    if (idx < 9) {
        inter->memctrl_regs[idx] = val;
        if (idx == 4 || idx == 8) // bios_delay or common_delay
            bus_memctrl_recalculate(inter);
    }
}

// --- PAD/SIO (0x1F801040-0x1F80105F) ---
static uint32_t hw_sio_read(Interconnect* inter, uint32_t addr, BusSize sz) {
    uint32_t off = addr - 0x1F801040;
    if (sz == BUS_WORD)  return sio_read32(&inter->sio, off);
    if (sz == BUS_HWORD) return sio_read16(&inter->sio, off);
    return sio_read8(&inter->sio, off);
}

static void hw_sio_write(Interconnect* inter, uint32_t addr, uint32_t val, BusSize sz) {
    uint32_t off = addr - 0x1F801040;
    if      (sz == BUS_WORD)  sio_write32(&inter->sio, off, val);
    else if (sz == BUS_HWORD) sio_write16(&inter->sio, off, (uint16_t)val);
    else                      sio_write8 (&inter->sio, off, (uint8_t)val);
    if (inter->sio.pending_irq) {
        inter->sio.pending_irq = false;
        interconnect_set_irq_line(inter, IRQ_CTRLMEMCARD, true);
        interconnect_set_irq_line(inter, IRQ_CTRLMEMCARD, false);
    }
}

// --- MemCtrl2 / RAM_SIZE (0x1F801060-0x1F80106F) ---
static uint32_t hw_memctrl2_read(Interconnect* inter, uint32_t addr, BusSize sz) {
    (void)inter; (void)addr; (void)sz;
    return 0x00000B88;
}
static void hw_memctrl2_write(Interconnect* inter, uint32_t addr, uint32_t val, BusSize sz) {
    (void)inter; (void)addr; (void)val; (void)sz;
}

// --- IRQ (0x1F801070-0x1F80107F) ---
static uint32_t hw_irq_read(Interconnect* inter, uint32_t addr, BusSize sz) {
    uint32_t v32 = (addr == IRQ_STATUS_ADDR) ? inter->irq_status : inter->irq_mask;
    if (sz == BUS_WORD)  return v32;
    if (sz == BUS_HWORD) return (uint16_t)v32;
    return (uint8_t)v32;
}

static void hw_irq_write(Interconnect* inter, uint32_t addr, uint32_t val, BusSize sz) {
    /* Same on-die rule as the DMA registers, measured on IMASK itself
     * (partialwordwrites.md:85-95, :121-124): a partial store latches the
     * source word shifted by the byte offset, in full. 16-bit writes to
     * 1F801070h/74h — the conventional way to touch these ports
     * (unpredictablethings.md:51-52) — land unshifted, as before. */
    if (sz != BUS_WORD) {
        val <<= (addr & 3u) << 3;
        addr &= ~3u;
    }
    if (addr == IRQ_STATUS_ADDR) {
        static const char* const names[] = {
            "VBLANK","GPU","CDROM","DMA","TMR0","TMR1","TMR2","PAD","SIO","SPU","IRQ10"
        };
        // PSX I_STAT: write 0 to clear, write 1 to keep.
        // wv = bits to KEEP; bits ~wv are being cleared.
        const uint16_t wv      = (uint16_t)(val & 0x7FF);
        const uint16_t cleared = (uint16_t)(inter->irq_status & ~wv);
        for (uint32_t i = 0; i < 11; i++)
            if (cleared & (1u << i)) LOG_IRQ_DEBUG("%s IRQ cleared", names[i]);
        inter->irq_status    &= wv;
        inter->irq_line_state &= wv;  // allow re-fire: next set_irq_line(true) is fresh edge
        if ((cleared & (1u << IRQ_SPU)) && inter->spu.irq9_flag) {
            inter->spu.irq9_flag  = false;
            inter->spu.status    &= ~SPU_STATUS_IRQ9_FLAG;
            interconnect_set_irq_line(inter, IRQ_SPU, false);
            LOG_IRQ_DEBUG("SPU IRQ9 edge-trigger reset");
        }
        if (inter->cpu) inter->cpu->downcount = 0;
    } else if (addr == IRQ_MASK_ADDR) {
        inter->irq_mask = (uint16_t)(val & 0x7FF);
        LOG_IRQ_DEBUG("I_MASK <- 0x%03x", val & 0x7FF);
        if (inter->cpu) inter->cpu->downcount = 0;
    }
}

// --- DMA (0x1F801080-0x1F8010FF) ---
/* DMA registers are 32-bit, but sub-word access to them is legal and real
 * software uses it — the BIOS shell enables the DMA completion IRQs with a
 * byte write to DICR+2 (0x1F8010F6). Sub-word accesses are serviced through
 * the containing word (PCSX-Redux lists 0x1F8010F0-0x1F8010F7 as
 * byte-addressable register space), so the offset must be word-aligned before
 * it reaches dma_read/dma_write — passing 0x76 straight through fell into
 * their default cases instead. */
static uint32_t hw_dma_read(Interconnect* inter, uint32_t addr, BusSize sz) {
    uint32_t off = addr - DMA_START;
    uint32_t v32 = dma_read(&inter->dma, off & ~3u);
    if (sz == BUS_WORD)  return v32;
    if (sz == BUS_HWORD) return (uint16_t)(v32 >> ((off & 2) << 3));
    return (uint8_t)(v32 >> ((off & 3) << 3));
}

static void hw_dma_write(Interconnect* inter, uint32_t addr, uint32_t val, BusSize sz) {
    uint32_t off = addr - DMA_START;
    if (sz != BUS_WORD) {
        /* On-die MMIO ignores the byte enables. The CPU drives the source word
         * shifted by the byte offset and the decoder latches all 32 bits, with
         * the previous contents contributing nothing — hardware-measured on
         * DPCR (partialwordwrites.md:85-119, summary table :244-261), and the
         * reason `unpredictablethings.md:71-82` tells emulators to treat every
         * access width as carrying 32 bits of data.
         *
         * This used to merge the written lane into the register's current value
         * (RAM semantics), which is the opposite of what the bus does. */
        const uint32_t shift = (off & 3) << 3;
        off &= ~3u;
        val <<= shift;
        LOG_DMA_DEBUG("[DMA] sub-word write to 0x%08x latched as 0x%08x", addr, val);
    }
    bool channel_became_active = dma_write(&inter->dma, off, val);
    if (channel_became_active) {
        uint32_t ch = (off >> 4) & 0x7;
        if ((inter->dma.control >> (ch * 4 + 3)) & 1u) {
            LOG_DMA_DEBUG("[DMA] ch%u activated (DPCR enabled)", ch);
            interconnect_perform_dma(inter, ch);
        } else {
            LOG_DMA_DEBUG("[DMA] ch%u blocked by DPCR", ch);
        }
    } else if (off == 0x70) {
        /* DPCR write: start the channels that were waiting for their Master
         * Enable ("A channel started while its Master Enable is off waits, and
         * begins when the enable bit is set", psx-spx system/dmachannels.md:
         * 121-122). A channel whose sliced transfer is already running is not
         * waiting: it used to be kicked again here, which for GPU DMA drained
         * the transfer and then started it over from MADR, sending it twice. */
        for (uint32_t i = 0; i < 7; i++) {
            if (((inter->dma.control >> (i * 4 + 3)) & 1u) &&
                dma_channel_is_active(&inter->dma.channels[i]) &&
                !dma_slice_in_flight(&inter->dma, i)) {
                LOG_DMA_DEBUG("[DMA] DPCR write: ch%u unblocked", i);
                interconnect_perform_dma(inter, i);
            }
        }
    }
}

// --- Timers (0x1F801100-0x1F80112F) ---
static uint32_t hw_timers_read(Interconnect* inter, uint32_t addr, BusSize sz) {
    int      idx = (int)((addr - 0x1F801100) / 0x10);
    uint32_t off = addr & 0xF;
    if (sz == BUS_WORD)  return timer_read32(&inter->timers_state, idx, off);
    if (sz == BUS_HWORD) return timer_read16(&inter->timers_state, idx, off);
    LOG_INTERCONNECT_WARN("[BUS] Timer byte read 0x%08x", addr);
    return 0xFF;
}
static void hw_timers_write(Interconnect* inter, uint32_t addr, uint32_t val, BusSize sz) {
    int      idx = (int)((addr - 0x1F801100) / 0x10);
    uint32_t off = addr & 0xF;
    if      (sz == BUS_WORD)  { timer_write32(&inter->timers_state, idx, off, val); return; }
    if      (sz == BUS_HWORD) { timer_write16(&inter->timers_state, idx, off, (uint16_t)val); return; }
    LOG_INTERCONNECT_WARN("[BUS] Timer byte write 0x%08x = 0x%02x", addr, val);
}

// --- CDROM (0x1F801800-0x1F80180F) ---
static uint32_t hw_cdrom_read(Interconnect* inter, uint32_t addr, BusSize sz) {
    /* The drive is an 8-bit device and the BIU's auto-increment for it is off by
     * default, so a wider access repeats the SAME register rather than stepping
     * the address: a 32-bit read of 1F801800h returns HSTS four times
     * (cdromdrive.md:315-320), and a 16-bit read of RDDATA yields two
     * consecutive data bytes — the documented way to read a sector with 1024
     * load-halfword opcodes (:118-129).
     *
     * Reading addr+1/+2/+3 instead, as this did, mixed RESULT and HINTSTS into
     * the result and popped the response FIFO as a side effect. */
    if (sz == BUS_BYTE) return cdrom_read8(&inter->cdrom, addr);
    uint32_t v = (uint32_t)cdrom_read8(&inter->cdrom, addr);
    v |= (uint32_t)cdrom_read8(&inter->cdrom, addr) << 8;
    if (sz == BUS_HWORD) return v;
    v |= (uint32_t)cdrom_read8(&inter->cdrom, addr) << 16;
    v |= (uint32_t)cdrom_read8(&inter->cdrom, addr) << 24;
    return v;
}
static void hw_cdrom_write(Interconnect* inter, uint32_t addr, uint32_t val, BusSize sz) {
    if (sz == BUS_BYTE) { cdrom_write8(&inter->cdrom, addr, (uint8_t)val); return; }
    LOG_CDROM_WARN("[CDROM] Write%s at 0x%08x (UNEXPECTED SIZE)",
                   sz == BUS_HWORD ? "16" : "32", addr);
    cdrom_write8(&inter->cdrom, addr, (uint8_t)val);
}

// --- GPU (0x1F801810-0x1F80181F) ---
static uint32_t hw_gpu_read(Interconnect* inter, uint32_t addr, BusSize sz) {
    uint32_t v32 = (addr == GPU_GPUREAD_ADDR) ? gpu_read_data(&inter->gpu)
                                               : gpu_read_status(&inter->gpu);
    if (sz == BUS_WORD)  return v32;
    if (sz == BUS_HWORD) return (uint16_t)(v32 >> ((addr & 2) << 3));
    return (uint8_t)(v32 >> ((addr & 3) << 3));
}
static void hw_gpu_write(Interconnect* inter, uint32_t addr, uint32_t val, BusSize sz) {
    if (sz != BUS_WORD) {
        LOG_GPU_WARN("[GPU] Write%s at 0x%08x (UNEXPECTED SIZE)",
                     sz == BUS_HWORD ? "16" : "8", addr);
        return;
    }
    if (addr == GPU_GP0_ADDR) gpu_gp0(&inter->gpu, val);
    else                      gpu_gp1(&inter->gpu, val);
}

// --- MDEC (0x1F801820-0x1F80182F) ---
static uint32_t hw_mdec_read(Interconnect* inter, uint32_t addr, BusSize sz) {
    /* "I/O ports can be read in 8bit, 16bit, or 32bit units, regardless of
     * their size" (psx-spx system/unpredictablethings.md:58-60): a narrower read
     * returns its part of the 32-bit register. mdec_read() only knows the two
     * word addresses, so 1F801822h and 1F801826h read 0 and an lhu of the
     * status' upper half (busy, DREQ, FIFO flags) always saw "idle". The data
     * port at 1F801822h now pops a word like 1F801820h already did; whether a
     * narrow read of it advances the FIFO on hardware is not documented. */
    uint32_t v32 = mdec_read(&inter->mdec, addr & ~3u);
    if (sz == BUS_WORD)  return v32;
    if (sz == BUS_HWORD) return (uint16_t)(v32 >> ((addr & 2) << 3));
    return (uint8_t)(v32 >> ((addr & 3) << 3));
}
static void hw_mdec_write(Interconnect* inter, uint32_t addr, uint32_t val, BusSize sz) {
    if (sz == BUS_WORD) { mdec_write(&inter->mdec, addr, val); return; }
    LOG_INTERCONNECT_WARN("[BUS] MDEC write%s at 0x%08x",
                          sz == BUS_HWORD ? "16" : "8", addr);
}

// --- SPU (0x1F801C00-0x1F801E7F) ---
static uint32_t hw_spu_read(Interconnect* inter, uint32_t addr, BusSize sz) {
    if (sz == BUS_WORD)  return spu_read32(inter, addr);
    if (sz == BUS_HWORD) return spu_read16(inter, addr);
    return (uint8_t)(spu_read16(inter, addr & ~1u) >> ((addr & 1u) << 3));
}
static void hw_spu_write(Interconnect* inter, uint32_t addr, uint32_t val, BusSize sz) {
    if      (sz == BUS_WORD)  { spu_write32(inter, addr, val); return; }
    if      (sz == BUS_HWORD) { spu_write16(inter, addr, (uint16_t)val); return; }
    spu_write8(inter, addr, (uint8_t)val);
}

// --- Unmapped HW region ---
static uint32_t hw_unmapped_read(Interconnect* inter, uint32_t addr, BusSize sz) {
    (void)inter;
    LOG_INTERCONNECT_WARN("[BUS] Unmapped HW read%d 0x%08x", sz * 8, addr);
    return 0xFFFFFFFF;
}
static void hw_unmapped_write(Interconnect* inter, uint32_t addr, uint32_t val, BusSize sz) {
    (void)inter;
    LOG_INTERCONNECT_WARN("[BUS] Unmapped HW write%d 0x%08x = 0x%x", sz * 8, addr, val);
}

// =============================================================================
// SCRATCHPAD HELPERS  (1 KB fast RAM, KUSEG/KSEG0 only, not KSEG1)
// =============================================================================
static inline uint32_t sp_load32(Interconnect* i, uint32_t off) {
    return (uint32_t)i->scratchpad[off]     | ((uint32_t)i->scratchpad[off+1] << 8)
         | ((uint32_t)i->scratchpad[off+2] << 16) | ((uint32_t)i->scratchpad[off+3] << 24);
}
static inline uint16_t sp_load16(Interconnect* i, uint32_t off) {
    return (uint16_t)i->scratchpad[off] | ((uint16_t)i->scratchpad[off+1] << 8);
}
static inline void sp_store32(Interconnect* i, uint32_t off, uint32_t v) {
    i->scratchpad[off]   = (uint8_t)(v);
    i->scratchpad[off+1] = (uint8_t)(v >> 8);
    i->scratchpad[off+2] = (uint8_t)(v >> 16);
    i->scratchpad[off+3] = (uint8_t)(v >> 24);
}
static inline void sp_store16(Interconnect* i, uint32_t off, uint16_t v) {
    i->scratchpad[off]   = (uint8_t)(v);
    i->scratchpad[off+1] = (uint8_t)(v >> 8);
}

// =============================================================================
// CPU DATA-ACCESS COST
// =============================================================================
//
// The interpreter charged one cycle per instruction regardless of what that
// instruction touched, which made every memory-bound loop run far faster in
// emulated cycles than on hardware. The BIOS kernel's VSync wait is the case
// that made it visible: 12 instructions per iteration, 32768 iterations, so
// 393216 cycles against a PAL frame of 680823 (GPU_GAP_ANALYSIS §4). The loop
// covered 0.58 of a field, its timeout expired before the VBlank it was waiting
// for arrived, and the kernel printed "VSync: timeout" on essentially every
// call — which in turn made every frame-counted animation run fast.
//
// What is documented, and what is not:
//
//   * BIOS ROM and the expansion/SPU/CDROM windows have published access times,
//     computed by calc_memory_timing_word_cycles() from the MEMCTRL delay
//     registers exactly as DOCS/memorycontrol.md:134-147 describes. That value
//     is used here as-is.
//
//   * Main RAM does not. The only figure DOCS publishes for it is RAM_SIZE bit 7,
//     "Delay on simultaneous CODE+DATA fetch from RAM (0=None, 1=One Cycle)"
//     (DOCS/memorycontrol.md:161-163) — one cycle, and only for the contention
//     case. Since the CPU is fetching instructions from RAM whenever it is also
//     loading data from RAM, that bit applies to essentially every data access
//     in RAM-resident code, but one cycle alone does not account for what the
//     R3000A actually pays to reach RAM.
//
// So RAM_DATA_STALL_CALIBRATED below is exactly that: calibrated, not
// transcribed. There is no line in DOCS/ behind it. It was chosen so the VSync
// loop spans more than one field with margin rather than landing on the
// boundary, and it is cross-checked by the cycles-per-instruction figure the
// interconnect now tracks — a plausible CPI for mixed PSX code is the evidence
// that this is a memory cost model and not a fudge sized to one loop.
//
// Scratchpad is the R3000A's data cache wired as fast local memory, so it is
// deliberately free.

/* Loads stall, stores do not.
 *
 * The R3000A retires a store into a write buffer and carries on; the pipeline
 * only stalls if the buffer is full. A load has nowhere to hide — the value is
 * needed, so the CPU waits for it. Charging both the same made the emulated CPU
 * markedly slower than the real one: cycles-per-instruction sat at 1.85, so the
 * guest executed nearly half the instructions per field that it should have, and
 * the games felt sluggish while the host sat at 18% of its frame budget. The
 * emulator was keeping real time perfectly; the machine inside it was not.
 *
 * The VSync loop survives the change with room: its iteration is three loads and
 * one store, so it goes to 12 + 3*3 = 21 cycles, and 32768 * 21 = 688128 still
 * clears the 680823-cycle PAL field. */
#define RAM_LOAD_STALL_DOCUMENTED  1u  /* DOCS/memorycontrol.md:161-163, RAM_SIZE bit 7 */
#define RAM_LOAD_STALL_CALIBRATED  2u  /* no citation — see the note above */
#define RAM_STORE_STALL            0u  /* write buffer absorbs it */

/* Only main RAM is charged, and that is a scoping decision rather than a claim
 * that nothing else costs anything.
 *
 * Scratchpad genuinely is free — it is the R3000A's data cache wired as local
 * memory. BIOS ROM and the I/O window are not, but charging them is a separate
 * change with a known hazard. cpu_icache.c records that applying the MEMCTRL word
 * time (~24-32 cycles) to ROM-resident code hung the CD-ROM command sequence at
 * LBA 23, never root-caused. Charging ROM *data* loads reintroduces exactly that:
 * it was tried here and killed controller input outright, because the BIOS pad
 * routines read their tables out of ROM and every one of those reads suddenly
 * cost thirty cycles. Charging the I/O window has the same shape — the pad is
 * polled in a tight loop over 1F801040h.
 *
 * The VSync measurement only ever justified the RAM figure: that loop is
 * RAM-resident, and its three loads and one store are all RAM. So this models RAM
 * and stops. Adding ROM or I/O is a later change that needs the LBA-23 isolation
 * test run against it on its own.
 *
 * Keeping it to one comparison also keeps it off the profile. This sits on the
 * hottest path in the emulator — interconnect_load32 alone was 4.3% of samples —
 * and an earlier version with a chain of region tests cost about 10% of host
 * frame time on its own. */
#define RAM_LOAD_STALL (RAM_LOAD_STALL_DOCUMENTED + RAM_LOAD_STALL_CALIBRATED)

/* ZS1_RAM_LOAD_STALL overrides the per-load figure for a run.
 *
 * The calibrated part of it has no citation, and the one measurement that pins
 * it — the VSync loop spanning a field — only sets a floor. Everything above
 * that floor is a judgement about how fast the emulated machine should feel,
 * which is not settled from inside the emulator: it needs comparison against
 * hardware or a known-good reference, by someone watching the game.
 *
 * So the value is a run-time knob rather than a rebuild. 0 restores the flat
 * one-cycle-per-instruction behaviour this replaced, which is the honest A/B —
 * VSync then times out again, exactly as it used to. 2 is the documented floor
 * plus nothing. The default stays 3.
 *
 * Read once and cached; the load path is the hottest in the emulator. */
static uint32_t ram_load_stall(void) {
    static int cached = -1;
    if (cached < 0) {
        const char* s = getenv("ZS1_RAM_LOAD_STALL");
        long v = s ? strtol(s, NULL, 10) : (long)RAM_LOAD_STALL;
        if (v < 0)  v = 0;
        if (v > 64) v = 64;      /* absurd values are a typo, not an intent */
        cached = (int)v;
        if (s) LOG_INTERCONNECT_INFO("[BUS] ZS1_RAM_LOAD_STALL=%d extra cycles per RAM load "
                                     "(default %u)", cached, (unsigned)RAM_LOAD_STALL);
    }
    return (uint32_t)cached;
}

/* RAM_SIZE bit 7 gates the documented contention cycle. hw_memctrl2_read returns
 * a fixed 0x00000B88 and hw_memctrl2_write discards writes, so the bit is set for
 * the whole run and the cycle always applies. Read the register here if it ever
 * becomes writable. */
static inline void bus_charge_cpu_load(Interconnect* inter, uint32_t phys) {
    if (phys < 0x00800000)                   /* main RAM, mirrored */
        inter->cpu_mem_stall_cycles += ram_load_stall();
    /* BIOS ROM *data* reads stay free. Tried 2026-08-17 (one extra comparison
     * here, charging inter->bios_access_cycles): it did not reproduce the old
     * "killed controller input" claim — the pad kept polling 32 times a field —
     * but it moved every boot milestone ~15% later without closing the phase it
     * was aimed at, overshooting the first BIOS phase to 143 fields against a
     * reference run's 110. Instruction fetches from ROM are charged, in
     * cpu_icache.c; that is what the reference run agrees with. */
}

static inline void bus_charge_cpu_store(Interconnect* inter, uint32_t phys) {
    if (phys < 0x00800000)
        inter->cpu_mem_stall_cycles += RAM_STORE_STALL;
}

// =============================================================================
// LOAD OPERATIONS
// =============================================================================

uint32_t interconnect_load32(Interconnect* inter, uint32_t address) {
    if (address & 3) {
        LOG_INTERCONNECT_ERROR("[BUS] Unaligned load32 0x%08x", address);
        if (inter->cpu) { inter->cpu->badvaddr = address; cpu_exception(inter->cpu, EXCEPTION_LOAD_ADDRESS_ERROR); }
        return 0;
    }
    debugger_check_read_watchpoint(&inter->debugger, inter->cpu, address, 4);
    const uint32_t phys = mask_region(address);
    bus_charge_cpu_load(inter, phys);

    // RAM (2 MB, mirrored in first 8 MB)
    if (phys < 0x00800000) {
        uint32_t ram_off = phys & (RAM_SIZE - 1);
        uint32_t val = ram_load32(inter->ram, ram_off);
        return val;
    }
    // Scratchpad (KUSEG/KSEG0 only — excluded in KSEG1)
    if (phys >= 0x1F800000 && phys < 0x1F800400 && address < 0xA0000000)
        return sp_load32(inter, phys - 0x1F800000);
    // Expansion 1 (unpopulated)
    if (phys >= 0x1F000000 && phys < 0x1F800000) return 0x00000000;
    // Hardware registers 0x1F801000-0x1F801FFF
    if (phys >= 0x1F801000 && phys < 0x1F802000)
        return g_hw_read[(phys >> 4) & 0xFF](inter, phys, BUS_WORD);
    // Expansion 2 (reads: open bus)
    if (phys >= 0x1F802000 && phys < 0x1F804000) return 0xFFFFFFFF;
    // Expansion 3
    if (phys >= 0x1FA00000 && phys < 0x1FC00000) return 0x00000000;
    // BIOS ROM (512 KB, mirrored up to 4 MB above start)
    if (phys >= 0x1FC00000 && phys < 0x20000000)
        return bios_load32(inter->bios, (phys - 0x1FC00000) & (BIOS_SIZE - 1));
    // Cache Control (KSEG2)
    if (phys == CACHE_CONTROL_ADDR) return 0;
    // Large unmapped windows — return 0 silently
    if (phys >= 0x20000000) return 0;

    LOG_INTERCONNECT_WARN("[BUS] Unmapped load32 0x%08x (phys 0x%08x)", address, phys);
    return 0xFFFFFFFF;
}

uint16_t interconnect_load16(Interconnect* inter, uint32_t address) {
    if (address & 1) {
        LOG_INTERCONNECT_ERROR("[BUS] Unaligned load16 0x%08x", address);
        if (inter->cpu) { inter->cpu->badvaddr = address; cpu_exception(inter->cpu, EXCEPTION_LOAD_ADDRESS_ERROR); }
        return 0;
    }
    debugger_check_read_watchpoint(&inter->debugger, inter->cpu, address, 2);
    const uint32_t phys = mask_region(address);
    bus_charge_cpu_load(inter, phys);

    if (phys < 0x00800000)
        return ram_load16(inter->ram, phys & (RAM_SIZE - 1));
    if (phys >= 0x1F800000 && phys < 0x1F800400 && address < 0xA0000000)
        return sp_load16(inter, phys - 0x1F800000);
    if (phys >= 0x1F000000 && phys < 0x1F800000) return 0x0000;
    if (phys >= 0x1F801000 && phys < 0x1F802000)
        return (uint16_t)g_hw_read[(phys >> 4) & 0xFF](inter, phys, BUS_HWORD);
    if (phys >= 0x1F802000 && phys < 0x1F804000) return 0xFFFF;
    if (phys >= 0x1FA00000 && phys < 0x1FC00000) return 0x0000;
    if (phys >= 0x1FC00000 && phys < 0x20000000)
        return bios_load16(inter->bios, (phys - 0x1FC00000) & (BIOS_SIZE - 1));
    if (phys >= 0x20000000) return 0;

    LOG_INTERCONNECT_WARN("[BUS] Unmapped load16 0x%08x (phys 0x%08x)", address, phys);
    return 0xFFFF;
}

uint8_t interconnect_load8(Interconnect* inter, uint32_t address) {
    debugger_check_read_watchpoint(&inter->debugger, inter->cpu, address, 1);
    const uint32_t phys = mask_region(address);
    bus_charge_cpu_load(inter, phys);

    if (phys < 0x00800000)
        return ram_load8(inter->ram, phys & (RAM_SIZE - 1));
    if (phys >= 0x1F800000 && phys < 0x1F800400 && address < 0xA0000000)
        return inter->scratchpad[phys - 0x1F800000];
    if (phys >= 0x1F000000 && phys < 0x1F800000) return 0x00;
    if (phys >= 0x1F801000 && phys < 0x1F802000)
        return (uint8_t)g_hw_read[(phys >> 4) & 0xFF](inter, phys, BUS_BYTE);
    if (phys >= 0x1F802000 && phys < 0x1F804000) return 0xFF;
    if (phys >= 0x1FA00000 && phys < 0x1FC00000) return 0x00;
    if (phys >= 0x1FC00000 && phys < 0x20000000)
        return inter->bios->data[(phys - 0x1FC00000) & (BIOS_SIZE - 1)];
    if (phys >= 0x20000000) return 0;

    LOG_INTERCONNECT_WARN("[BUS] Unmapped load8 0x%08x (phys 0x%08x)", address, phys);
    return 0xFF;
}

// =============================================================================
// STORE OPERATIONS
// =============================================================================

void interconnect_store32(Interconnect* inter, uint32_t address, uint32_t value) {
    if (address & 3) {
        LOG_INTERCONNECT_ERROR("[BUS] Unaligned store32 0x%08x = 0x%08x", address, value);
        if (inter->cpu) { inter->cpu->badvaddr = address; cpu_exception(inter->cpu, EXCEPTION_STORE_ADDRESS_ERROR); }
        return;
    }
    debugger_check_write_watchpoint(&inter->debugger, inter->cpu, address, 4);
    const uint32_t phys = mask_region(address);
    bus_charge_cpu_store(inter, phys);

    if (phys < 0x00800000) {
        uint32_t ram_off = phys & (RAM_SIZE - 1);
        ram_store32(inter->ram, ram_off, value);
        return;
    }
    if (phys >= 0x1F800000 && phys < 0x1F800400) { sp_store32(inter, phys - 0x1F800000, value); return; }
    if (phys >= 0x1F000000 && phys < 0x1F800000) return; // EXP1
    if (phys >= 0x1F801000 && phys < 0x1F802000) { g_hw_write[(phys>>4)&0xFF](inter, phys, value, BUS_WORD); return; }
    if (phys >= 0x1F802000 && phys < 0x1F804000) return; // EXP2
    if (phys >= 0x1FA00000 && phys < 0x1FC00000) return; // EXP3
    if (phys >= 0x1FC00000 && phys < 0x20000000) { LOG_INTERCONNECT_ERROR("[BUS] Write to BIOS ROM 0x%08x", address); return; }
    if (phys == CACHE_CONTROL_ADDR) return;
    if (phys >= 0x20000000) return; // Large unmapped windows

    LOG_INTERCONNECT_WARN("[BUS] Unhandled store32 0x%08x = 0x%08x", address, value);
}

void interconnect_store16(Interconnect* inter, uint32_t address, uint16_t value) {
    if (address & 1) {
        LOG_INTERCONNECT_ERROR("[BUS] Unaligned store16 0x%08x = 0x%04x", address, value);
        if (inter->cpu) { inter->cpu->badvaddr = address; cpu_exception(inter->cpu, EXCEPTION_STORE_ADDRESS_ERROR); }
        return;
    }
    debugger_check_write_watchpoint(&inter->debugger, inter->cpu, address, 2);
    const uint32_t phys = mask_region(address);
    bus_charge_cpu_store(inter, phys);

    if (phys < 0x00800000) {
        uint32_t ram_off = phys & (RAM_SIZE - 1);
        ram_store16(inter->ram, ram_off, value);
        return;
    }
    if (phys >= 0x1F800000 && phys < 0x1F800400) { sp_store16(inter, phys - 0x1F800000, value); return; }
    if (phys >= 0x1F000000 && phys < 0x1F800000) return;
    if (phys >= 0x1F801000 && phys < 0x1F802000) { g_hw_write[(phys>>4)&0xFF](inter, phys, value, BUS_HWORD); return; }
    if (phys >= 0x1F802000 && phys < 0x1F804000) return; // EXP2 16-bit: ignored
    if (phys >= 0x1FA00000 && phys < 0x1FC00000) return;
    if (phys >= 0x1FC00000 && phys < 0x20000000) { LOG_INTERCONNECT_ERROR("[BUS] Write16 to BIOS ROM 0x%08x", address); return; }
    if (phys >= 0x20000000) return;

    LOG_INTERCONNECT_WARN("[BUS] Unhandled store16 0x%08x = 0x%04x", address, value);
}

void interconnect_store8(Interconnect* inter, uint32_t address, uint8_t value) {
    debugger_check_write_watchpoint(&inter->debugger, inter->cpu, address, 1);
    const uint32_t phys = mask_region(address);
    bus_charge_cpu_store(inter, phys);

    if (phys < 0x00800000) {
        uint32_t ram_off = phys & (RAM_SIZE - 1);
        ram_store8(inter->ram, ram_off, value);
        return;
    }
    if (phys >= 0x1F800000 && phys < 0x1F800400) { inter->scratchpad[phys - 0x1F800000] = value; return; }
    if (phys >= 0x1F000000 && phys < 0x1F800000) return; // EXP1
    if (phys >= 0x1F801000 && phys < 0x1F802000) { g_hw_write[(phys>>4)&0xFF](inter, phys, value, BUS_BYTE); return; }
    // EXP2 (0x1F802000-0x1F803FFF) — only offset 0x23/0x80 is the TTY char
    // port. The rest of the range is POST status codes and other diagnostic
    // registers — capturing writes to those as if they were TTY text corrupts
    // the log with garbage characters.
    if (phys >= 0x1F802000 && phys < 0x1F804000) {
        uint32_t offset = phys - 0x1F802000;
        if (offset == 0x23 || offset == 0x80) {
            interconnect_tty_char(inter, (char)(value & 0xFF), true);
        } else if (offset == 0x41 || offset == 0x42) {
            LOG_BIOS_DEBUG("[BUS] BIOS POST status: 0x%02x", (unsigned)(value & 0x0F));
        } else if (offset == 0x70) {
            LOG_BIOS_DEBUG("[BUS] BIOS POST2 status: 0x%02x", (unsigned)(value & 0x0F));
        }
        return;
    }
    if (phys >= 0x1FA00000 && phys < 0x1FC00000) return;
    if (phys >= 0x1FC00000 && phys < 0x20000000) { LOG_INTERCONNECT_ERROR("[BUS] Write8 to BIOS ROM 0x%08x", address); return; }
    if (phys >= 0x20000000) return;

    LOG_INTERCONNECT_WARN("[BUS] Unhandled store8 0x%08x = 0x%02x", address, value);
}

// =============================================================================
// DMA TRANSFER LOGIC
// =============================================================================

#define DMA_SLICE_WORDS  64    /* GPU commands per slice before yielding */
#define DMA_SLICE_CYCLES 1000  /* EVQ cycles between slices (~30µs) */

/* RAM as the DMA controller sees it: 2 MB mirrored across the first 8 MB, which
 * is the default RAM_SIZE configuration (memorymap.md:123, dmachannels.md:32-34).
 * The bound used to be RAM_SIZE itself, so a transfer to a perfectly legal
 * mirror address was dropped; and walking past the end is a documented bus
 * error (dmachannels.md:126, :186-192), not a silent stop. The load/store path
 * masks the mirror down to physical RAM on its own. */
#define DMA_RAM_LIMIT 0x00800000u

/* MDEC DMA pacing. Real DMA reaches RAM in DRAM hyper-page mode: ~1 cycle per
 * word plus a row-load per 16 (DOCS/dmachannels.md). Pacing MDEC's ch0/ch1
 * slices with the GPU path's flat 64-words-per-1000-cycles instead works out
 * to ~15.6 cycles/word — slow enough that libmdec's DecDCTinSync/
 * DecDCToutSync spin loops give up mid-FMV, which the game reports on the TTY
 * as "MDEC_in_sync timeout:" / "time out in decoding !". The slice size caps
 * MDEC slices low on purpose so an oversized FIFO can't swallow kilobytes at
 * once and starve other interrupts, and the stall between slices is the real
 * cost of the words actually moved, not a fixed quantum. */
#define MDEC_SLICE_WORDS   100  /* slice size while decoding MDEC */
#define MDEC_IDLE_BACKOFF  100  /* DMA halt ticks when the FIFO is full */

static uint32_t dma_ram_ticks(uint32_t words) { return words + (words + 15u) / 16u; }

/* Signal DMA ch2 completion IRQ */
static void dma_ch2_signal_done(Interconnect* inter) {
    DmaChannel* ch = &inter->dma.channels[2];
    dma_channel_done(ch);
    inter->dma.stat_ch2_uploads++;   /* pipeline view: uploads/s (debug only) */
    frame_events_record(FEV_DMA_GPU, ch->block_size);
    /* The transfer cost is charged per slice by dma_gpu_resume() from the words
     * actually moved; there is no extra flat charge here. */

    if (inter->dma.channel_irq_enable & (1u << 2)) {
        inter->dma.channel_irq_flags |= (1u << 2);
        dma_update_irq(&inter->dma);
    }
    lua_debug_notify("dma_ch2_done");
}

/* Run one slice of the GPU DMA transfer. Returns true when fully done, and
 * reports how many words it moved so the caller can charge the real cost. */
static bool dma_gpu_run_slice(Interconnect* inter, uint32_t* words_out) {
    Dma* dma = &inter->dma;
    uint32_t words_local = 0;
    uint32_t* const wc = words_out ? words_out : &words_local;
    *wc = 0;
    #define words_done (*wc)

    if (dma->gpu_ll_active) {
        uint32_t addr = dma->gpu_ll_addr;
        while (words_done < DMA_SLICE_WORDS) {
            if (addr >= DMA_RAM_LIMIT) {
                LOG_DMA_ERROR("[DMA] GPU LL: addr 0x%08x out of bounds", addr);
                dma_flag_bus_error(dma);
                dma->gpu_ll_active = false;
                return true;
            }
            uint32_t header    = interconnect_load32(inter, addr);
            uint32_t num_words = header >> 24;
            uint32_t raw_next  = header & 0x00FFFFFF;
            uint32_t next_addr = raw_next & 0x00FFFFFC;

            for (uint32_t i = 0; i < num_words; i++) {
                addr = (addr + 4) & 0x00FFFFFC;
                if (addr >= DMA_RAM_LIMIT) {
                    dma_flag_bus_error(dma);
                    dma->gpu_ll_active = false;
                    return true;
                }
                gpu_gp0(&inter->gpu, interconnect_load32(inter, addr));
                words_done++;
            }

            /* FFFFFFh is the clean end marker. Any other address past 8 MB also
             * ends the transfer, but as a bus error the guest can see in DICR
             * (dmachannels.md:186-192) — some games end a chain that way. */
            /* gpu_ll_addr keeps the marker that ended the list: MADR holds
             * "the end marker in SyncMode=2" (dmachannels.md:27-29). */
            if (raw_next == 0x00FFFFFFu) {
                LOG_DMA_TRACE("[DMA] GPU LL done after %u words (sliced)", words_done);
                dma->gpu_ll_addr   = raw_next;
                dma->gpu_ll_active = false;
                return true;
            }
            if (next_addr >= DMA_RAM_LIMIT) {
                LOG_DMA_DEBUG("[DMA] GPU LL ended on out-of-range next 0x%06x", raw_next);
                dma_flag_bus_error(dma);
                dma->gpu_ll_addr   = raw_next;
                dma->gpu_ll_active = false;
                return true;
            }
            addr = next_addr;
            words_done++;  /* count header traversal */
        }
        dma->gpu_ll_addr = addr;
        return false;
    }

    if (dma->gpu_req_active) {
        uint32_t addr      = dma->gpu_req_addr;
        uint32_t remaining = dma->gpu_req_remaining;
        while (words_done < DMA_SLICE_WORDS && remaining > 0) {
            uint32_t cur = addr & 0x00FFFFFC;
            if (cur >= DMA_RAM_LIMIT) {
                LOG_DMA_ERROR("[DMA] GPU req: addr 0x%08x out of bounds", cur);
                dma_flag_bus_error(dma);
                dma->gpu_req_active = false;
                return true;
            }
            gpu_gp0(&inter->gpu, interconnect_load32(inter, cur));
            addr = (uint32_t)((int32_t)addr + dma->gpu_req_step);
            remaining--;
            words_done++;
        }
        dma->gpu_req_addr      = addr;
        dma->gpu_req_remaining = remaining;
        if (remaining == 0) {
            dma->gpu_req_active = false;
            return true;
        }
        return false;
    }

    return true;  /* nothing active */
    #undef words_done
}

/* Public: called by evq_handle_dma_gpu each slice tick.
 *
 * The slice used to be paced by a flat DMA_SLICE_CYCLES per DMA_SLICE_WORDS —
 * 1000 cycles for 64 words, ~15.6 clocks a word, against a documented 1 clk/word
 * plus a DRAM row load per 16 (dmachannels.md:194-220). GPU list DMA therefore
 * ran about fifteen times slower than hardware. The MDEC path already charged
 * the real cost; this now uses the same dma_ram_ticks() model. */
void dma_gpu_resume(struct Interconnect* inter) {
    /* ZS1_DMA_GPU_PACE=legacy restores the old flat 1000-cycles-per-64-words
     * quantum. Kept as a one-run A/B because switching to the documented rate
     * hands the guest back a large slice of every field: whatever the CPU used
     * to spend stalled on DMA it now spends executing, which changes how much
     * host time a field costs. */
    static int legacy = -1;
    if (legacy < 0) {
        const char* v = getenv("ZS1_DMA_GPU_PACE");
        legacy = (v && v[0] == 'l') ? 1 : 0;
    }
    const bool was_ll  = inter->dma.gpu_ll_active;
    const bool was_req = inter->dma.gpu_req_active;
    uint32_t words = 0;
    bool done = dma_gpu_run_slice(inter, &words);
    uint32_t ticks = legacy ? DMA_SLICE_CYCLES : (words ? dma_ram_ticks(words) : 1u);
    if (inter->cpu) inter->cpu->downcount -= (int32_t)ticks;

    /* MADR follows the list: the node the next slice starts from, then the end
     * marker (dmachannels.md:27-29). SyncMode 1 through dma_channel_progress. */
    DmaChannel* ch2 = &inter->dma.channels[2];
    if (was_ll)
        ch2->base_addr = inter->dma.gpu_ll_addr & 0x00FFFFFFu;
    else if (was_req)
        dma_channel_progress(ch2, inter->dma.gpu_req_addr,
                             inter->dma.gpu_req_active ? inter->dma.gpu_req_remaining : 0u);
    if (done) {
        dma_ch2_signal_done(inter);
    } else {
        eventq_schedule(inter, EVQ_DMA_GPU, ticks);
        LOG_DMA_TRACE("[DMA] GPU DMA slice: %u words moved (%u cy), %u left",
                      words, ticks,
                      inter->dma.gpu_req_active ? inter->dma.gpu_req_remaining : 0u);
    }
}

/* Signal MDEC ch0 (input) / ch1 (output) completion IRQ. */
static void dma_mdec_signal_done(Interconnect* inter, uint32_t channel) {
    DmaChannel* ch = &inter->dma.channels[channel];
    dma_channel_done(ch);
    if (inter->dma.channel_irq_enable & (1u << channel)) {
        inter->dma.channel_irq_flags |= (1u << channel);
        dma_update_irq(&inter->dma);
    }
    if (channel == 1) lua_debug_notify("mdec_ch1_done");
}

/* Run one slice of MDEC input (ch0) / output (ch1) DMA, each gated on MDEC's
 * own FIFO readiness rather than blasting the whole burst through unconditionally
 * — see the comment on Dma.mdec_in_active in dma.h for why this matters. */
static bool dma_mdec_run_slice(Interconnect* inter, uint32_t* words_moved) {
    Dma* dma = &inter->dma;
    *words_moved = 0;

    if (dma->mdec_in_active) {
        uint32_t addr = dma->mdec_in_addr;
        uint32_t remaining = dma->mdec_in_remaining;
        uint32_t words_done = 0;
        while (words_done < MDEC_SLICE_WORDS && remaining > 0 && mdec_input_has_space(&inter->mdec)) {
            uint32_t cur = addr & 0x00FFFFFC;
            if (cur >= DMA_RAM_LIMIT) {
                LOG_DMA_ERROR("[DMA] MDEC in: addr 0x%08x out of bounds", cur);
                dma_flag_bus_error(dma);
                remaining = 0;
                break;
            }
            mdec_dma_in(&inter->mdec, interconnect_load32(inter, cur));
            addr = (uint32_t)((int32_t)addr + dma->mdec_in_step);
            remaining--;
            words_done++;
        }
        dma->mdec_in_addr = addr;
        dma->mdec_in_remaining = remaining;
        if (remaining == 0) dma->mdec_in_active = false;
        *words_moved += words_done;
    }

    if (dma->mdec_out_active) {
        uint32_t addr = dma->mdec_out_addr;
        uint32_t remaining = dma->mdec_out_remaining;
        uint32_t words_done = 0;
        while (words_done < MDEC_SLICE_WORDS && remaining > 0 && mdec_output_has_data(&inter->mdec)) {
            uint32_t cur = addr & 0x00FFFFFC;
            if (cur >= DMA_RAM_LIMIT) {
                LOG_DMA_ERROR("[DMA] MDEC out: addr 0x%08x out of bounds (base 0x%08x rem %u step %d done %u)",
                              cur, inter->dma.channels[1].base_addr, remaining,
                              dma->mdec_out_step, words_done);
                dma_flag_bus_error(dma);
                remaining = 0;
                break;
            }
            interconnect_store32(inter, cur, mdec_dma_out(&inter->mdec));
            addr = (uint32_t)((int32_t)addr + dma->mdec_out_step);
            remaining--;
            words_done++;
        }
        dma->mdec_out_addr = addr;
        dma->mdec_out_remaining = remaining;
        if (remaining == 0) dma->mdec_out_active = false;
        *words_moved += words_done;

        /* If ch1 is still expecting output but none is ready, decode may be
         * stuck mid-command with buffered-but-undecoded input sitting in the
         * input FIFO (blocked on out_empty, which just became true above but
         * nothing re-entered the state machine to notice). mdec_dma_in/out
         * are the only other callers of mdec_execute(), and if ch0 already
         * finished its own word count this tick, nothing else will ever
         * nudge decode forward again — permanently stalling it. Give it one
         * more try here; safely a no-op if there isn't enough input yet. */
        if (dma->mdec_out_active && !mdec_output_has_data(&inter->mdec)) {
            mdec_execute(&inter->mdec);
        }
    }

    return !dma->mdec_in_active && !dma->mdec_out_active;
}

/* Public: called by evq_handle_mdec each slice tick */
void dma_mdec_resume(struct Interconnect* inter) {
    bool was_in  = inter->dma.mdec_in_active;
    bool was_out = inter->dma.mdec_out_active;
    uint32_t words = 0;
    bool done = dma_mdec_run_slice(inter, &words);
    /* MADR/BCR move with the transfer (dmachannels.md:27-29, :56-58). */
    if (was_in)
        dma_channel_progress(&inter->dma.channels[0], inter->dma.mdec_in_addr,
                             inter->dma.mdec_in_active ? inter->dma.mdec_in_remaining : 0u);
    if (was_out)
        dma_channel_progress(&inter->dma.channels[1], inter->dma.mdec_out_addr,
                             inter->dma.mdec_out_active ? inter->dma.mdec_out_remaining : 0u);
    if (was_in  && !inter->dma.mdec_in_active)  dma_mdec_signal_done(inter, 0);
    if (was_out && !inter->dma.mdec_out_active) dma_mdec_signal_done(inter, 1);
    if (!done) {
        /* Charge the real transfer cost; if the slice moved nothing (MDEC's
         * FIFOs blocked in both directions) back off instead of respinning
         * the event every cycle. */
        uint32_t ticks = words ? dma_ram_ticks(words) : MDEC_IDLE_BACKOFF;
        if (inter->cpu) inter->cpu->downcount -= (int32_t)ticks;
        eventq_schedule(inter, EVQ_MDEC, ticks);
    }
}

static uint32_t dma_get_transfer_size_words(DmaChannel* ch) {
    if (ch->sync == LINKED_LIST) return 0;

    uint32_t bs = (uint32_t)ch->block_size;
    if (ch->sync == MANUAL) {
        return (bs == 0) ? 0x10000 : bs;
    }
    uint32_t bc = (ch->block_count == 0) ? 0x10000u : (uint32_t)ch->block_count;
    bs = (bs == 0) ? 0x10000u : bs;
    return bs * bc;
}

/* True while a sliced, event-scheduled transfer for this channel is still
 * mid-flight (its remaining word count and address live in Dma, not in the
 * channel registers). */
static bool dma_slice_in_flight(const Dma* dma, uint32_t channel_index) {
    switch (channel_index) {
        case 0:  return dma->mdec_in_active;
        case 1:  return dma->mdec_out_active;
        case 2:  return dma->gpu_ll_active || dma->gpu_req_active;
        default: return false;
    }
}

static void interconnect_perform_dma(Interconnect* inter, uint32_t channel_index) {
    if (channel_index >= 7) {
        LOG_DMA_ERROR("[DMA] Invalid channel index %u", channel_index);
        return;
    }

    /* A channel stays enable/busy for the whole of a sliced transfer, so every
     * later kick that inspects "is this channel active?" — a DPCR write
     * unblocking channels, or software re-poking CHCR — would otherwise
     * restart the transfer from base_addr and re-send the entire payload.
     * A resumable transfer advances base_address/block_control as it goes and
     * re-entry continues rather than rewinds; ours resumes off the event
     * scheduler, so a re-kick has to be a no-op. This was corrupting FMV
     * playback: the duplicate GPU ch2 payload arrived with no GP0(0xA0) in
     * front of it, so MDEC pixel words were decoded as GP0 commands. */
    /* A kick that arrives while this channel still has a sliced transfer in
     * flight used to be dropped outright, which silently lost a whole transfer:
     * during FMV playback the movie player kicks the next column upload while a
     * GPU linked list is still draining, and that column stayed black on
     * screen. Real hardware cannot lose the transfer — the channel is busy, so
     * the guest's write lands on a channel that finishes what it is doing.
     * Drain the outstanding slices here, then run the new transfer. */
    if (dma_slice_in_flight(&inter->dma, channel_index)) {
        if (channel_index != 2) {
            /* MDEC's two channels gate on each other's FIFO readiness, so a
             * synchronous drain here deadlocks (it hangs the emulator at the
             * first FMV frame). Leave those kicks dropped until the MDEC path
             * itself can queue them. */
            LOG_DMA_DEBUG("[DMA] ch%u kick ignored — sliced transfer already in flight", channel_index);
            return;
        }
        LOG_DMA_DEBUG("[DMA] ch2 kick while slice in flight — draining first");
        /* No MADR/BCR writeback for the drained transfer: the guest has just
         * written the registers for the new one. */
        uint32_t guard = 0;
        while (!dma_gpu_run_slice(inter, NULL) && ++guard < 65536) { /* drain */ }
        if (guard >= 65536)
            LOG_DMA_ERROR("[DMA] ch2 drain gave up after %u slices", guard);
        dma_ch2_signal_done(inter);
    }

    LOG_DMA_DEBUG("[DMA] ch%d start", channel_index);
    DmaChannel* ch = &inter->dma.channels[channel_index];
    DmaSync sync_mode = ch->sync;
    /* Generic path: where the transfer stopped, for the MADR/BCR writeback. */
    bool     have_progress = false;
    uint32_t moved_words = 0, next_addr = 0;

    LOG_DMA_DEBUG("[DMA] ch%d sync=%d dir=%d base=0x%08x",
                  channel_index, sync_mode, ch->direction, ch->base_addr);

    switch (sync_mode) {
        case LINKED_LIST:
            if (channel_index == 2 && ch->direction == FROM_RAM) {
                LOG_DMA_DEBUG("[DMA] GPU LL start at 0x%08x (sliced)", ch->base_addr & 0x00FFFFFC);
                inter->dma.gpu_ll_addr   = ch->base_addr & 0x00FFFFFC;
                inter->dma.gpu_ll_active = true;
                inter->dma.gpu_req_active = false;
                dma_gpu_resume(inter);
                return;  /* done/IRQ handled by dma_gpu_resume */
            } else {
                LOG_DMA_ERROR("[DMA] Linked list on unsupported ch=%d dir=%d", channel_index, ch->direction);
            }
            break;

        case MANUAL:
        case REQUEST: {
            uint32_t words_to_transfer = dma_get_transfer_size_words(ch);
            if (words_to_transfer == 0) { LOG_DMA_WARN("[DMA] ch%d zero-size", channel_index); break; }

            uint32_t addr = ch->base_addr & 0x00FFFFFC;
            int32_t  step = (ch->step == INCREMENT) ? 4 : -4;

            /* GPU block FROM_RAM: read the source NOW, in one go.
             *
             * This used to be sliced across EVQ ticks like the linked-list
             * path, which sampled guest RAM long after the guest had kicked the
             * transfer. FMV playback exposed it: the movie player owns just two
             * staging buffers and refills one as soon as its transfer is
             * kicked, so a deferred read picked up the NEXT column's pixels and
             * VRAM ended up holding two payloads repeated across all twenty
             * 16-pixel columns — the striping visible on screen. A block
             * transfer must consume the buffer at kick time: the data is
             * read then, only the completion is scheduled later. The linked
             * list below stays sliced: its node chain is built before the
             * kick and is not rewritten under us. */
            if (channel_index == 2 && ch->direction == FROM_RAM) {
                LOG_DMA_DEBUG("[DMA] GPU REQUEST/MANUAL FROM_RAM: %u words", words_to_transfer);
                inter->dma.gpu_ll_active  = false;
                inter->dma.gpu_req_active = false;
                uint32_t cur_addr = addr, moved = 0;
                for (; moved < words_to_transfer; moved++) {
                    uint32_t cur = cur_addr & 0x00FFFFFC;
                    if (cur >= DMA_RAM_LIMIT) {
                        LOG_DMA_ERROR("[DMA] GPU req: addr 0x%08x out of bounds", cur);
                        dma_flag_bus_error(&inter->dma);
                        break;
                    }
                    gpu_gp0(&inter->gpu, interconnect_load32(inter, cur));
                    cur_addr = (uint32_t)((int32_t)cur_addr + step);
                }
                dma_channel_progress(ch, cur_addr, words_to_transfer - moved);
                dma_ch2_signal_done(inter);
                return;
            }

            /* MDEC input (ch0) / output (ch1): slice via EVQ, gated on MDEC's own
             * FIFO readiness — see dma_mdec_run_slice's comment for why blasting
             * the whole burst through synchronously (like the generic loop below)
             * overflows MDEC's input FIFO before ch1 ever gets a chance to drain
             * decoded output. */
            if (channel_index == 0 && ch->direction == FROM_RAM) {
                LOG_DMA_DEBUG("[DMA] MDEC ch0 FROM_RAM: %u words (sliced)", words_to_transfer);
                inter->dma.mdec_in_addr      = addr;
                inter->dma.mdec_in_remaining = words_to_transfer;
                inter->dma.mdec_in_step      = step;
                inter->dma.mdec_in_active    = true;
                dma_mdec_resume(inter);
                return;  /* done/IRQ handled by dma_mdec_resume */
            }
            if (channel_index == 1 && ch->direction == TO_RAM) {
                LOG_DMA_DEBUG("[DMA] MDEC ch1 TO_RAM: %u words (sliced)", words_to_transfer);
                inter->dma.mdec_out_addr      = addr;
                inter->dma.mdec_out_remaining = words_to_transfer;
                inter->dma.mdec_out_step      = step;
                inter->dma.mdec_out_active    = true;
                dma_mdec_resume(inter);
                return;  /* done/IRQ handled by dma_mdec_resume */
            }

            LOG_DMA_DEBUG("[DMA] ch%d %s %s step=%d addr=0x%08x words=%u",
                          channel_index,
                          ch->direction == FROM_RAM ? "FROM_RAM" : "TO_RAM",
                          sync_mode == MANUAL ? "MANUAL" : "REQUEST",
                          step, addr, words_to_transfer);

            uint32_t i = 0;
            for (; i < words_to_transfer; ++i) {
                uint32_t cur = addr & 0x00FFFFFC;
                if (cur >= DMA_RAM_LIMIT) {
                    LOG_DMA_ERROR("[DMA] ch%d addr 0x%08x out of RAM", channel_index, cur);
                    dma_flag_bus_error(&inter->dma);
                    break;
                }
                if (ch->direction == FROM_RAM) {
                    uint32_t data = interconnect_load32(inter, cur);
                    switch (channel_index) {
                        case 4: {
                            uint16_t hw[2] = { (uint16_t)(data & 0xFFFF), (uint16_t)(data >> 16) };
                            spu_dma_write_halfwords(&inter->spu, inter, hw, 2);
                            break;
                        }
                        default:
                            LOG_DMA_DEBUG("[DMA] FROM_RAM ch%d unhandled addr=0x%08x", channel_index, cur);
                            break;
                    }
                } else {
                    uint32_t data = 0;
                    switch (channel_index) {
                        case 6: data = (i == words_to_transfer - 1) ? 0x00FFFFFF : ((addr - 4) & 0x00FFFFFC); break;
                        case 2: data = gpu_read_data(&inter->gpu); break;
                        case 3: data = cdrom_dma_read_word(&inter->cdrom); break;
                        case 4: {
                            uint16_t hw[2];
                            spu_dma_read_halfwords(&inter->spu, inter, hw, 2);
                            data = (uint32_t)hw[0] | ((uint32_t)hw[1] << 16);
                            break;
                        }
                        default:
                            LOG_DMA_DEBUG("[DMA] TO_RAM ch%d unhandled addr=0x%08x", channel_index, cur);
                            break;
                    }
                    interconnect_store32(inter, cur, data);
                }
                addr = (uint32_t)((int32_t)addr + step);
            }
            LOG_DMA_DEBUG("[DMA] ch%d transfer complete: %u words", channel_index, words_to_transfer);
            moved_words   = i;
            next_addr     = addr;
            have_progress = true;
            break;
        }

        default:
            LOG_DMA_ERROR("[DMA] Unknown sync mode %d on ch%d", sync_mode, channel_index);
            break;
    }

    dma_channel_done(ch);
    LOG_DMA_DEBUG("[DMA] ch%d done", channel_index);

    // CPU stall: account for DMA bus contention
    if (inter->cpu) {
        static const uint32_t dev_clks[7] = {1, 1, 1, 40, 4, 20, 1};
        uint32_t rate = dev_clks[channel_index < 7 ? channel_index : 0];
        uint32_t words = (sync_mode == LINKED_LIST)
                       ? ((ch->block_size > 0) ? ch->block_size : 64)
                       : dma_get_transfer_size_words(ch);
        if (words == 0) words = 1;
        uint32_t stall = words * rate + ((words + 15u) / 16u) * 17u;
        inter->cpu->downcount -= (int32_t)stall;
    }

    /* After the stall above, which reads BCR for its word count. */
    if (have_progress)
        dma_channel_progress(ch, next_addr, dma_get_transfer_size_words(ch) - moved_words);

    // DMA completion IRQ (IRQ3)
    if (inter->dma.channel_irq_enable & (1u << channel_index)) {
        inter->dma.channel_irq_flags |= (1u << channel_index);
        dma_update_irq(&inter->dma);
    }
}

