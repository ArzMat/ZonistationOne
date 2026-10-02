/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 ZioZoni95
 *
 * Part of ZoniStation One, a PlayStation 1 emulator.
 * See LICENSE for the full licence text and THIRD-PARTY.md for the
 * components of this project that have other authors.
 */
#include "event_scheduler.h"
#include "interconnect.h"
#include "cpu.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "log.h"
#include "dma.h"           // For DMA structures and helpers
#include "gpu.h"           // For GPU DMA transfer
#include "timers.h"
#include "spu.h"        // For timer event handler prototypes
#include "sio.h"           // For sio_execute_event
#include "lua_debug.h"     // For the Lua "vblank" probe hook

// ===============================
// Event Scheduler Implementation
// ===============================
// This file implements the central event/timing system for the emulator.
// All hardware events (timers, VBlank, DMA, etc.) are scheduled and dispatched here.
// --- Event Handler Table ---
typedef void (*EventQueueHandler)(struct Interconnect*);

// Event handler forward declarations
static void evq_handle_vblank(struct Interconnect* sys); // VBlank event
static void evq_handle_timer0(struct Interconnect* sys); // Timer0 event
static void evq_handle_timer1(struct Interconnect* sys); // Timer1 event
static void evq_handle_timer2(struct Interconnect* sys); // Timer2 event
static void evq_handle_dma_gpu(struct Interconnect* sys);   // GPU DMA event
static void evq_handle_dma_cdrom(struct Interconnect* sys); // CDROM DMA event
static void evq_handle_cdrom_command(struct Interconnect* sys);         // CDROM command event
static void evq_handle_cdrom_drive(struct Interconnect* sys);           // CDROM drive event
static void evq_handle_cdrom_second_response(struct Interconnect* sys); // CDROM second-response event
static void evq_handle_sio(struct Interconnect* sys);   // SIO deferred transfer
static void evq_handle_spu(struct Interconnect* sys);   // SPU sample generation

// Table of event handlers, indexed by EventQueueType
typedef EventQueueHandler EventHandlerTable[EVQ_EVENT_COUNT];
static EventHandlerTable evq_handlers = {
    evq_handle_vblank,                // EVQ_VBLANK
    evq_handle_timer0,                // EVQ_TIMER0
    evq_handle_timer1,                // EVQ_TIMER1
    evq_handle_timer2,                // EVQ_TIMER2
    evq_handle_dma_gpu,               // EVQ_DMA_GPU
    evq_handle_dma_cdrom,             // EVQ_DMA_CDROM
    evq_handle_sio,                   // EVQ_SIO
    evq_handle_cdrom_command,         // EVQ_CDROM_COMMAND
    evq_handle_cdrom_drive,           // EVQ_CDROM_DRIVE
    evq_handle_cdrom_second_response, // EVQ_CDROM_SECOND_RESPONSE
    dma_mdec_resume,                  // EVQ_MDEC — sliced ch0/ch1 MDEC DMA (see dma_mdec_resume, bus.c)
    evq_handle_spu                    // EVQ_SPU — samples owed since the last tick
};

// --- Event Scheduling ---
/**
 * @brief Schedule an event to occur after a given number of cycles from now.
 */
void eventq_schedule(struct Interconnect* sys, EventQueueType event, uint32_t cycles_from_now) {
    sys->evq_pending |= (1u << event);
    sys->evq_target_cycle[event] = sys->cpu_cycle_counter + cycles_from_now;

    // Update the next event cycle if this event is sooner, or the stored next
    // has already passed. Signed-delta compares (not absolute >/<=) so this is
    // correct across the uint32 cpu_cycle_counter wrap (~every 127 s).
    int32_t d_target = (int32_t)(sys->evq_next_cycle - sys->evq_target_cycle[event]); // >0: target sooner
    int32_t d_stale  = (int32_t)(sys->evq_next_cycle - sys->cpu_cycle_counter);       // <=0: next passed
    if (d_target > 0 || d_stale <= 0) {
        sys->evq_next_cycle = sys->evq_target_cycle[event];
        // Immediately truncate CPU downcount so the CPU wakes up at the right time
        if (sys->cpu) {
            int32_t cycles_until = (int32_t)(sys->evq_next_cycle - sys->cpu_cycle_counter);
            if (cycles_until > 0 && cycles_until < sys->cpu->downcount)
                sys->cpu->downcount = cycles_until;
        }
    }
}

