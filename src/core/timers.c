/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 ZioZoni95
 * SPDX-FileCopyrightText: The PCSX ReARMed authors
 *
 * Part of ZoniStation One, a PlayStation 1 emulator.
 * See LICENSE for the full licence text and THIRD-PARTY.md for the
 * components of this project that have other authors.
 */
// timers.c
#include "timers.h"
#include "interconnect.h" // Needed for interconnect_request_irq and IRQ defines
#include <stdio.h>
#include "gpu.h"
#include <string.h>
#include <math.h> // For floor()
#include "log.h"
#include "event_scheduler.h" // For eventq_schedule

/* The double-typed clock this file's rate arithmetic divides by. Note that
 * cdrom_disc.h already defines PSX_SYSCLK_HZ as the same figure in integer form;
 * this file used to redefine that name as a double, which shadowed it with a
 * different type. Nothing here ever read it — every rate below uses PSX_CPU_HZ
 * directly — so the redefinition was only a warning and a trap waiting for the
 * first person to use the name and get integer or float arithmetic by accident. */
#define PSX_CPU_HZ 33868800.0
#define DOTCLOCK_NTSC_HZ 25175000.0
#define DOTCLOCK_PAL_HZ 25200000.0 // PAL frequency, for completeness
#define HBLANK_NTSC_HZ 15625.0 // Horizontal blanking frequency for NTSC
// --- VBlank timing constant (NTSC: 33868800 / 60) ---
#define VBLANK_CYCLES 564480
#define TIMER_MODE_OFFSET 0x4

// Logging: Only use LOG_ERROR for timer hardware faults. No per-frame or per-IRQ logs.

#include "cpu.h"

/* A timers_handle_setrcnt() stub used to live here: declared in timers.h, called
 * by nobody, and an empty body. Removed rather than silenced. This project runs
 * the BIOS low-level, so SetRCnt(0xBC) executes as real BIOS code writing the
 * timer registers, and those writes already land in this file through the normal
 * register path — there is nothing for a side-channel handler to do. Leaving a
 * no-op with the name of a syscall handler invited someone to call it and
 * believe the syscall had been handled. */

// Derived-counter model forward declarations (definitions further down).
static uint32_t timer_rate_cycles(Timers* timers, Timer* t, int i);
static void timers_catch_up_one(Timers* timers, int i);
static void timer_rebase(Timers* timers, int i);

/**
 * @brief Recomputes counting_enabled from sync_enable/sync_mode/gate.
 * Includes Timer2's gate-less special case (its sync bit selects a
 * stop/free-run behavior instead of real gating).
 */
static void timer_update_counting_enabled(Timer* t, int timer_index) {
    if (timer_index != 2) {
        if (t->sync_enable) {
            switch (t->sync_mode) {
                case 0: /* PauseWhileGateActive */
                    t->counting_enabled = !t->gate;
                    break;
                case 1: /* ResetOnGateEnd */
                    t->counting_enabled = true;
                    break;
                case 2: /* ResetAndRunOnGateStart */
                case 3: /* FreeRunOnGateEnd */
                    t->counting_enabled = t->gate;
                    break;
            }
        } else {
            t->counting_enabled = true;
        }
    } else {
        /* Timer2: no gate source. Sync modes 0/3 stop counting, 1/2 free-run. */
        t->counting_enabled = !t->sync_enable || t->sync_mode == 1 || t->sync_mode == 2;
    }
}

/**
 * @brief Sets the external gate line level for a timer and applies the PS1
 * sync-mode edge behavior (counter reset/pause per mode). See timers.h.
 */
