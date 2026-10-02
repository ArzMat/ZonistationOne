/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 ZioZoni95
 *
 * Part of ZoniStation One, a PlayStation 1 emulator.
 * See LICENSE for the full licence text and THIRD-PARTY.md for the
 * components of this project that have other authors.
 */
#include "spu.h"
#include "interconnect.h"
#include "log.h"

/* =========================================================================
 * IRQ9 (SPU RAM IRQ)
 *
 * The IRQ address traps accesses to SPU RAM, and nothing else: a voice reading
 * ADPCM data from it (psx-spx spu/soundprocessingunitspu.md:823-834), a write
 * to one of the four capture buffers (:836-844) and a data transfer (:851-852).
 *
 * Writing the IRQ address or the transfer address is not an access. This file
 * used to compare a freshly written IRQA against every voice's next block and
 * against the transfer address, and fire on the spot. A streaming driver that
 * re-arms IRQA at the half of the ring a voice is about to enter, or right
 * after a DMA that ended exactly there, then took an interrupt that the
 * hardware never raises, refilled the half that was still playing, and the
 * stream jumped ahead.
 *
 * The flag stays up until the game clears SPUCNT.6, which the documentation
 * names as the acknowledge: "IRQ9 Enable (0=Disabled/Acknowledge, 1=Enabled)"
 * (:635), with the flag itself in SPUSTAT.6 (:655). Acknowledging I_STAT does
 * not touch it (psx-spx system/interrupts.md:34-36: the device has to be
 * acknowledged at its own port as well), so while it is up no further edge can
 * reach I_STAT.9.
 * ========================================================================= */

static void spu_raise_irq9(Spu* spu, struct Interconnect* inter, uint32_t address) {
    spu->irq9_flag = true;
    spu->status |= SPU_STATUS_IRQ9_FLAG;
    LOG_SPU_DEBUG("[SPU] IRQ9 triggered at address 0x%05X", address);
    if (inter) interconnect_set_irq_line(inter, IRQ_SPU, true);
}

/* One halfword access at `address` (a transfer or a capture write). */
bool spu_check_irq(Spu* spu, struct Interconnect* inter, uint32_t address) {
    if (!(spu->control & SPU_CTRL_IRQ9_ENABLE)) return false;
    if (spu->irq9_flag) return false;

    uint32_t irq_byte_addr = (uint32_t)spu->irq_addr * 8;
    if (irq_byte_addr != address) return false;
    spu_raise_irq9(spu, inter, address);
    return true;
}

/* A read of `len` bytes starting at `start`, wrapping at the end of SPU RAM:
 * a voice fetching one 16-byte ADPCM block. "Triggers an IRQ when a voice reads
 * ADPCM data from the IRQ address" (:824) - the block being read, wherever in
 * it the address sits. The documentation recommends block-aligned addresses
 * because a mid-block one "doesn't seem to trigger always" (:831-834); taking
 * it every time is the reliable half of that. */
bool spu_check_irq_range(Spu* spu, struct Interconnect* inter, uint32_t start, uint32_t len) {
    if (!(spu->control & SPU_CTRL_IRQ9_ENABLE)) return false;
    if (spu->irq9_flag) return false;

    uint32_t irq_byte_addr = (uint32_t)spu->irq_addr * 8;
    if (((irq_byte_addr - start) & (SPU_RAM_SIZE - 1)) >= len) return false;
    spu_raise_irq9(spu, inter, irq_byte_addr);
    return true;
}

/* SPUCNT.6 = 0: the acknowledge (:635). The SPU's request goes away with the
 * flag, so the line to the interrupt controller drops too, and the next match
 * after the game sets SPUCNT.6 again is a fresh edge. */
void spu_irq_acknowledge(Spu* spu, struct Interconnect* inter) {
    spu->irq9_flag = false;
    spu->status &= (uint16_t)~SPU_STATUS_IRQ9_FLAG;
    if (inter) interconnect_set_irq_line(inter, IRQ_SPU, false);
}
