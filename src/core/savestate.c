/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 ZioZoni95
 *
 * Part of ZoniStation One, a PlayStation 1 emulator.
 * See LICENSE for the full licence text and THIRD-PARTY.md for the
 * components of this project that have other authors.
 */
#include "savestate.h"

#include "cpu.h"
#include "interconnect.h"
#include "renderer.h"
#include "event_scheduler.h"
#include "log.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define ZS1_STATE_MAGIC   0x5A533153u   /* "ZS1S" */
/* 3: added SIOI — the SIO0 protocol state
 * 4: Cdrom gained head_lba, so every field after it moved. The CDRH/CDRT
 *    sections are raw byte ranges of the struct, so a v3 state would load
 *    shifted and appear to work while the drive state was nonsense.
 * 5: the SIO0 pad gained stick_mode (LED=green flight mode), so every field
 *    after it in the SIOI blob moved. Same hazard as 4: SIOI is a raw byte
 *    range of SioInternal, and a v4 state would restore shifted.
 * 6: Cdrom gained cmd_deadline/second_deadline (the drive keeps the due cycle
 *    of a response an unacknowledged interrupt is holding back), which moves
 *    every field after them inside the raw CDRH range.
 * 7: Cdrom gained last_header/last_header_valid (what GetlocL answers with),
 *    again inside CDRH.
 * 8: Cdrom gained seek_phase (the drive refuses GetlocL/Pause while the head is
 *    moving) and xa_mute (ADPCTL's ADPMUTE bit), both inside CDRH.
 * 9: Cpu lost out_regs[32] — the second register file the interpreter used to
 *    memcpy into regs on every instruction. T_CPU is the raw struct, so every
 *    field after the GPRs moved by 128 bytes.
 * 10: MemoryCard gained backed_up, which sits inside the raw T_SIO struct — and
 *    loading a state no longer restores the memory cards at all (see the note
 *    at the T_SIO read below). */
#define ZS1_STATE_VERSION 11u

#define TAG(a,b,c,d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

#define T_CPU  TAG('C','P','U',' ')
#define T_RAM  TAG('R','A','M',' ')
#define T_SCRP TAG('S','C','R','P')
#define T_INTC TAG('I','N','T','C')
#define T_GPUH TAG('G','P','U','H')
#define T_GPUT TAG('G','P','U','T')
#define T_DMA  TAG('D','M','A',' ')
#define T_TMR  TAG('T','M','R',' ')
#define T_CDRH TAG('C','D','R','H')
#define T_CDRT TAG('C','D','R','T')
#define T_SPU  TAG('S','P','U',' ')
#define T_SIO  TAG('S','I','O',' ')
#define T_SIOI TAG('S','I','O','I')
#define T_MDEC TAG('M','D','E','C')
#define T_DISC TAG('D','I','S','C')

/* The SIO0 protocol state (analog/config mode, the Analog-button lock, the
 * rumble map, the controller and memory-card transfer steps) is a file-static
 * inside sio.c, reached through sio_save/load_internal_state. Its size is a
 * runtime value, so the section travels through a fixed staging buffer with a
 * guard rather than a sized array. A few hundred bytes today; the guard is what
 * turns a future overrun into a refused save instead of a smashed stack. */
#define SIO_INTERNAL_MAX 2048

/* The Renderer inside Gpu, and the disc handles and reader thread inside Cdrom,
 * are host objects: GL names, an open FILE per track, a pthread and its
 * condition variables. Writing them would store values that mean nothing in the
 * next process, and reading them back would overwrite live handles with those
 * dead values. Each struct is therefore stored as the span before the excluded
 * member and the span after it. */
#define GPU_HEAD_SIZE  offsetof(Gpu, renderer)
#define GPU_TAIL_OFF   (offsetof(Gpu, renderer) + sizeof(Renderer))
#define GPU_TAIL_SIZE  (sizeof(Gpu) - GPU_TAIL_OFF)

#define CDR_HEAD_SIZE  offsetof(Cdrom, disc)
#define CDR_TAIL_OFF   (offsetof(Cdrom, async_reader) + sizeof(CdromAsyncReader))
#define CDR_TAIL_SIZE  (sizeof(Cdrom) - CDR_TAIL_OFF)