void timers_set_gate(Timers* timers, int timer_index, bool state) {
    if (timer_index < 0 || timer_index > 2) return;
    Timer* t = &timers->timers[timer_index];
    if (t->gate == state) return;

    /* Freeze the live counter into t->counter before the gate edge changes
     * counting/reset state, so the derived value doesn't jump. */
    timers_catch_up_one(timers, timer_index);
    t->gate = state;

    if (t->sync_enable) {
        switch (t->sync_mode) {
            case 0: /* PauseWhileGateActive: no counter change on edge */
                break;
            case 1: /* ResetOnGateEnd: reset while gate is active */
                t->counter = state ? t->counter : 0;
                break;
            case 2: /* ResetAndRunOnGateStart: reset at gate-active edge */
                t->counter = state ? 0 : t->counter;
                break;
            case 3: /* FreeRunOnGateEnd: sync_enable latches off once gate clears */
                t->sync_enable = t->sync_enable && state;
                break;
        }
        timer_update_counting_enabled(t, timer_index);
    }

    /* Re-anchor to the (possibly reset) counter and re-arm — or, if the gate
     * just paused the timer, timers_reschedule no-ops and it stays frozen. */
    timer_rebase(timers, timer_index);
    timers_reschedule(timers, timer_index);
}

/**
 * @brief Helper function to decode the mode register into internal state flags.
 * Called whenever the mode register is written.
 * @param timers Pointer to the Timers structure.
 * @param timer Pointer to the Timer instance to update.
 * @param timer_index Index of the timer (0, 1, or 2).
 */
static void timer_update_internal_state(Timers* timers, Timer* timer, int timer_index) {
    uint16_t mode = timer->mode;

    timer->sync_enable       = (mode & (1 << 0)) != 0;
    timer->sync_mode         = (mode >> 1) & 0x3;
    timer->reset_on_target   = (mode & (1 << 3)) != 0;
    timer->irq_on_target     = (mode & (1 << 4)) != 0;
    timer->irq_on_ffff       = (mode & (1 << 5)) != 0;
    timer->irq_repeat        = (mode & (1 << 6)) != 0;
    timer->irq_pulse         = (mode & (1 << 7)) != 0;
    timer->clock_source      = (mode >> 8) & 0x3;

    // Writing to mode register acknowledges/clears sticky IRQ flags (Bits 11, 12)
    // Also clear our internal tracking flags.
    timer->reached_target_flag = false;
    timer->reached_ffff_flag   = false;
    timer->interrupt_requested = false;
    // Clear Mode[10] interrupt request flag in the actual hardware register value
    timer->mode &= ~(1 << 10);
    /* Re-assert if counter already at target/overflow when mode is written */
    if ((timer->irq_on_target && timer->counter == timer->target) ||
        (timer->irq_on_ffff   && timer->counter == 0xFFFF)) {
        timer->interrupt_requested = true;
        timer->mode |= (1 << 10);
        interconnect_request_irq(timers->inter, timer->irq, "Timer re-assert after mode write");
    }

    timer_update_counting_enabled(timer, timer_index);
}

/**
 * @brief Initializes the state of all three timers.
 * @param timers Pointer to the Timers structure.
 * @param inter Pointer to the Interconnect (needed for requesting interrupts).
 */
void timers_init(Timers* timers, struct Interconnect* inter) {
timers->inter = inter; // Store interconnect pointer

    // Initialize all three timers
    for (int i = 0; i < 3; ++i) {
        Timer* t = &timers->timers[i];
        t->counter = 0;
        t->mode    = 0;
        t->target  = 0;
        timer_update_internal_state(timers, t, i);
        t->reached_target_flag = false;
        t->reached_ffff_flag   = false;
        t->interrupt_requested = false;
        timers->fractional_ticks[i] = 0.0;
        // Set new fields
        t->rate = (i == 2) ? 8 : 1; // Default: Timer2 is /8, others are /1
        t->irq = (i == 0) ? TIMER0_IRQ : (i == 1) ? TIMER1_IRQ : TIMER2_IRQ;
        t->counter_state = TIMER_COUNT_TO_OVERFLOW;
        t->irq_state = 0;
        t->cycle = 0;
        t->cycle_start = 0;
    }
}


/**
 * @brief Reads a 16-bit value from a timer register.
 * @param timers Pointer to the Timers structure.
 * @param timer_index Index of the timer (0, 1, or 2).
 * @param offset Register offset (0x0, 0x4, 0x8).
 * @return The 16-bit value read.
 */