// --- Event Dispatch ---
/**
 * @brief Check and dispatch any events that are due at the current cycle.
 */
void eventq_dispatch_due(struct Interconnect* sys) {
    uint32_t now = sys->cpu_cycle_counter;
    
    // Keep dispatching as long as any event is due
    while (1) {
        uint32_t pending = sys->evq_pending;
        int any_fired = 0;
        /* The next target, gathered on the way. It is only used from a pass in
         * which nothing fired, and such a pass sees exactly what
         * eventq_recompute_next() would see afterwards: no handler ran, so
         * evq_pending, every target and cpu_cycle_counter are unchanged, and
         * every pending event is one that did not fire. Same condition, same
         * order, same result; one scan of the twelve slots fewer per
         * dispatch. */
        uint32_t soonest    = UINT32_MAX;
        int32_t  best_delta = INT32_MAX;
        
        for (EventQueueType event = 0; event < EVQ_EVENT_COUNT; ++event) {
            if (!(pending & (1u << event))) continue;
            const uint32_t target = sys->evq_target_cycle[event];
            if ((int32_t)(now - target) >= 0) {
                sys->evq_pending &= ~(1u << event);
                if (evq_handlers[event]) {
                    evq_handlers[event](sys);
                }
                any_fired = 1;
            } else {
                const int32_t delta = (int32_t)(target - now);
                if (delta > 0 && delta < best_delta) {
                    best_delta = delta;
                    soonest = target;
                }
            }
        }
        if (!any_fired) {
            sys->evq_next_cycle = soonest;   /* == eventq_recompute_next(sys) */
            return;
        }
        // After firing, update now in case event handler advanced cycles
        now = sys->cpu_cycle_counter;
    }
}

/* Derive the next-event anchor from the pending set rather than carrying it.
 * Called after dispatch, and after a savestate load: evq_next_cycle is a cache
 * over evq_pending and evq_target_cycle, and a cache restored from a file is
 * only as trustworthy as the moment it was written. */
void eventq_recompute_next(struct Interconnect* sys) {
    const uint32_t now = sys->cpu_cycle_counter;
    uint32_t soonest = UINT32_MAX;
    int32_t  best_delta = INT32_MAX;
    for (EventQueueType event = 0; event < EVQ_EVENT_COUNT; ++event) {
        if (sys->evq_pending & (1u << event)) {
            uint32_t target = sys->evq_target_cycle[event];
            int32_t  delta  = (int32_t)(target - now);
            if (delta > 0 && delta < best_delta) {
                best_delta = delta;
                soonest = target;
            }
        }
    }
    sys->evq_next_cycle = soonest;
}

// --- Next Event Cycle Query ---
/**
 * @brief Get the cycle of the next scheduled event (for main loop efficiency).
 */
uint32_t eventq_next_cycle(const struct Interconnect* sys) {
    return sys->evq_next_cycle;
}

uint32_t eventq_cycles_until_next(const struct Interconnect* sys) {
    const uint32_t now = sys->cpu_cycle_counter;
    const uint32_t next = sys->evq_next_cycle;

    if (next == UINT32_MAX) {
        // No pending scheduled events: execute at least one cycle.
        return 1;
    }

    if ((int32_t)(now - next) >= 0) {
        return 0;
    }

    return next - now;
}

// --- Event Handlers ---

#define TIMER0_CYCLES 1000   // Placeholder, tune as needed

