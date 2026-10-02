/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 ZioZoni95
 * SPDX-FileCopyrightText: 2002 Pete Bernert and the PCSX-Redux authors
 *
 * Part of ZoniStation One, a PlayStation 1 emulator.
 * See LICENSE for the full licence text and THIRD-PARTY.md for the
 * components of this project that have other authors.
 */
#include "spu.h"
#include "interconnect.h"
#include "log.h"
#include <string.h>
#include <stdlib.h>

/* Forward declarations from other modules */
extern void spu_reverb_init(Spu* spu);

/* =========================================================================
 * Helpers
 * ========================================================================= */

static int spu_offset_for_addr(uint32_t addr) {
    if (addr < SPU_START || addr > SPU_END) return -1;
    return (int)(addr - SPU_START);
}


/* =========================================================================
 * Init / Reset
 * ========================================================================= */

void spu_init(Spu* spu) {
    if (!spu) return;
    /* Latch ZS1_SPU_RING_TARGET here, on the emulation thread and before the
     * audio device (and with it the thread that also reads the value) exists. */
    (void)spu_ring_target_samples();
    memset(spu, 0, sizeof(Spu));
    spu_reverb_init(spu);
    spu_stretch_reset(&spu->stretch);
    spu->stretch_tempo = 1.0;
    LOG_SPU_DEBUG("[SPU] SPU initialized (MMIO %08x-%08x, RAM %u bytes)",
                  SPU_START, SPU_END, SPU_RAM_SIZE);
}

void spu_reset(Spu* spu) {
    if (!spu) return;
    uint16_t* ram_copy = (uint16_t*)malloc(SPU_RAM_SIZE);
    memcpy(ram_copy, spu->ram, SPU_RAM_SIZE);
    memset(spu, 0, sizeof(Spu));
    memcpy(spu->ram, ram_copy, SPU_RAM_SIZE);
    free(ram_copy);
    spu_reverb_init(spu);
    spu_stretch_reset(&spu->stretch);
    spu->stretch_tempo = 1.0;
    LOG_SPU_INFO("[SPU] SPU reset (RAM preserved)");
}

/* =========================================================================
 * Key On/Off Processing
 * ========================================================================= */

void spu_process_key_on_off(Spu* spu) {
    /* Runs once per output sample and nearly always finds both latches empty:
     * nothing below does anything then, so skip the two 24-voice scans. */
    if (!(spu->key_on | spu->key_off)) return;

    /* Process key off first */
    for (int v = 0; v < NUM_VOICES; v++) {
        if (spu->key_off & (1u << v)) {
            SpuVoice* voice = &spu->voices[v];
            if (voice->on) {
                voice->stop = true;
                LOG_SPU_TRACE("[SPU] Voice %d Key Off", v);
            }
        }
    }

    /* Process key on, after the pcsx-redux init. Key On copies the start
     * address to the current address (soundprocessingunitspu.md:131) and leaves
     * the repeat address alone: only a Loop Start flag or a write changes it
     * (:133-147). Resetting the loop state here threw away a repeat address the
     * game had written before Key On, which is the usual way to loop a ring
     * that carries no Loop Start flag of its own. */
    for (int v = 0; v < NUM_VOICES; v++) {
        if (spu->key_on & (1u << v)) {
            SpuVoice* voice = &spu->voices[v];
            spu->endx &= ~(1u << v);
            voice->SBPos        = 28;           /* trigger decode on first sample */
            voice->spos         = 0x30000;      /* prime gauss ring with 3 samples */
            voice->s_1          = 0;
            voice->s_2          = 0;
            memset(voice->gauss_ring, 0, sizeof(voice->gauss_ring));
            voice->gpos         = 0;
            voice->reach_end    = false;
            voice->ignore_loop  = false;        /* see the LSAX write */
            voice->blocks_since_kon = 0;
            voice->curr_addr    = (uint32_t)voice->start_address * 8;
            voice->start_addr   = voice->curr_addr;
            voice->on           = true;
            voice->stop         = false;
            voice->endx_mask    = false;
            voice->adsr_state     = ADSR_STATE_ATTACK;
            voice->EnvelopeVol    = 0;
            voice->EnvelopeVolF   = 0;
            voice->adsr_volume    = 0;
            voice->sval           = 0;
            voice->vol_left_count  = 0;
            voice->vol_right_count = 0;
            spu->total_key_on_events++;
            LOG_SPU_DEBUG("[SPU] Voice %d Key On: start=0x%04X pitch=0x%04X volL=0x%04X volR=0x%04X adsr=%04X/%04X",
                         v, voice->start_address, voice->pitch,
                         voice->volume_left, voice->volume_right,
                         voice->adsr_low, voice->adsr_high);
        }
    }

    spu->key_on = 0;
    spu->key_off = 0;
}