uint16_t timer_read16(Timers* timers, int timer_index, uint32_t offset) {
    if (timer_index < 0 || timer_index > 2) {
        LOG_TIMER_ERROR("[TIMER] Timer Read Error: Invalid timer index %d", timer_index);
        return 0;
    }
    Timer* t = &timers->timers[timer_index];

    switch (offset) {
        case TMR_REG_VAL: // 0x0: Counter Value
            // Catch up to the current cycle so a busy-poll sees a live value
            // (derived from cycle_start), not one frozen until the next event.
            timers_catch_up_one(timers, timer_index);
            return t->counter;
        case TMR_REG_MODE: // 0x4: Mode Register
            {
                /* Catch up first, as the counter read does. The reached flags
                 * (bits 11-12) say whether the counter has hit its target or
                 * FFFFh since the last read (psx-spx ps1/system/timers.md:50-51), and
                 * that is a question about now. Without this they were only as
                 * current as the last timer event, which can sit up to one
                 * timer tick (the rate, in CPU cycles) past the boundary; a
                 * counter read in that window already saw the wrap. */
                timers_catch_up_one(timers, timer_index);
                // Update read-only status bits before returning mode value
                uint16_t mode = t->mode & ~0x1F00; // Clear status bits 12:10
                mode |= (uint16_t)t->reached_target_flag << 11;
                mode |= (uint16_t)t->reached_ffff_flag << 12;
                // Bit 10 (IRQ flag) reflects internal request state combined with enable bits
                bool irq_flag = (t->reached_target_flag && t->irq_on_target) ||
                                (t->reached_ffff_flag && t->irq_on_ffff);
                mode |= (uint16_t)irq_flag << 10;
                
                // HIGH PRIORITY FIX: PSX-SPEX mandates clearing flags after read
                t->reached_target_flag = false;
                t->reached_ffff_flag = false;
                
                return mode;
            }
        case TMR_REG_TARGET: // 0x8: Target Value
            return t->target;
        default:
            LOG_TIMER_ERROR("[TIMER] Timer Read Error: Unhandled timer%d offset 0x%x", timer_index, offset);
            return 0;
    }
}

/**
 * @brief Reads a 32-bit value from a timer register pair (Not standard PSX access).
 * For simplicity, just reads the lower 16 bits from the specified offset.
 * @param timers Pointer to the Timers structure.
 * @param timer_index Index of the timer (0, 1, or 2).
 * @param offset Register offset (0x0, 0x4, 0x8).
 * @return The 32-bit value (16-bit register zero-extended).
 */
uint32_t timer_read32(Timers* timers, int timer_index, uint32_t offset) {
    // 32-bit reads to timer registers likely only read the first 16 bits
    return (uint32_t)timer_read16(timers, timer_index, offset);
}


/**
 * @brief Writes a 16-bit value to a timer register.
 * @param timers Pointer to the Timers structure.
 * @param timer_index Index of the timer (0, 1, or 2).
 * @param offset Register offset (0x0, 0x4, 0x8).
 * @param value The 16-bit value to write.
 */
/* Re-anchor cycle_start so the derived counter equals t->counter at "now". */
static void timer_rebase(Timers* timers, int i) {
    Timer* t = &timers->timers[i];
    if (!timers->inter) { return; }
    uint32_t rate = timer_rate_cycles(timers, t, i);
    if (rate == 0) rate = 1;
    t->rate = rate;
    t->cycle_start = timers->inter->cpu_cycle_counter - (uint32_t)t->counter * rate;
}

void timer_write16(Timers* timers, int timer_index, uint32_t offset, uint16_t value) {
    Timer* t = &timers->timers[timer_index];
    /* Catch up to the exact write cycle before applying, so the change takes
     * effect from now (register writes are synchronous). */
    timers_catch_up_one(timers, timer_index);

    if (offset == TIMER_MODE_OFFSET) {
        LOG_TIMER_DEBUG("[TIMER] Timer%d mode <- 0x%04x (sync=%d clkSrc=%d irqTarget=%d irqOverflow=%d)",
                        timer_index, value,
                        (value & 1) != 0, (value >> 8) & 3,
                        (value >> 4) & 1, (value >> 5) & 1);
        t->mode = value;
        t->counter = 0;                 /* mode write resets the counter */
        t->interrupt_requested = false;
        t->reached_target_flag = false;
        t->reached_ffff_flag = false;
        t->mode &= ~(1 << 10);          /* clear IRQ request bit */
        timer_update_internal_state(timers, t, timer_index);
        timer_rebase(timers, timer_index);
        timers_reschedule(timers, timer_index);
        return;
    }
    switch (offset) {
        case TMR_REG_VAL:
            t->counter = value;
            timer_rebase(timers, timer_index);
            timers_reschedule(timers, timer_index);
            break;
        case TMR_REG_MODE:
            t->mode = value;
            timer_update_internal_state(timers, t, timer_index);
            timer_rebase(timers, timer_index);
            timers_reschedule(timers, timer_index);
            break;
        case TMR_REG_TARGET:
            t->target = value;          /* period changed → re-arm */
            timers_reschedule(timers, timer_index);
            break;
        default:
            LOG_TIMER_ERROR("[TIMER] Timer Write Error: Unhandled timer%d offset 0x%x", timer_index, offset);
            break;
    }
}

