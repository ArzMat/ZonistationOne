/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 ZioZoni95
 *
 * Part of ZoniStation One, a PlayStation 1 emulator.
 * See LICENSE for the full licence text and THIRD-PARTY.md for the
 * components of this project that have other authors.
 */
#include "spu.h"
#include "log.h"
#include <string.h>

/* =========================================================================
 * DMA Transfer
 * ========================================================================= */

void spu_dma_write_halfwords(Spu* spu, struct Interconnect* inter, const uint16_t* data, int count) {
    int mode = (spu->control >> 4) & 0x03;
    if (mode != TRANSFER_DMA_WRITE) {
        LOG_SPU_WARN("[SPU] DMA write but SPUCNT mode=%d (expected 2), proceeding", mode);
    }

    spu->status |= SPU_STATUS_TRANSFER_BUSY;

    for (int i = 0; i < count; i++) {
        if (spu->transfer_addr >= SPU_RAM_SIZE) {
            spu->transfer_addr &= (SPU_RAM_SIZE - 1);
        }
        spu->ram[spu->transfer_addr / 2] = data[i];

        spu_check_irq(spu, inter, spu->transfer_addr);

        spu->transfer_addr += 2;
        if (spu->transfer_addr >= SPU_RAM_SIZE) {
            spu->transfer_addr = 0;
        }
    }

    spu->transfer_addr_reg = (uint16_t)(spu->transfer_addr / 8);
    spu->status &= ~SPU_STATUS_TRANSFER_BUSY;
}

void spu_dma_read_halfwords(Spu* spu, struct Interconnect* inter, uint16_t* data, int count) {
    int mode = (spu->control >> 4) & 0x03;
    if (mode != TRANSFER_DMA_READ) {
        LOG_SPU_WARN("[SPU] DMA read but SPUCNT mode=%d (expected 3), proceeding", mode);
    }

    spu->status |= SPU_STATUS_TRANSFER_BUSY;

    uint16_t last_val = 0;
    for (int i = 0; i < count; i++) {
        if (spu->transfer_addr >= SPU_RAM_SIZE) {
            spu->transfer_addr &= (SPU_RAM_SIZE - 1);
        }

        if (spu->transfer_addr / 2 < (SPU_RAM_SIZE / 2)) {
            last_val = spu->ram[spu->transfer_addr / 2];
        }

        spu_check_irq(spu, inter, spu->transfer_addr);

        data[i] = last_val;

        spu->transfer_addr += 2;
        if (spu->transfer_addr >= SPU_RAM_SIZE) {
            spu->transfer_addr = 0;
        }
    }

    spu->transfer_addr_reg = (uint16_t)(spu->transfer_addr / 8);
    spu->status &= ~SPU_STATUS_TRANSFER_BUSY;
}

bool spu_dma_write_request(Spu* spu) {
    int mode = (spu->control >> 4) & 0x03;
    return mode == TRANSFER_DMA_WRITE;
}

bool spu_dma_read_request(Spu* spu) {
    int mode = (spu->control >> 4) & 0x03;
    return mode == TRANSFER_DMA_READ;
}

/* =========================================================================
 * Manual Transfer
 * ========================================================================= */

void spu_transfer_write(Spu* spu, struct Interconnect* inter, uint16_t value) {
    int mode = (spu->control >> 4) & 0x03;
    if (mode != TRANSFER_MANUAL_WRITE && mode != TRANSFER_DMA_WRITE) return;

    if (spu->transfer_addr >= SPU_RAM_SIZE) {
        spu->transfer_addr &= (SPU_RAM_SIZE - 1);
    }

    static uint32_t xfer_count = 0;
    uint32_t cur_addr = spu->transfer_addr;
    xfer_count++;
    if (xfer_count <= 8 || xfer_count % 256 == 0) {
        LOG_SPU_DEBUG("[SPU] Manual xfer #%u: addr=0x%05X val=0x%04X", xfer_count, cur_addr, value);
    }

    spu->ram[spu->transfer_addr / 2] = value;
    spu_check_irq(spu, inter, spu->transfer_addr);

    spu->transfer_addr += 2;
    if (spu->transfer_addr >= SPU_RAM_SIZE) {
        spu->transfer_addr = 0;
    }

    spu->transfer_addr_reg = (uint16_t)(spu->transfer_addr / 8);
}

/* 1F801DA8h written while the transfer mode is Stop. The FIFO holds 32
 * halfwords ("Data (max 32 halfwords)", soundprocessingunitspu.md:683); the
 * documentation does not say what a 33rd write does, so it is dropped and
 * logged rather than guessed at. */
void spu_manual_fifo_push(Spu* spu, uint16_t value) {
    if (spu->manual_fifo_count >= 32) {
        LOG_SPU_WARN("[SPU] Manual-write FIFO full (32 halfwords), 0x%04X dropped", value);
        return;
    }
    spu->manual_fifo[spu->manual_fifo_count++] = value;
}

/* Manual Write selected with halfwords waiting: they go to SPU RAM at the
 * transfer address, in order, each one an access the IRQ address can trap,
 * exactly as if they had been written in Manual Write mode (:715-721, :852). */
void spu_manual_fifo_flush(Spu* spu, struct Interconnect* inter) {
    uint8_t n = spu->manual_fifo_count;
    spu->manual_fifo_count = 0;
    for (uint8_t i = 0; i < n && i < 32; i++)
        spu_transfer_write(spu, inter, spu->manual_fifo[i]);
}

uint16_t spu_transfer_read(Spu* spu, struct Interconnect* inter) {
    int mode = (spu->control >> 4) & 0x03;
    if (mode != TRANSFER_DMA_READ) return 0;

    if (spu->transfer_addr >= SPU_RAM_SIZE) {
        spu->transfer_addr &= (SPU_RAM_SIZE - 1);
    }

    uint16_t val = spu->ram[spu->transfer_addr / 2];
    spu_check_irq(spu, inter, spu->transfer_addr);

    spu->transfer_addr += 2;
    if (spu->transfer_addr >= SPU_RAM_SIZE) {
        spu->transfer_addr = 0;
    }

    spu->transfer_addr_reg = (uint16_t)(spu->transfer_addr / 8);
    return val;
}