static void evq_handle_vblank(struct Interconnect* sys) {
    /* Fold the field that just ended into the monotonic cycle total before
     * anything else logs: cpu_cycle_counter wraps every ~127 s, so the 32-bit
     * difference is the only safe way to accumulate it. This is what the log
     * stamp and any cross-emulator timing comparison read. */
    sys->emu_cycle_base += (uint64_t)(sys->cpu_cycle_counter - sys->field_cycle_mark);
    sys->field_cycle_mark = sys->cpu_cycle_counter;
    sys->field_count++;

    /* Period follows the GPU's current video mode — see gpu_cycles_per_frame().
     * A fixed NTSC period here ran PAL titles ~20% fast. */
    const uint32_t vblank_cycles = gpu_cycles_per_frame(&sys->gpu);

    gpu_crtc_tick(&sys->gpu, vblank_cycles);

    if (sys->field_count <= 5 || sys->field_count % 60 == 0)
        LOG_EVENT_DEBUG("[EVQ] VBlank #%u (cycle=%u, period=%u)",
                        sys->field_count, sys->cpu_cycle_counter, vblank_cycles);

    /* Periodic probe point for debug scripts: the one event that keeps firing
     * no matter what the guest is doing, so a script can sample state (PC, DMA,
     * display) even while the guest is stuck in a wait loop. */
    lua_debug_notify("vblank");

    eventq_schedule(sys, EVQ_VBLANK, vblank_cycles);

    if (!(sys->evq_pending & (1u << EVQ_VBLANK))) {
        LOG_EVENT_ERROR("[EVQ] CRITICAL: VBlank not rescheduled! pending=0x%X", sys->evq_pending);
    }
    
    // Trigger VBlank interrupt (IRQ0 per PSX-SPX)
    interconnect_request_irq(sys, 0, "VBlank");

    // The VBlank event is the frame boundary — end the current system_run_frame().
    sys->frame_complete = true;

    // NOTE: renderer_blit_vram disabled — FBO already contains correctly rendered content
    // from OpenGL primitive draws (polys, rects, lines). The blit read from vram_texture
    // (CPU-side R16UI buffer) had a UV wrap bug: fragment shader masks v with &0xFF, so
    // for vram_y=240 the v coordinate wraps at 256 → draws VRAM y=0 data over frame 2.
    // draw_ps1_display() now crops the FBO to crtc.display_vram_y/display_height directly.
}

/* Periodic firing log (first few + every 512th) so the real timer cadence is
 * visible without flooding — same style as the VBlank counter above. */
static void evq_log_timer_fire(int idx, uint32_t cycle) {
    static uint32_t n[3] = {0, 0, 0};
    uint32_t c = ++n[idx];
    if (c <= 5 || (c % 512) == 0)
        LOG_EVENT_DEBUG("[EVQ] Timer%d fired #%u (cycle=%u)", idx, c, cycle);
}

static void evq_handle_timer0(struct Interconnect* sys) {
    evq_log_timer_fire(0, sys->cpu_cycle_counter);
    timer0_event_handler(sys);
}
static void evq_handle_timer1(struct Interconnect* sys) {
    evq_log_timer_fire(1, sys->cpu_cycle_counter);
    timer1_event_handler(sys);
}
static void evq_handle_timer2(struct Interconnect* sys) {
    evq_log_timer_fire(2, sys->cpu_cycle_counter);
    timer2_event_handler(sys);
}

static void evq_handle_dma_gpu(struct Interconnect* sys) {
    /* Resume GPU DMA slice — sets up next slice or signals completion */
    dma_gpu_resume(sys);
}

static void evq_handle_dma_cdrom(struct Interconnect* sys) {
    /* CDROM DMA is performed synchronously in interconnect_perform_dma()
       when channel 3 CHCR bit 24 is written. This handler is unused. */
    (void)sys;
}

static void evq_handle_cdrom_command(struct Interconnect* sys) {
    cdrom_command_event_tick(sys);
}

static void evq_handle_cdrom_drive(struct Interconnect* sys) {
    cdrom_drive_event_tick(sys);
}

static void evq_handle_cdrom_second_response(struct Interconnect* sys) {
    cdrom_second_response_event_tick(sys);
}

static void evq_handle_sio(struct Interconnect* sys) {
    LOG_EVENT_DEBUG("[EVQ] Firing SIO deferred byte transfer");
    sio_execute_event(&sys->sio);
}

/* SPU: generate every sample the emulated clock owes, then re-arm.
 *
 * The period covers a batch of samples rather than one, because the per-sample
 * work is small and an event dispatch at 44100 Hz would be pure overhead.
 * Register accesses catch the SPU up on demand (spu_catch_up), so the batch size
 * bounds only how far the output ring can run dry — never how accurately a
 * register write lands relative to the audio it affects. */
static void evq_handle_spu(struct Interconnect* sys) {
    spu_catch_up(sys);
    eventq_schedule(sys, EVQ_SPU, SPU_EVENT_PERIOD_CYCLES);
}