/* Which disc the state was taken on.
 *
 * The Cdrom struct's disc *flags* — disc_present, disc_region, current_lba —
 * live in the saved span, but the CdromDisc holding the open FILE per track
 * does not, and must not. Restoring the flags onto a machine with no disc
 * mounted leaves the drive believing it can read from tracks whose file handle
 * is NULL, and the first sector read dereferences it. That is what crashed a
 * load into a freshly started emulator.
 *
 * So the identity travels with the state and is checked before anything is
 * restored: refuse the load, with a message, rather than hand the drive a
 * position on a disc that is not in it. */
typedef struct {
    uint8_t  present;        /* did the state have a disc at all */
    uint8_t  first_track;
    uint8_t  last_track;
    uint8_t  region;         /* 'A' / 'E' / 'I', 0 if none */
    uint32_t total_sectors;
} DiscIdentity;

static void disc_identity_of(const Interconnect* inter, DiscIdentity* out) {
    memset(out, 0, sizeof(*out));
    out->present       = inter->cdrom.disc_present ? 1 : 0;
    out->first_track   = inter->cdrom.disc.first_track;
    out->last_track    = inter->cdrom.disc.last_track;
    out->region        = (uint8_t)inter->cdrom.disc_region;
    out->total_sectors = inter->cdrom.disc.total_sectors;
}

/* Interconnect scalars that are not covered by any embedded subsystem. */
typedef struct {
    uint16_t irq_status, irq_mask, irq_line_state;
    uint32_t evq_pending;
    uint32_t evq_target_cycle[EVQ_MAX_EVENTS];
    uint32_t evq_next_cycle;
    uint32_t cpu_cycle_counter;
    uint8_t  frame_complete;
    char     tty_line_buf[256];
    int32_t  tty_line_len;
    uint32_t memctrl_regs[9];
    uint32_t bios_access_cycles;
} IntcBlock;

/* --- writer ------------------------------------------------------------- */

static bool put_section(FILE* f, uint32_t tag, const void* data, size_t size) {
    uint32_t sz = (uint32_t)size;
    if (fwrite(&tag, 4, 1, f) != 1) return false;
    if (fwrite(&sz,  4, 1, f) != 1) return false;
    return fwrite(data, 1, size, f) == size;
}

bool savestate_save(const char* path, struct Cpu* cpu, struct Interconnect* inter) {
    if (!path || !cpu || !inter) return false;

    /* savestates/ may not exist on a fresh clone. */
    {
        const char* slash = strrchr(path, '/');
        if (slash && slash != path) {
            char dir[512];
            size_t n = (size_t)(slash - path);
            if (n >= sizeof(dir)) n = sizeof(dir) - 1;
            memcpy(dir, path, n);
            dir[n] = '\0';
            mkdir(dir, 0755);   /* EEXIST is fine */
        }
    }

    FILE* f = fopen(path, "wb");
    if (!f) {
        LOG_SYSTEM_ERROR("[STATE] Cannot open %s for writing", path);
        return false;
    }

    uint32_t magic = ZS1_STATE_MAGIC, version = ZS1_STATE_VERSION;
    bool ok = fwrite(&magic, 4, 1, f) == 1 && fwrite(&version, 4, 1, f) == 1;

    IntcBlock ib;
    memset(&ib, 0, sizeof(ib));
    ib.irq_status       = inter->irq_status;
    ib.irq_mask         = inter->irq_mask;
    ib.irq_line_state   = inter->irq_line_state;
    ib.evq_pending      = inter->evq_pending;
    memcpy(ib.evq_target_cycle, inter->evq_target_cycle, sizeof(ib.evq_target_cycle));
    ib.evq_next_cycle     = inter->evq_next_cycle;
    ib.cpu_cycle_counter  = inter->cpu_cycle_counter;
    ib.frame_complete     = inter->frame_complete ? 1 : 0;
    memcpy(ib.tty_line_buf, inter->tty_line_buf, sizeof(ib.tty_line_buf));
    ib.tty_line_len       = inter->tty_line_len;
    memcpy(ib.memctrl_regs, inter->memctrl_regs, sizeof(ib.memctrl_regs));
    ib.bios_access_cycles = inter->bios_access_cycles;

    DiscIdentity did;
    disc_identity_of(inter, &did);

    size_t sio_sz = sio_internal_state_size();
    uint8_t sio_blk[SIO_INTERNAL_MAX];
    if (sio_sz > sizeof(sio_blk)) {
        LOG_SYSTEM_ERROR("[STATE] SIO protocol state is %zu bytes, staging buffer is %zu — "
                         "raise SIO_INTERNAL_MAX", sio_sz, sizeof(sio_blk));
        fclose(f);
        return false;
    }
    sio_save_internal_state(sio_blk);

    ok = ok && put_section(f, T_DISC, &did,              sizeof(did));
    ok = ok && put_section(f, T_CPU,  cpu,               sizeof(Cpu));
    ok = ok && put_section(f, T_RAM,  inter->ram->data,  RAM_SIZE);
    ok = ok && put_section(f, T_SCRP, inter->scratchpad, SCRATCHPAD_SIZE);
    ok = ok && put_section(f, T_INTC, &ib,               sizeof(ib));
    ok = ok && put_section(f, T_GPUH, &inter->gpu,                          GPU_HEAD_SIZE);
    ok = ok && put_section(f, T_GPUT, (const uint8_t*)&inter->gpu + GPU_TAIL_OFF, GPU_TAIL_SIZE);
    ok = ok && put_section(f, T_DMA,  &inter->dma,          sizeof(Dma));
    ok = ok && put_section(f, T_TMR,  &inter->timers_state, sizeof(Timers));
    ok = ok && put_section(f, T_CDRH, &inter->cdrom,                          CDR_HEAD_SIZE);
    ok = ok && put_section(f, T_CDRT, (const uint8_t*)&inter->cdrom + CDR_TAIL_OFF, CDR_TAIL_SIZE);
    ok = ok && put_section(f, T_SPU,  &inter->spu,  sizeof(Spu));
    ok = ok && put_section(f, T_SIO,  &inter->sio,  sizeof(Sio));
    ok = ok && put_section(f, T_SIOI, sio_blk,      sio_sz);
    ok = ok && put_section(f, T_MDEC, &inter->mdec, sizeof(Mdec));

    long bytes = ftell(f);
    fclose(f);

    if (!ok) {
        LOG_SYSTEM_ERROR("[STATE] Write failed: %s", path);
        return false;
    }
    LOG_SYSTEM_INFO("[STATE] Saved %s (%ld bytes, PC=0x%08x, cycle=%u)",
                    path, bytes, cpu->pc, inter->cpu_cycle_counter);
    return true;
}