/**
 * @brief Writes a 32-bit value to a timer register pair (Not standard PSX access).
 * Writes the lower 16 bits of the value to the specified register offset.
 * @param timers Pointer to the Timers structure.
 * @param timer_index Index of the timer (0, 1, or 2).
 * @param offset Register offset (0x0, 0x4, 0x8).
 * @param value The 32-bit value (lower 16 bits are used).
 */
void timer_write32(Timers* timers, int timer_index, uint32_t offset, uint32_t value) {
    // 32-bit writes to timer registers likely only write the lower 16 bits
    timer_write16(timers, timer_index, offset, (uint16_t)value);
}

/* =========================================================================
 * Derived-counter timer model (interpreter-native, PCSX-Redux style).
 *
 * The counter is NOT stepped tick-by-tick. Its live value is DERIVED from the
 * global cpu_cycle_counter: counter = (now - cycle_start) / rate. The IRQ/reset
 * fires from the scheduled EVQ_TIMER event (armed at the next target/overflow),
 * and any register read/write catches the timer up to "now" on demand — so a
 * busy-poll always sees a continuously-advancing value with no per-tick loop.
 * See pcsx-redux Counters::readCounterInternal / update / set (psxcounters.cc).
 * ====================================================================== */

/* CPU cycles per timer tick for this timer's clock source (integer, >=1,
 * like Redux's Rcnt.rate). Dotclock (Timer0) and hblank (Timer1) derive from
 * the GPU's active video mode (PAL/NTSC) — see gpu_dotclock_hz/gpu_hblank_hz. */
static uint32_t timer_rate_cycles(Timers* timers, Timer* t, int i) {
    const Gpu* gpu = timers->inter ? &timers->inter->gpu : NULL;
    /* The two source bits do not mean the same thing in every counter
     * (timers.md:34-36):
     *   Counter 0:  0 or 2 = System Clock,  1 or 3 = Dotclock
     *   Counter 1:  0 or 2 = System Clock,  1 or 3 = Hblank
     *   Counter 2:  0 or 1 = System Clock,  2 or 3 = System Clock/8
     * Reading "0 and 1 are the system clock" out of counter 2's row and
     * applying it to all three left Timer 1 running at the CPU clock whenever a
     * game selected source 1, which is the usual way to ask for Hblank. */
    switch (i) {
        case 0: { /* Timer0: dotclock on an odd source */
            if (!(t->clock_source & 1)) return 1;
            double hz = gpu ? gpu_dotclock_hz(gpu) : DOTCLOCK_NTSC_HZ;
            uint32_t r = (uint32_t)(PSX_CPU_HZ / hz + 0.5);
            return r ? r : 1;
        }
        case 1: { /* Timer1: one tick per scanline on an odd source */
            if (!(t->clock_source & 1)) return 1;
            double hz = gpu ? gpu_hblank_hz(gpu) : (PSX_CPU_HZ / (60.0 * 263.0));
            uint32_t r = (uint32_t)(PSX_CPU_HZ / hz + 0.5);
            return r ? r : 1;
        }
        default: return (t->clock_source & 2) ? 8 : 1;   /* Timer2 */
    }
}

/* Tick distance from counter==0 back to the reset point. reset_on_target with
 * a non-zero target resets at target; otherwise it wraps at 0x10000. */
