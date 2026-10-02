/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 ZioZoni95
 *
 * Part of ZoniStation One, a PlayStation 1 emulator.
 * See LICENSE for the full licence text and THIRD-PARTY.md for the
 * components of this project that have other authors.
 */
/*
 * system.c — core per-frame execution driver.
 *
 * Extracted from main.c's inline loop so main.c can be a thin host shell.
 * The machine runs until the VBlank event marks the frame boundary
 * (inter->frame_complete); the CPU's downcount already dispatches every due
 * scheduled event (VBlank/CDROM/DMA/SIO/MDEC) inside cpu_run_next_instruction,
 * so there is no manual chunk-sizing here beyond bounding each inner burst by
 * "cycles until the next event" (still needed while timers are stepped by hand
 * — that hand-stepping goes away in the timer rewrite step).
 */

#include "system.h"
#include "interconnect.h"
#include "cpu.h"
#include "event_scheduler.h"
#include "timers.h"
#include "gpu.h"
#include "spu.h"
#include "debugger.h"
#include "debug_ui.h"
#include "frame_events.h"

void system_init(Interconnect* inter, Cpu* cpu) {
    (void)cpu;
    inter->frame_complete = false;
    /* VBlank is the frame boundary and self-reschedules. Timers arm their own
     * EVQ_TIMER{0,1,2} events at their first target/overflow. */
    eventq_schedule(inter, EVQ_VBLANK, gpu_cycles_per_frame(&inter->gpu));
    /* SPU samples are produced from the emulated clock, on this thread. */
    inter->spu.last_update_cycle = inter->cpu_cycle_counter;
    eventq_schedule(inter, EVQ_SPU, SPU_EVENT_PERIOD_CYCLES);
    timers_start(&inter->timers_state);
}

void system_run_frame(Interconnect* inter, Cpu* cpu) {
    Debugger* dbg = &inter->debugger;

    /* Paused: service at most one single-step request, then let main render. */
    if (dbg->paused) {
        if (debug_ui_step_requested()) {
            dbg->step_skip_bp = true;
            dbg->paused = false;
            cpu_run_next_instruction(cpu);
            dbg->paused = true;
        }
        return;
    }

    inter->frame_complete = false;

    /* Safety bound: if VBlank ever stops firing, never spin forever — cap a
     * frame at ~2x its cycle budget and bail (a bug elsewhere, but not a hang). */
    const uint32_t start = inter->cpu_cycle_counter;
    const uint32_t cap   = 2u * gpu_cycles_per_frame(&inter->gpu);

    /* Just run the CPU: every instruction dispatches each due scheduled event
     * (VBlank, timers, CDROM, DMA, ...) via its downcount check, and timers
     * catch up on-read/on-event, so there is no manual stepping. The loop lives
     * in cpu_execution.c now, next to the instruction it runs, so it costs no
     * cross-unit call per instruction; it tests frame_complete, the pause and
     * the cap after every instruction exactly as this loop did. */
    if (!cpu_run_frame(cpu, start, cap)) return;       /* breakpoint hit mid-frame */

    /* Frame boundary: publish the event ring the Frame view reads. */
    frame_events_end_frame();
}