/* --- reader ------------------------------------------------------------- */

/* Reads the next section and copies it into dst. Sections are stored in a fixed
 * order, so a tag or size that does not match means the file was written by a
 * different build — refuse rather than load a mismatched layout. */
static bool get_section(FILE* f, uint32_t want_tag, void* dst, size_t want_size) {
    uint32_t tag = 0, sz = 0;
    if (fread(&tag, 4, 1, f) != 1 || fread(&sz, 4, 1, f) != 1) {
        LOG_SYSTEM_ERROR("[STATE] Truncated file (expected section '%.4s')", (const char*)&want_tag);
        return false;
    }
    if (tag != want_tag) {
        LOG_SYSTEM_ERROR("[STATE] Section order mismatch: got '%.4s', expected '%.4s'",
                         (const char*)&tag, (const char*)&want_tag);
        return false;
    }
    if (sz != (uint32_t)want_size) {
        LOG_SYSTEM_ERROR("[STATE] Section '%.4s' is %u bytes, this build expects %zu — "
                         "the state was written by a different layout",
                         (const char*)&tag, sz, want_size);
        return false;
    }
    if (fread(dst, 1, want_size, f) != want_size) {
        LOG_SYSTEM_ERROR("[STATE] Short read in section '%.4s'", (const char*)&tag);
        return false;
    }
    return true;
}