static uint32_t timer_period_ticks(const Timer* t) {
    if (t->reset_on_target && t->target != 0) return (uint32_t)t->target;
    return 0x10000u;
}

/* Fire the timer IRQ, honoring pulse/toggle mode, the already-pending guard,
 * and one-shot (irq_repeat==0) disable. Extracted so the event handler and the
 * on-read catch-up share ONE path (the old code had two conflicting guards). */
static void timer_fire_irq(Timers* timers, Timer* t, int i, bool hit_target, bool hit_overflow) {
    bool already_pending = timers->inter &&
        (timers->inter->irq_status & (1u << t->irq)) != 0;

    if (t->irq_pulse) {
        /* Toggle mode: bit10 flips; CPU IRQ only on the 0->1 transition. */
        if (t->mode & (1 << 10)) {
            t->mode &= ~(1 << 10);
        } else {
            t->mode |= (1 << 10);
            LOG_TIMER_DEBUG("[TIMER] Timer%d IRQ toggle->1 (tgt=%d ov=%d)", i, hit_target, hit_overflow);
            if (!already_pending) interconnect_request_irq(timers->inter, t->irq, "Timer IRQ (toggle)");
        }
    } else {
        t->mode |= (1 << 10);
        LOG_TIMER_DEBUG("[TIMER] Timer%d IRQ pulse (tgt=%d ov=%d)", i, hit_target, hit_overflow);
        if (!already_pending) interconnect_request_irq(timers->inter, t->irq, "Timer IRQ (pulse)");
    }

    if (!t->irq_repeat) {  /* one-shot: disable after first fire */
        t->irq_on_target = false;
        t->irq_on_ffff   = false;
        t->mode &= ~((1 << 4) | (1 << 5));
    }
}

/* Catch a single timer up to the current cycle: cross any elapsed boundaries
 * (firing IRQ + rebasing cycle_start each time), then refresh the cached
 * counter value. Idempotent — re-entry after a boundary sees ticks<period and
 * does nothing, so the read path and the event path can both call it without
 * double-firing. No effect while paused (sync gate) or before inter is wired. */
static void timers_catch_up_one(Timers* timers, int i) {
    Timer* t = &timers->timers[i];
    if (!timers->inter || !t->counting_enabled) return;
    uint32_t rate = timer_rate_cycles(timers, t, i);
    if (rate == 0) rate = 1;
    t->rate = rate;

    const uint32_t now     = timers->inter->cpu_cycle_counter;
    const uint32_t elapsed = (now - t->cycle_start) / rate;   /* ticks since last base */
    uint32_t period        = timer_period_ticks(t);
    if (period == 0) period = 1;          /* cannot happen today; a div-by-zero if it ever could */

    if (elapsed < period) { t->counter = (uint16_t)elapsed; return; }

    /* How many boundaries were crossed, in one division.
     *
     * This used to be a `for(;;)` that stepped one period at a time, which is
     * O(elapsed/period) — unbounded. A timer with a small target that is read
     * after a long gap, or re-enabled after one, walks hundreds of thousands of
     * iterations; the event scheduler normally keeps elapsed under one period,
     * so the cost never showed up in a profile and the hazard stayed hidden.
     * Every effect of those iterations collapses, because no guest code runs
     * between them:
     *   - the reached_* flags are sticky, so setting them once is the same;
     *   - a pulse IRQ sets mode bit 10 and is then suppressed by the
     *     already-pending guard in timer_fire_irq, so one request is the same;
     *   - a toggle IRQ flips bit 10 per crossing, so only the parity survives,
     *     and only the first 0->1 among them can reach the interrupt controller;
     *   - one-shot (irq_repeat == 0) disarms itself on the first fire, so later
     *     crossings do nothing at all.
     * The rebase is then one multiply. */
    const uint32_t crossings = elapsed / period;

    const bool hit_target   = (t->reset_on_target && t->target != 0);
    const bool hit_overflow = !hit_target;
    if (hit_target)   t->reached_target_flag = true;
    if (hit_overflow) t->reached_ffff_flag   = true;

    if ((hit_target && t->irq_on_target) || (hit_overflow && t->irq_on_ffff)) {
        /* First crossing goes through the unchanged path, so pulse/toggle
         * handling, the already-pending guard, the log line and the one-shot
         * disarm all behave exactly as they did. */
        timer_fire_irq(timers, t, i, hit_target, hit_overflow);

        /* Crossings 2..N. One-shot has disarmed itself above, so re-testing the
         * arm bits is what decides whether there are any. */
        const bool still_armed = (hit_target && t->irq_on_target) ||
                                 (hit_overflow && t->irq_on_ffff);
        const uint32_t rest = crossings - 1u;
        if (still_armed && rest) {
            if (t->irq_pulse) {
                /* Toggle mode: bit 10 flips once per crossing. Only the parity
                 * of the remainder is observable, plus whether any of those
                 * flips was a 0->1 — the transition that can raise the line. */
                const bool bit_set = (t->mode & (1u << 10)) != 0;
                const bool rises   = bit_set ? (rest >= 2u) : (rest >= 1u);
                if (rest & 1u) t->mode ^= (1u << 10);
                if (rises && timers->inter &&
                    (timers->inter->irq_status & (1u << t->irq)) == 0) {
                    interconnect_request_irq(timers->inter, t->irq, "Timer IRQ (toggle)");
                }
            } else {
                t->mode |= (1u << 10);   /* pulse mode: idempotent */
            }
        }
    }

    /* Rebase to the last boundary crossed. crossings*period <= elapsed and
     * elapsed*rate <= now - cycle_start, so this cannot overflow. */
    t->cycle_start += crossings * period * rate;
    t->counter = (uint16_t)(elapsed - crossings * period);
}