/* =========================================================================
 * Control Register
 * ========================================================================= */

void spu_set_control(Spu* spu, struct Interconnect* inter, uint16_t value) {
    uint16_t old = spu->control;
    spu->control = value;

    /* Mute: SPUCNT.14 = 0 silences the voices, not the CD input
     * (soundprocessingunitspu.md:631, "Don't care for CD Audio"); applied in
     * spu_generate_one_sample. */
    spu->muted = !(value & SPU_CTRL_MUTE);

    /* IRQ9 Enable = 0 is also the acknowledge: "(0=Disabled/Acknowledge,
     * 1=Enabled)" (:635). It is the only thing that clears the flag in SPUSTAT.6
     * (:655) and takes the SPU's request off the interrupt line; writing I_STAT
     * does neither (src/core/bus.c, psx-spx system/interrupts.md:34-36). */
    if (!(value & SPU_CTRL_IRQ9_ENABLE))
        spu_irq_acknowledge(spu, inter);

    /* SPU disable: force all voices off */
    if (!(value & SPU_CTRL_ENABLE)) {
        for (int v = 0; v < NUM_VOICES; v++) {
            spu->voices[v].on          = false;
            spu->voices[v].adsr_volume = 0;
        }
    }

    /* SPUSTAT.5-0 mirrors SPUCNT.5-0 in full — not just the transfer mode in
     * bits 5-4 (DOCS/soundprocessingunitspu.md:579). Every documented SPUCNT
     * sequence ends with "wait until it is applied in SPUSTAT" (:566-568,
     * :635-651), so a driver that writes SPUCNT and polls SPUSTAT for the same
     * low bits never sees its write land if bits 3-0 are dropped. Ace Combat 2
     * writes 0xC085 (low bits 05h: CD audio + CD reverb) at the handover to the
     * game engine, and stayed there with main volume at zero.
     * Bit 7 repeats SPUCNT.5 (:577). */
    spu->status = (uint16_t)((spu->status & ~(SPU_STATUS_MODE | SPU_STATUS_DMA_REQUEST))
                             | (value & SPU_STATUS_MODE)
                             | ((value & (1u << 5)) ? SPU_STATUS_DMA_REQUEST : 0u));

    /* Selecting Manual Write is what sends the halfwords queued in the data
     * FIFO while the mode was Stop into SPU RAM (:715-721). */
    if (((value >> 4) & 0x03) == TRANSFER_MANUAL_WRITE && spu->manual_fifo_count)
        spu_manual_fifo_flush(spu, inter);

    if (value != old) {
        LOG_SPU_DEBUG("[SPU] Control=0x%04X (enable=%d, muted=%d, irq=%d, mode=%d)",
                     value,
                     (value & SPU_CTRL_ENABLE) ? 1 : 0,
                     spu->muted ? 1 : 0,
                     (value & SPU_CTRL_IRQ9_ENABLE) ? 1 : 0,
                     (value >> 4) & 0x03);
        if (spu->muted) {
            LOG_SPU_WARN("[SPU] SPU muted (SPUCNT bit14=0)");
        }
    }
}

/* =========================================================================
 * MMIO Read
 * ========================================================================= */