bool savestate_load(const char* path, struct Cpu* cpu, struct Interconnect* inter) {
    if (!path || !cpu || !inter) return false;

    FILE* f = fopen(path, "rb");
    if (!f) {
        LOG_SYSTEM_WARN("[STATE] No state at %s", path);
        return false;
    }

    uint32_t magic = 0, version = 0;
    if (fread(&magic, 4, 1, f) != 1 || fread(&version, 4, 1, f) != 1 ||
        magic != ZS1_STATE_MAGIC || version != ZS1_STATE_VERSION) {
        LOG_SYSTEM_ERROR("[STATE] %s is not a v%u ZoniStation state", path, ZS1_STATE_VERSION);
        fclose(f);
        return false;
    }

    /* Disc check first: this is the one section read before anything is
     * overwritten, so a refusal here leaves the machine exactly as it was. */
    uint8_t have_disc_present = 0;
    {
        DiscIdentity want, have;
        if (!get_section(f, T_DISC, &want, sizeof(want))) { fclose(f); return false; }
        disc_identity_of(inter, &have);

        if (want.present && !have.present) {
            LOG_SYSTEM_ERROR("[STATE] This state was taken with a disc in the drive "
                             "and no disc is mounted. Start with the same --game= and retry.");
            fclose(f);
            return false;
        }
        if (want.present &&
            (want.total_sectors != have.total_sectors ||
             want.first_track   != have.first_track   ||
             want.last_track    != have.last_track)) {
            LOG_SYSTEM_ERROR("[STATE] Different disc: state has tracks %u-%u / %u sectors, "
                             "mounted disc has %u-%u / %u",
                             want.first_track, want.last_track, want.total_sectors,
                             have.first_track, have.last_track, have.total_sectors);
            fclose(f);
            return false;
        }
        if (want.present && want.region && have.region && want.region != have.region) {
            LOG_SYSTEM_ERROR("[STATE] Region mismatch: state '%c', mounted disc '%c'",
                             want.region, have.region);
            fclose(f);
            return false;
        }
        have_disc_present = have.present;
    }

    /* Host-owned pointers, held across the load and put back afterwards. */
    Interconnect* cpu_inter   = cpu->inter;
    Interconnect* gpu_inter   = inter->gpu.inter;
    Interconnect* dma_inter   = inter->dma.inter;
    Interconnect* tmr_inter   = inter->timers_state.inter;
    Interconnect* cdr_inter   = inter->cdrom.inter;

    size_t sio_sz = sio_internal_state_size();
    uint8_t sio_blk[SIO_INTERNAL_MAX];
    if (sio_sz > sizeof(sio_blk)) {
        LOG_SYSTEM_ERROR("[STATE] SIO protocol state is %zu bytes, staging buffer is %zu — "
                         "raise SIO_INTERNAL_MAX", sio_sz, sizeof(sio_blk));
        fclose(f);
        return false;
    }

    IntcBlock ib;
    bool ok = true;
    ok = ok && get_section(f, T_CPU,  cpu,               sizeof(Cpu));
    ok = ok && get_section(f, T_RAM,  inter->ram->data,  RAM_SIZE);
    ok = ok && get_section(f, T_SCRP, inter->scratchpad, SCRATCHPAD_SIZE);
    ok = ok && get_section(f, T_INTC, &ib,               sizeof(ib));
    ok = ok && get_section(f, T_GPUH, &inter->gpu,                          GPU_HEAD_SIZE);
    ok = ok && get_section(f, T_GPUT, (uint8_t*)&inter->gpu + GPU_TAIL_OFF, GPU_TAIL_SIZE);
    ok = ok && get_section(f, T_DMA,  &inter->dma,          sizeof(Dma));
    ok = ok && get_section(f, T_TMR,  &inter->timers_state, sizeof(Timers));
    ok = ok && get_section(f, T_CDRH, &inter->cdrom,                          CDR_HEAD_SIZE);
    ok = ok && get_section(f, T_CDRT, (uint8_t*)&inter->cdrom + CDR_TAIL_OFF, CDR_TAIL_SIZE);
    ok = ok && get_section(f, T_SPU,  &inter->spu,  sizeof(Spu));
    /* The memory cards are host storage, not machine state.
     *
     * They live inside Sio, so a raw read of the struct restores whatever cards
     * the state was written with — and since every boot writes frame 63 as its
     * write test, the next save flushes those cards over the .mcd files. A state
     * captured with empty cards therefore erases real saves one boot later,
     * which is exactly how two cards full of saves became two formatted blanks
     * here. Nothing on hardware rewinds the card in your hand when the machine
     * is restored, so the live cards are held across the read and put back. */
    static MemoryCard keep_slot1, keep_slot2;   /* 128 KB each — not on the stack */
    keep_slot1 = inter->sio.card_slot1;
    keep_slot2 = inter->sio.card_slot2;
    ok = ok && get_section(f, T_SIO,  &inter->sio,  sizeof(Sio));
    inter->sio.card_slot1 = keep_slot1;
    inter->sio.card_slot2 = keep_slot2;
    bool sio_ok = ok && get_section(f, T_SIOI, sio_blk, sio_sz);
    ok = ok && sio_ok;
    ok = ok && get_section(f, T_MDEC, &inter->mdec, sizeof(Mdec));
    fclose(f);

    /* Only hand the block over once it actually read, otherwise a failed section
     * would push an uninitialised staging buffer into the live protocol state. */
    if (sio_ok) sio_load_internal_state(sio_blk);

    /* Put the host pointers back before anything can dereference them. A
     * partial load has already overwritten some of the structs, so this runs
     * whether or not the read succeeded. */
    cpu->inter                 = cpu_inter;
    inter->gpu.inter           = gpu_inter;
    inter->dma.inter           = dma_inter;
    inter->timers_state.inter  = tmr_inter;
    inter->cdrom.inter         = cdr_inter;

    /* The drive's idea of what is in it comes from the hardware in front of us,
     * never from the file — the identity check above only proved they agree. */
    inter->cdrom.disc_present = (have_disc_present != 0);

    if (!ok) {
        LOG_SYSTEM_ERROR("[STATE] Load of %s failed — the machine is now in a "
                         "partially restored state; restart before trusting it", path);
        return false;
    }

    inter->irq_status      = ib.irq_status;
    inter->irq_mask        = ib.irq_mask;
    inter->irq_line_state  = ib.irq_line_state;
    inter->evq_pending     = ib.evq_pending;
    memcpy(inter->evq_target_cycle, ib.evq_target_cycle, sizeof(ib.evq_target_cycle));
    inter->evq_next_cycle    = ib.evq_next_cycle;
    inter->cpu_cycle_counter = ib.cpu_cycle_counter;
    inter->frame_complete    = ib.frame_complete != 0;
    memcpy(inter->tty_line_buf, ib.tty_line_buf, sizeof(ib.tty_line_buf));
    inter->tty_line_len      = ib.tty_line_len;
    memcpy(inter->memctrl_regs, ib.memctrl_regs, sizeof(ib.memctrl_regs));
    inter->bios_access_cycles = ib.bios_access_cycles;

    /* State the MDEC keeps outside its saved span (the idle value of status
     * bits 15-0) is re-derived rather than left at whatever this session had. */
    mdec_state_restored(&inter->mdec);

    /* VBlank must always be in the queue. It is the frame boundary, the source
     * of IRQ0 and the only event that re-arms itself, so a state that lost it —
     * anything written from inside the dispatch, before the handler rescheduled
     * itself — restores into a machine that never produces another frame: black
     * screen, no interrupts, silent SPU. Rather than refuse such a file, put the
     * event back; one frame's phase is not worth losing the state over. */
    if (!(inter->evq_pending & (1u << EVQ_VBLANK))) {
        LOG_SYSTEM_WARN("[STATE] The state carried no pending VBlank — rearming it. "
                        "It was written from inside the event dispatch.");
        eventq_schedule(inter, EVQ_VBLANK, gpu_cycles_per_frame(&inter->gpu));
    }

    /* evq_next_cycle is a cache over the pending set — rebuild it rather than
     * trust the stored copy. Same reasoning as pcsx-redux re-deriving its
     * counter rates and cycle anchors on load instead of restoring them. */
    eventq_recompute_next(inter);

    /* What the machine looks like at the instant it resumes. A state that loads
     * and then does nothing is the failure mode here, and it is invisible from
     * outside: the window keeps redrawing, so it reads as a freeze rather than as
     * a stopped event queue. These four numbers separate the cases — no pending
     * VBlank, a VBlank target that never arrives, a zero frame length, or a CPU
     * downcount that stops events being dispatched at all. */
    LOG_SYSTEM_INFO("[STATE] Resume: cycle=%u evq_pending=0x%08x vblank_target=%u "
                    "next=%u cycles_per_frame=%u downcount=%d",
                    inter->cpu_cycle_counter, inter->evq_pending,
                    inter->evq_target_cycle[EVQ_VBLANK], inter->evq_next_cycle,
                    gpu_cycles_per_frame(&inter->gpu), cpu->downcount);

    /* The GL side holds the picture, and nothing in the file reached it: push
     * the restored VRAM through the path that stamps both the sampling mirror
     * and the unified texture, or the first frame after a load shows whatever
     * the previous state left on screen. */
    renderer_upload_vram_rect(&inter->gpu.renderer,
                              (const uint16_t*)inter->gpu.vram.data, 0, 0, 1024, 512);
    inter->gpu.vram_dirty = false;

    LOG_SYSTEM_INFO("[STATE] Loaded %s (PC=0x%08x, cycle=%u)",
                    path, cpu->pc, inter->cpu_cycle_counter);
    return true;
}
