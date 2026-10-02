/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 ZioZoni95
 *
 * Part of ZoniStation One, a PlayStation 1 emulator.
 * See LICENSE for the full licence text and THIRD-PARTY.md for the
 * components of this project that have other authors.
 */
#include "dma.h"
#include <stdio.h> // For fprintf, stderr
#include "log.h"
#include "lua_debug.h"
#include "event_scheduler.h" // For eventq_schedule
#include "interconnect.h"   // For Interconnect struct (needed for event system)


// Helper function to get channel control register value
// REMOVED 'static'
uint32_t channel_get_control(DmaChannel* ch) {
    uint32_t r = 0;
    r |= (uint32_t)ch->direction;      // Bit 0
    r |= ((uint32_t)ch->step << 1);    // Bit 1
    r |= ((uint32_t)ch->chopping << 8);          // Bit 8
    r |= ((uint32_t)ch->sync << 9);              // Bits 9-10
    r |= ((uint32_t)ch->chopping_dma_sz << 16);  // Bits 16-18
    r |= ((uint32_t)ch->chopping_cpu_sz << 20);  // Bits 20-22
    r |= ((uint32_t)ch->enable << 24); // Bit 24
    r |= ((uint32_t)ch->trigger << 28);// Bit 28
    // r |= ((uint32_t)ch->chcr_unknown_rw << 29); // Bits 29-30 - Not implemented
    return r;
}

/* Drop whatever sliced transfer channel_index still has in flight. The slice
 * state (address, remaining words, step) lives here in Dma rather than in the
 * channel registers, so anything that stops a channel outside of its own
 * completion path has to clear it explicitly. */
void dma_cancel_slice(Dma* dma, uint32_t channel_index) {
    switch (channel_index) {
        case 0: dma->mdec_in_active  = false; break;
        case 1: dma->mdec_out_active = false; break;
        case 2: dma->gpu_ll_active   = false;
                dma->gpu_req_active  = false; break;
        default: return;
    }
    LOG_DMA_DEBUG("[DMA] ch%u sliced transfer cancelled by CHCR write", channel_index);
}

/* MADR and BCR are not fixed for the length of a transfer. "In SyncMode=1 and
 * SyncMode=2, the hardware does update MADR (it will contain the start address
 * of the currently transferred block; at transfer end, it'll hold the
 * end-address in SyncMode=1, or the end marker in SyncMode=2)"; in SyncMode 0
 * it does not, "unless Chopping is enabled" (psx-spx system/dmachannels.md:
 * 23-29). "SyncMode=1 decrements BA to zero, SyncMode=0 with chopping enabled
 * decrements BC to zero" (:56-58).
 *
 * They used to keep the values the guest wrote, so a player that queues the
 * next block by setting only CHCR again, relying on MADR having moved on,
 * re-sent the previous data. Bits 0-1 of MADR are kept as written (:30-32). */
void dma_channel_progress(DmaChannel* ch, uint32_t next_addr, uint32_t remaining) {
    switch (ch->sync) {
        case REQUEST: {
            const uint64_t bs = ch->block_size ? ch->block_size : 0x10000u;
            const uint64_t blocks_left = ((uint64_t)remaining + bs - 1u) / bs;
            /* Words of the current block already moved. */
            const uint32_t done = (uint32_t)(blocks_left * bs - remaining);
            const int32_t  step = (ch->step == INCREMENT) ? 4 : -4;
            const uint32_t madr = (uint32_t)((int32_t)next_addr - step * (int32_t)done);
            ch->base_addr   = (madr & 0x00FFFFFCu) | (ch->base_addr & 3u);
            ch->block_count = (uint16_t)blocks_left;   /* 10000h blocks reads as 0, as written */
            break;
        }
        case MANUAL:
            if (!ch->chopping) break;
            ch->base_addr  = (next_addr & 0x00FFFFFCu) | (ch->base_addr & 3u);
            ch->block_size = (uint16_t)remaining;
            break;
        default:
            break;
    }
}

/* Channels whose MADR or BCR the guest wrote while their sliced transfer was
 * still running here.
 *
 * Our slices can still be running when the guest believes the transfer is
 * over and sets up the next one: the drain in interconnect_perform_dma exists
 * because FMV players do exactly that. A slice event that fires between the
 * guest's MADR write and its CHCR write would then overwrite the new MADR with
 * the old transfer's progress, and the next transfer would start from the
 * wrong place. So once the guest has written either register during a
 * transfer, that transfer stops writing them back; the guest's values win.
 * Not part of the saved state: it only matters inside one transfer, and the
 * next start clears it. */