uint16_t spu_read16(struct Interconnect* inter, uint32_t addr) {
    Spu* spu = &inter->spu;
    /* Bring the DSP up to now before reporting anything derived from it:
     * envelope volumes, the transfer address, the status bits. */
    spu_catch_up(inter);
    int off = spu_offset_for_addr(addr);
    if (off < 0) return 0;

    uint32_t reg = (uint32_t)off;

    /* Extended read-only: current envelope volume (0x1E00-0x1E5F) */
    if (reg >= 0x200 && reg < 0x260) {
        int voice = (reg - 0x200) / 4;
        if (voice >= 0 && voice < NUM_VOICES)
            return (uint16_t)spu->voices[voice].EnvelopeVol;
        return 0;
    }

    /* Voice registers (0x000-0x17F) */
    if (reg < 0x180) {
        int voice = (reg >> 4) & 0x1F;
        int sub = reg & 0x0F;
        if (voice < NUM_VOICES) {
            switch (sub) {
                case 0x00: return spu->voices[voice].volume_left;
                case 0x02: return spu->voices[voice].volume_right;
                case 0x04: return spu->voices[voice].pitch;
                case 0x06: return spu->voices[voice].start_address;
                case 0x08: return spu->voices[voice].adsr_low;
                case 0x0A: return spu->voices[voice].adsr_high;
                case 0x0C: return (uint16_t)spu->voices[voice].adsr_volume;
                case 0x0E: return spu->voices[voice].repeat_address;
            }
        }
        return 0;
    }

    /* Control registers */
    switch (reg) {
        case SPU_REG_MVOL_L: return spu->main_vol_left;
        case SPU_REG_MVOL_R: return spu->main_vol_right;
        case SPU_REG_RVOL_L: return (uint16_t)spu->reverb_vol_left;
        case SPU_REG_RVOL_R: return (uint16_t)spu->reverb_vol_right;
        case SPU_REG_KEY_ON_L: return spu->key_on & 0xFFFF;
        case SPU_REG_KEY_ON_H: return (spu->key_on >> 16) & 0xFFFF;
        case SPU_REG_KEY_OFF_L: return spu->key_off & 0xFFFF;
        case SPU_REG_KEY_OFF_H: return (spu->key_off >> 16) & 0xFFFF;
        case SPU_REG_FMOD_L: return spu->pitch_mod & 0xFFFF;
        case SPU_REG_FMOD_H: return (spu->pitch_mod >> 16) & 0xFFFF;
        case SPU_REG_NOISE_L: return spu->noise_mode & 0xFFFF;
        case SPU_REG_NOISE_H: return (spu->noise_mode >> 16) & 0xFFFF;
        case SPU_REG_REVERB_L: return spu->reverb_on & 0xFFFF;
        case SPU_REG_REVERB_H: return (spu->reverb_on >> 16) & 0xFFFF;
        case SPU_REG_ENDX_L: return spu->endx & 0xFFFF;
        case SPU_REG_ENDX_H: return (spu->endx >> 16) & 0xFFFF;
        case SPU_REG_REVERB_BASE: return spu->reverb_base;
        case SPU_REG_IRQ_ADDR: return spu->irq_addr;
        case SPU_REG_TRANSFER_ADDR: return spu->transfer_addr_reg;
        case 0x1AC: return 0x0004; /* SPU_DATA_TRANSFER_CONTROL */
        case SPU_REG_CONTROL: return spu->control;
        case SPU_REG_STATUS: return spu->status;
        case SPU_REG_CD_VOL_L: return (uint16_t)spu->cd_vol_left;
        case SPU_REG_CD_VOL_R: return (uint16_t)spu->cd_vol_right;
        case SPU_REG_EXT_VOL_L: return (uint16_t)spu->ext_vol_left;
        case SPU_REG_EXT_VOL_R: return (uint16_t)spu->ext_vol_right;
        case SPU_REG_MVOL_CUR_L: return (uint16_t)spu->main_vol_left_cur;
        case SPU_REG_MVOL_CUR_R: return (uint16_t)spu->main_vol_right_cur;
        default:
            if (reg >= 0x1C0 && reg < 0x200) {
                return spu->reverb_regs[(reg - 0x1C0) / 2];
            }
            break;
    }

    LOG_SPU_DEBUG("[SPU] Read16 unknown reg 0x%04X", reg);
    return 0;
}

uint32_t spu_read32(struct Interconnect* inter, uint32_t addr) {
    uint16_t lo = spu_read16(inter, addr);
    uint16_t hi = spu_read16(inter, addr + 2);
    return (uint32_t)lo | ((uint32_t)hi << 16);
}

/* =========================================================================
 * MMIO Write
 * ========================================================================= */