/* Arm EVQ_TIMER{i} to fire at this timer's next target/overflow. Skipped while
 * the timer is gated off (no event ⇒ it simply won't fire until re-enabled). */
void timers_reschedule(Timers* timers, int i) {
    Timer* t = &timers->timers[i];
    if (!timers->inter || !t->counting_enabled) return;
    uint32_t rate = timer_rate_cycles(timers, t, i);
    if (rate == 0) rate = 1;
    t->rate = rate;
    uint32_t period    = timer_period_ticks(t);
    uint32_t remaining = (t->counter < period) ? (period - t->counter) : 1;
    eventq_schedule(timers->inter, EVQ_TIMER0 + i, remaining * rate);
}

/* Legacy name kept for the write path; now just reschedules. */
void timers_schedule_next_event(Timers* timers, int timer_index) {
    timers_reschedule(timers, timer_index);
}

/* Anchor all three timers at the current cycle and arm their first events. */
void timers_start(Timers* timers) {
    for (int i = 0; i < 3; i++) {
        timer_rebase(timers, i);
        timers_reschedule(timers, i);
    }
}

// --- Lightgun/Scanline IRQ10 X coordinate conversion (PSX-Spex/nocash) ---
static inline float timer0_to_x_coord(uint16_t timer0, bool is_ntsc) {
    // Subtract 140 as per docs, then apply region-specific factor
    float base = (float)timer0 - 140.0f;
    return base * (is_ntsc ? 0.198166f : 0.196358f);
}

// --- BEGIN: Timer Event Handlers ---
// Structure inspired by PCSX ReARMed's timer handling (GPL-2.0-or-later,
// Copyright (c) PCSX ReARMed authors). The counter model itself is derived
// on read from DOCS/timers.md rather than ticked, so this is the scheduling
// shape rather than the arithmetic.
// These handlers are called by the event queue when a timer event fires.
static void timer_event_handler(Timers* timers, int timer_index) {
    /* The event fires at the timer's next boundary: catch it up (which crosses
     * the boundary, sets sticky flags, and fires the IRQ through the single
     * shared path), then re-arm for the following boundary. */
    timers_catch_up_one(timers, timer_index);
    timers_reschedule(timers, timer_index);
}

// Expose C-callable wrappers for event_scheduler.c
void timer0_event_handler(struct Interconnect* sys) { timer_event_handler(&sys->timers_state, 0); }
void timer1_event_handler(struct Interconnect* sys) { timer_event_handler(&sys->timers_state, 1); }
void timer2_event_handler(struct Interconnect* sys) { timer_event_handler(&sys->timers_state, 2); }
// --- END: PCSX ReARMed-inspired Timer Event Handlers ---