static uint8_t s_guest_regs_during_transfer = 0;

static bool dma_slice_running(const Dma* dma, uint32_t channel) {
    switch (channel) {
        case 0:  return dma->mdec_in_active;
        case 1:  return dma->mdec_out_active;
        case 2:  return dma->gpu_ll_active || dma->gpu_req_active;
        default: return false;
    }
}

void dma_writeback_begin(uint32_t channel) {
    if (channel < 7) s_guest_regs_during_transfer &= (uint8_t)~(1u << channel);
}

void dma_writeback(Dma* dma, uint32_t channel, uint32_t next_addr, uint32_t remaining) {
    if (channel >= 7 || (s_guest_regs_during_transfer & (1u << channel))) return;
    dma_channel_progress(&dma->channels[channel], next_addr, remaining);
}

void dma_writeback_list(Dma* dma, uint32_t channel, uint32_t madr) {
    if (channel >= 7 || (s_guest_regs_during_transfer & (1u << channel))) return;
    dma->channels[channel].base_addr = madr & 0x00FFFFFFu;
}

// Helper function to set channel control register value
// REMOVED 'static'
void channel_set_control(DmaChannel* ch, uint32_t value) {
    ch->direction = (value & 1) ? FROM_RAM : TO_RAM;
    ch->step = ((value >> 1) & 1) ? DECREMENT : INCREMENT;
    ch->chopping = (value >> 8) & 1;
    switch ((value >> 9) & 3) {
        case 0: ch->sync = MANUAL; break;
        case 1: ch->sync = REQUEST; break;
        case 2: ch->sync = LINKED_LIST; break;
        default:
            LOG_DMA_WARN("[DMA] Invalid DMA Sync mode %d written to CHCR", (value >> 9) & 3);
            break;
    }
    ch->chopping_dma_sz = (value >> 16) & 7;
    ch->chopping_cpu_sz = (value >> 20) & 7;
    ch->enable = (value >> 24) & 1;
    ch->trigger = (value >> 28) & 1;
    // ch->chcr_unknown_rw = (value >> 29) & 3; // Not implemented
}

// Checks if a channel should start transferring based on its state.
bool dma_channel_is_active(DmaChannel* ch) {
    if (!ch->enable) {
        return false;
    }
    if (ch->sync == MANUAL) {
        return ch->trigger;
    }
    return true;
}

// Marks a channel as finished after a transfer.
void dma_channel_done(DmaChannel* ch) {
    ch->enable = false;
    ch->trigger = false;
}

/* An estimate_dma_cycles() helper used to sit here — "2 cycles per word (tune as
 * needed)" — called by nobody since it was written. Removed rather than silenced.
 * DMA is paced by the event scheduler through EVQ slices, not by a per-transfer
 * cycle estimate, so reviving this would mean deciding it is the right model
 * first; an uncalled placeholder only makes it look like that decision was
 * already taken. The real gap here is recorded in GAP_ANALYSIS §4. */

/* Recompute DICR's master flag and drive the DMA interrupt LINE from it.
 *
 * This is the single place allowed to touch the IRQ3 line: recompute the
 * DICR master flag, then set the line from it.
 *
 * Why the line, and not just I_STAT: interconnect_set_irq_line only latches
 * I_STAT on a low->high edge. Completion sites used to poke the line high
 * directly while the DICR acknowledge path cleared only irq_status — leaving
 * irq_line_state stuck high, so every later completion was swallowed with no
 * edge and its interrupt was lost forever. Driving the line down whenever the
 * master flag clears is what makes the next completion a fresh edge. */
void dma_update_irq(Dma* dma) {
    const bool prev = dma->master_irq_flag;
    /* DICR.31 = DICR.15 OR (DICR.23 AND any of DICR.24-30)
     * (dmachannels.md:135-143). The per-channel enables in bits 16-22 decide
     * whether a completion is allowed to SET a flag — they do not take part in
     * this calculation, and "once a flag bit is set, it contributes to the
     * master flag regardless of whether the channel enable is still on". We used
     * to AND the flags with the enables here, so a game that disabled a channel
     * before acknowledging lost the interrupt. */
    dma->master_irq_flag = dma->force_irq ||
        (dma->master_irq_enable && dma->channel_irq_flags != 0);

    if (!dma->inter) return;

    /* Act on the TRANSITION only. Re-asserting a line that is already logically
     * high would restart the interrupt each time this runs, and since
     * hw_irq_write clears irq_line_state when the CPU acknowledges I_STAT (so a
     * device can re-fire), that would trap the CPU in the handler forever —
     * the failure the old "re-raise on DICR write" hack produced. */
    if (!prev && dma->master_irq_flag) {
        interconnect_set_irq_line(dma->inter, IRQ_DMA, true);
    } else if (prev && !dma->master_irq_flag) {
        /* Drop the line so the next completion is a fresh edge — but leave
         * I_STAT alone. An I_STAT bit is cleared by writing 0 to I_STAT and by
         * nothing else (interrupts.md:4-5, :26-31); clearing it here threw away
         * a pending IRQ3 whenever the guest acknowledged DICR first. */
        interconnect_set_irq_line(dma->inter, IRQ_DMA, false);
    }
}