static void voice_write_reg(Spu* spu, int voice, int sub, uint16_t value) {
    if (voice < 0 || voice >= NUM_VOICES) return;
    SpuVoice* v = &spu->voices[voice];

    switch (sub) {
        /* Fixed mode (bit15=0) sets the level directly. Sweep mode (bit15=1)
         * describes how to *move* from wherever the level already is — writing
         * the configuration bits into the level, as this used to, starts every
         * sweep from a meaningless volume
         * (DOCS/soundprocessingunitspu.md:379-383). */
        case 0x00:
            v->volume_left = value;
            if (!(value & 0x8000))
                v->vol_left = (int)(int16_t)((value & 0x7FFF) << 1);
            break;
        case 0x02:
            v->volume_right = value;
            if (!(value & 0x8000))
                v->vol_right = (int)(int16_t)((value & 0x7FFF) << 1);
            break;
        case 0x04:
            /* VxPitch holds all 16 bits: "0-15 Sample rate (0=stop, 4000h=fastest,
             * 4001h..FFFFh=usually same as 4000h)" — DOCS/soundprocessingunitspu.md:166.
             *
             * This used to mask the register with 0x3FFF on the way in, which is the
             * limit from the *pitch counter* (:197) applied at the wrong moment. The
             * masked value is not a clamp, it wraps: a game writing 4000h, the
             * documented fastest rate, stored 0 and the voice stopped dead, and 5000h
             * stored 1000h and played at normal speed. The limit belongs where the
             * documentation puts it, in spu_voice_get_sample, once per output sample
             * and after pitch modulation. */
            v->pitch = value;
            break;
        case 0x06:
            v->start_address = value;
            break;
        case 0x08:
            v->adsr_low      = value;
            v->attack_mode_exp = (value >> 15) & 1;
            v->attack_rate     = (value >> 8) & 0x7F;
            v->decay_rate      = (value >> 4) & 0x0F;
            v->sustain_level   = value & 0x0F;
            break;
        case 0x0A:
            v->adsr_high        = value;
            v->sustain_mode_exp = (value >> 15) & 1;
            v->sustain_increase = ((value >> 14) & 1) ? 0 : 1;  /* bit14=0: inc, 1: dec */
            v->sustain_rate     = (int)(value & (0x1f00u | 0xc0u)) >> 6;
            v->release_mode_exp = (value >> 5) & 1;
            v->release_rate     = value & 0x1F;
            break;
        case 0x0C:
            v->adsr_volume = (int16_t)value;
            break;
        case 0x0E:
            /* A write sets the repeat address (soundprocessingunitspu.md:
             * 142-147), "eg. to redirect a one-shot sample ... to a silent-loop
             * located elsewhere in memory". The documentation does not say
             * whether a Loop Start flag read afterwards overrides the write.
             * The reference emulator, with games named in its comments, lets the
             * write win (Loop Start flags are ignored until the next Key On)
             * when the voice is off or already past its first block, and lets
             * the sample's own Loop Start win when the write lands during the
             * first block, the moment a driver sets LSAX right after Key On.
             * That is followed here. Key On clears the latch, so a write made
             * before Key On never suppresses the sample's flags. Before this
             * rule existed every write latched (a redirect worked, a write right
             * after Key On broke the sample's own loop), and for a while no
             * write did (the redirect stopped working); docs/study/
             * SPU_2026-07-29.md finding 11 has the history. */
            v->repeat_address = value;
            if (!v->on || v->blocks_since_kon >= 2)
                v->ignore_loop = true;
            break;
    }
}

void spu_write16(struct Interconnect* inter, uint32_t addr, uint16_t value) {
    Spu* spu = &inter->spu;
    /* Flush before mutate: everything owed up to this cycle is generated with
     * the old register values, so a write cannot retroactively change audio the
     * guest already asked for. */
    spu_catch_up(inter);
    int off = spu_offset_for_addr(addr);
    if (off < 0) return;

    uint32_t reg = (uint32_t)off;

    /* Voice registers (0x000-0x17F) */
    if (reg < 0x180) {
        int voice = (reg >> 4) & 0x1F;
        int sub = reg & 0x0F;
        voice_write_reg(spu, voice, sub, value);
        return;
    }

    /* Control registers */
    switch (reg) {
        case SPU_REG_MVOL_L:
            spu->main_vol_left = value;
            /* Same rule as the voice volumes: only fixed mode sets the level; a
             * sweep moves it from there, once per sample (spu_mixing.c). */
            if (!(value & 0x8000))
                spu->main_vol_left_cur = (int32_t)(int16_t)((value & 0x7FFF) << 1);
            LOG_SPU_DEBUG("[SPU] Main Vol L <- 0x%04X (working=%d)", value, spu->main_vol_left_cur);
            break;
        case SPU_REG_MVOL_R:
            spu->main_vol_right = value;
            if (!(value & 0x8000))
                spu->main_vol_right_cur = (int32_t)(int16_t)((value & 0x7FFF) << 1);
            LOG_SPU_DEBUG("[SPU] Main Vol R <- 0x%04X (working=%d)", value, spu->main_vol_right_cur);
            break;
        case SPU_REG_RVOL_L: spu->reverb_vol_left = (int16_t)value; break;
        case SPU_REG_RVOL_R: spu->reverb_vol_right = (int16_t)value; break;
        case SPU_REG_KEY_ON_L: spu->key_on |= value; break;
        case SPU_REG_KEY_ON_H: spu->key_on |= ((uint32_t)value << 16); break;
        case SPU_REG_KEY_OFF_L: spu->key_off |= value; break;
        case SPU_REG_KEY_OFF_H: spu->key_off |= ((uint32_t)value << 16); break;
        case SPU_REG_FMOD_L: spu->pitch_mod = (spu->pitch_mod & ~0xFFFF) | value; break;
        case SPU_REG_FMOD_H: spu->pitch_mod = (spu->pitch_mod & ~0xFFFF0000) | ((uint32_t)value << 16); break;
        case SPU_REG_NOISE_L: spu->noise_mode = (spu->noise_mode & ~0xFFFF) | value; break;
        case SPU_REG_NOISE_H: spu->noise_mode = (spu->noise_mode & ~0xFFFF0000) | ((uint32_t)value << 16); break;
        case SPU_REG_REVERB_L: spu->reverb_on = (spu->reverb_on & ~0xFFFF) | value; break;
        case SPU_REG_REVERB_H: spu->reverb_on = (spu->reverb_on & ~0xFFFF0000) | ((uint32_t)value << 16); break;
        case SPU_REG_REVERB_BASE:
            /* mBASE is a full 16-bit address divided by 8, covering all 512 KB
             * of SPU RAM; masking it to 14 bits put the work area ~384 KB too
             * low, on top of the voices' own ADPCM data. Writing it also sets
             * the current buffer address (DOCS/soundprocessingunitspu.md:810-814). */
            spu->reverb_base = value;
            spu->reverb_current_addr = (uint32_t)spu->reverb_base * 4u;
            break;
        /* Writing IRQA or TSA only stores an address. The IRQ is raised by an
         * access to SPU RAM at that address (soundprocessingunitspu.md:823-852),
         * and a register write is not one; both used to fire it on the spot when
         * a voice or the transfer pointer already sat there. */
        case SPU_REG_IRQ_ADDR:
            spu->irq_addr = value;
            break;
        case SPU_REG_TRANSFER_ADDR:
            spu->transfer_addr_reg = value;
            spu->transfer_addr = (uint32_t)value * 8;
            break;
        case 0x1AC: /* SPU_DATA_TRANSFER_CONTROL — ignore write */ break;
        case SPU_REG_TRANSFER_DATA: {
            /* "Data (max 32 halfwords)" (:683). The documented upload writes it
             * while the mode is Stop and selects Manual Write afterwards
             * (:715-721), so in Stop the halfword waits in the FIFO; it used to
             * be dropped. In Manual Write (and DMA write) it goes straight to RAM
             * as before: that is the multi-block case, where the mode is already
             * Manual Write when the next block's FIFO writes arrive (:722-727). */
            int mode = (spu->control >> 4) & 0x03;
            if (mode == TRANSFER_MANUAL_WRITE || mode == TRANSFER_DMA_WRITE) {
                spu_transfer_write(spu, inter, value);
            } else if (mode == TRANSFER_STOPPED) {
                spu_manual_fifo_push(spu, value);
            }
            /* DMA read: nothing documented, ignored as before. */
            break;
        }
        case SPU_REG_CONTROL:
            spu_set_control(spu, inter, value);
            break;
        case SPU_REG_CD_VOL_L: spu->cd_vol_left = (int16_t)value; break;
        case SPU_REG_CD_VOL_R: spu->cd_vol_right = (int16_t)value; break;
        case SPU_REG_EXT_VOL_L: spu->ext_vol_left = (int16_t)value; break;
        case SPU_REG_EXT_VOL_R: spu->ext_vol_right = (int16_t)value; break;
        default:
            if (reg >= 0x1C0 && reg < 0x200) {
                spu->reverb_regs[(reg - 0x1C0) / 2] = value;
            } else {
                LOG_SPU_DEBUG("[SPU] Write16 unknown reg 0x%04X val=0x%04X", reg, value);
            }
            break;
    }
}

void spu_write32(struct Interconnect* inter, uint32_t addr, uint32_t value) {
    spu_write16(inter, addr, (uint16_t)(value & 0xFFFF));
    spu_write16(inter, addr + 2, (uint16_t)((value >> 16) & 0xFFFF));
}

void spu_write8(struct Interconnect* inter, uint32_t addr, uint8_t value) {
    if (addr & 1) return;
    uint16_t prev = spu_read16(inter, addr);
    uint16_t newv = (prev & 0xFF00) | value;
    spu_write16(inter, addr, newv);
}