/* DICR.15, the bus-error flag: raised when a transfer reaches an address
 * outside RAM, and it forces the master flag (dmachannels.md:126, :186-192).
 * Games that end a linked list by setting the high bit of the next-address
 * field rely on this path, and it is also the machine's own alarm for the
 * runaway-transfer class of bug. */
void dma_flag_bus_error(Dma* dma) {
    if (dma->force_irq) return;      /* already latched */
    dma->force_irq = true;
    LOG_DMA_WARN("[DMA] Bus error: transfer left RAM — DICR.15 set");
    dma_update_irq(dma);
}

// Initializes the DMA state to reset values.
void dma_init(Dma* dma, struct Interconnect* inter) {
dma->inter = inter; // Store pointer to Interconnect
    // DPCR reset value
    dma->control = 0x07654321;

    // Initialize DICR fields
    dma->force_irq = false;
    dma->channel_irq_enable = 0;
    dma->master_irq_enable = false;
    dma->channel_irq_flags = 0;
    dma->master_irq_flag = false;
    dma->dicr_unknown_rw = 0;

    // Initialize all 7 channels to default values
    for (int i = 0; i < 7; ++i) {
        dma->channels[i].enable = false;
        dma->channels[i].direction = TO_RAM;
        dma->channels[i].step = INCREMENT;
        dma->channels[i].sync = MANUAL;
        dma->channels[i].trigger = false;
        dma->channels[i].base_addr = 0;
        dma->channels[i].block_size = 0;
        dma->channels[i].block_count = 0;
    }

    dma->gpu_ll_addr       = 0;
    dma->gpu_ll_active     = false;
    dma->gpu_req_addr      = 0;
    dma->gpu_req_remaining = 0;
    dma->gpu_req_step      = 4;
    dma->gpu_req_active    = false;

    dma->mdec_in_addr      = 0;
    dma->mdec_in_remaining = 0;
    dma->mdec_in_step      = 4;
    dma->mdec_in_active    = false;
    dma->mdec_out_addr      = 0;
    dma->mdec_out_remaining = 0;
    dma->mdec_out_step      = 4;
    dma->mdec_out_active    = false;
    s_guest_regs_during_transfer = 0;

    LOG_DMA_INFO("[DMA] DMA Initialized. DPCR=0x%08x, Channels initialized.", dma->control);
}

// Reads a 32-bit value from a DMA register address (relative offset).
uint32_t dma_read(Dma* dma, uint32_t offset) {
    uint32_t channel_index = (offset >> 4) & 0x7;
    uint32_t register_offset = offset & 0xF;

    if (channel_index < 7) { // Channel Register Access
        DmaChannel* ch = &dma->channels[channel_index];
        switch (register_offset) {
            case 0x0: // MADR
                return ch->base_addr;
            case 0x4: // BCR
                return ((uint32_t)ch->block_count << 16) | (uint32_t)ch->block_size;
            case 0x8: // CHCR
                return channel_get_control(ch);
            default:
                LOG_DMA_WARN("[DMA] Unhandled DMA Channel read at offset 0x%x (Channel %d, Reg %x)", offset, channel_index, register_offset);
                return 0;
        }
    } else { // Main DMA Register Access
        switch (offset) {
            case 0x70: // DPCR
                return dma->control;
            case 0x74: // DICR
                {
                    uint32_t dicr = 0;
                    dicr |= (uint32_t)dma->dicr_unknown_rw & 0x3F;
                    dicr |= ((uint32_t)dma->force_irq << 15);
                    dicr |= ((uint32_t)dma->channel_irq_enable << 16);
                    dicr |= ((uint32_t)dma->master_irq_enable << 23);
                    dicr |= ((uint32_t)dma->channel_irq_flags << 24);
                    // Master IRQ flag (bit 31)
                    dicr |= ((uint32_t)dma->master_irq_flag << 31);
                    return dicr;
                }
            default:
                LOG_DMA_ERROR("[DMA] Error: Unhandled DMA Main register read at offset 0x%x", offset);
                return 0;
        }
    }
}

// Writes a 32-bit value to a DMA register address (relative offset).
// Returns true if the write made a channel active, false otherwise.
bool dma_write(Dma* dma, uint32_t offset, uint32_t value) {
    uint32_t channel_index = (offset >> 4) & 0x7;
    uint32_t register_offset = offset & 0xF;
    bool channel_became_active = false;

    if (channel_index < 7) { // Channel Register Access
        DmaChannel* ch = &dma->channels[channel_index];
        switch (register_offset) {
            case 0x0: // MADR
                ch->base_addr = value & 0x00FFFFFF;
                if (dma_slice_running(dma, channel_index))
                    s_guest_regs_during_transfer |= (uint8_t)(1u << channel_index);
                break;
            case 0x4: // BCR
                ch->block_size = (uint16_t)(value & 0xFFFF);
                ch->block_count = (uint16_t)(value >> 16);
                if (dma_slice_running(dma, channel_index))
                    s_guest_regs_during_transfer |= (uint8_t)(1u << channel_index);
                break;
            case 0x8: // CHCR
                channel_set_control(ch, value);
                channel_became_active = dma_channel_is_active(ch);
                /* Start/busy bit cleared: software aborted the transfer. Our
                 * sliced channels keep their remaining word count in Dma, not
                 * in the channel registers, so clearing CHCR has to cancel that
                 * state too — otherwise the slice keeps running off the event
                 * scheduler after the guest has moved on. libmdec kicks DMA1
                 * with an oversized BCR and clears CHCR when the frame is out;
                 * the zombie slice then wrote MDEC output across the rest of
                 * RAM (0x126000..0x200000 in Monsters & Co.), smearing the
                 * game's code and display list, and every later ch1 kick was
                 * dropped as "already in flight" so DecDCToutSync never
                 * completed ("time out in decoding !"). */
                if (!channel_became_active) dma_cancel_slice(dma, channel_index);
                if (channel_became_active) {
                    static const char* const sync_names[] = {"MANUAL","REQUEST","LINKED_LIST","?"};
                    LOG_DMA_DEBUG("[DMA] Channel %u activated: sync=%s blockSize=%u blockCount=%u addr=0x%08x",
                                  channel_index,
                                  sync_names[ch->sync < 3 ? ch->sync : 3],
                                  ch->block_size, ch->block_count, ch->base_addr);
                }
                break;
            default:
                if (log_get_level() >= LOG_LEVEL_WARN) {
                    LOG_DMA_WARN("[DMA] Unhandled DMA Channel write at offset 0x%x = 0x%08x (Channel %d, Reg %x)", offset, value, channel_index, register_offset);
                }
                break;
        }
    } else { // Main DMA Register Access
         switch (offset) {
            case 0x70: // DPCR
                dma->control = value;
                break;
            case 0x74: // DICR
                dma->dicr_unknown_rw = (uint8_t)(value & 0x3F);
                dma->force_irq = (value >> 15) & 1;
                dma->channel_irq_enable = (uint8_t)((value >> 16) & 0x7F);
                dma->master_irq_enable = (value >> 23) & 1;
// --- DMA IRQ acknowledge logic ---
                // The PCSX ReARMed "re-raise on write" hack has been removed: with proper
                // edge-triggered DMA completion IRQ, re-raising here causes infinite loops
                // when the BIOS ACKs the wrong channel (e.g. ack_flags=0x08 for ch3 while
                // ch2 flag is set), trapping the CPU in the interrupt handler forever.
                uint8_t ack_flags = (uint8_t)((value >> 24) & 0x7F);
                if (ack_flags) {
                    LOG_DMA_DEBUG("[DMA] DICR IRQ ack: channels 0x%02x cleared, flags now 0x%02x",
                                  ack_flags, (uint8_t)(dma->channel_irq_flags & ~ack_flags));
                    dma->channel_irq_flags &= ~ack_flags;
                }
                /* Always re-evaluate: besides acknowledging, this write may have
                 * just ENABLED a channel whose transfer already finished — games
                 * legitimately program DICR after CHCR, so every DICR write
                 * re-evaluates the master flag. */
                dma_update_irq(dma);
                break;
            default:
                LOG_DMA_ERROR("[DMA] Error: Unhandled DMA Main register write at offset 0x%x = 0x%08x", offset, value);
                break;
        }
    }

    return channel_became_active;
}