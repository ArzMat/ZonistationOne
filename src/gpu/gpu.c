/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 ZioZoni95
 *
 * Part of ZoniStation One, a PlayStation 1 emulator.
 * See LICENSE for the full licence text and THIRD-PARTY.md for the
 * components of this project that have other authors.
 */
/**
 * gpu.c
 * PlayStation GPU state management: init/reset, GP1 command handlers,
 * GPUSTAT register, GPUREAD data port, and display mapping updates.
 *
 * GP0 command handling is in gpu_commands.c (modular split).
 */

#include "gpu.h"
#include "renderer.h"
#include "vram.h"
#include "log.h"
#include "interconnect.h"
#include "timers.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Internal helpers shared with gpu_commands.c  (implementations are in
// gpu_commands.c; we just use them via the non-static declarations in gpu.h)
// ---------------------------------------------------------------------------
// gpu_clear_cmd_buf()  — in gpu_commands.c
// gpu_push_cmd_word()  — in gpu_commands.c

// ---------------------------------------------------------------------------
// Helper: update renderer screen scale and drawing offset
// Uses the CRTC-derived display area (set by GP1(05/06/07/08)).
// ---------------------------------------------------------------------------
/* Horizontal cycles per pixel for the active resolution: 10/8/5/4 for
 * 256/320/512/640 and 7 for 368, where GPUSTAT bit6 (hr2) wins over bits 0-1
 * (DOCS/graphicsprocessingunitgpu.md:735-742). Same divider the dotclock uses,
 * so both read it from here. */
static uint32_t gpu_cycles_per_pixel(const Gpu* gpu) {
    if (gpu->hres_raw.hr2) return 7;
    switch (gpu->hres_raw.hr1) {
        case 0:  return 10;   /* 256 */
        case 1:  return 8;    /* 320 */
        case 2:  return 5;    /* 512 */
        default: return 4;    /* 640 */
    }
}

/* Overscan crop: a TV hides roughly 8 lines at the top and bottom of the
 * picture, and games park stale VRAM there — which is the strip along the top
 * of an FMV (docs/GPU_DISPLAY_STUDY_2026-08-10.md §2.1). DuckStation runs its
 * reference with CropMode=Overscan, and comparisons against it only line up
 * with the crop on. ZS1_OVERSCAN=0 turns it off. */
/* GP1(03) writes since the last field boundary, reported by gpu_crtc_tick().
 * The long black screens across a load are either the game blanking the display
 * or us mishandling the blank, and this count is what tells the two apart
 * (docs/GPU_DISPLAY_STUDY_2026-08-10.md §2.3). Instrumentation only — a
 * file-static rather than a Gpu field, so it stays out of the savestate. */
static uint32_t s_gp1_03_writes_this_field = 0;

/* Overscan is an NTSC property, not a display-wide one: "Many NTSC games
 * display 240 lines, but on most analog television sets, only 224 lines are
 * visible (8 lines of overscan on top and 8 lines of overscan on bottom). Many
 * PAL games display only 256 lines (underscan with black borders)"
 * (DOCS/graphicsprocessingunitgpu.md:716-719).
 *
 * Cropping a PAL field therefore throws away picture the game drew and the TV
 * would have shown — the underscan borders are already in the 256 lines. It
 * cost the bottom line of the SCEE screen ("www.playstation.com") before this
 * was restricted to NTSC. */
#define GPU_OVERSCAN_LINES 8u
static bool gpu_overscan_crop(const Gpu* gpu) {
    static int cached = -1;
    if (cached < 0) {
        const char* v = getenv("ZS1_OVERSCAN");
        cached = (v && (v[0] == '0' || v[0] == 'n' || v[0] == 'N')) ? 0 : 1;
    }
    return cached != 0 && gpu->vmode == Ntsc;
}

void gpu_update_display_mapping(Gpu* gpu) {
    /* Width comes from the GP1(06) horizontal range, not from the resolution
     * index: (((X2-X1)/cycles_per_pix)+2) AND NOT 3
     * (DOCS/graphicsprocessingunitgpu.md:687-690). Taking it from GP1(08)
     * instead displayed every PAL game with non-default centering at a width it
     * never asked for, and made GP1(06) screen-shake effects do nothing. The
     * resolution index survives only as the fallback for the state before a
     * game has written a range. */
    const uint32_t cyc_per_pix     = gpu_cycles_per_pixel(gpu);
    const uint32_t scanline_cycles = (gpu->vmode == Pal) ? 3406u : 3413u;
    uint32_t x1 = gpu->display_horiz_start;
    uint32_t x2 = gpu->display_horiz_end;
    if (x2 > scanline_cycles) x2 = scanline_cycles;   /* range ends clamp to the line */
    uint32_t w = 0;
    if (x2 > x1) w = (((x2 - x1) / cyc_per_pix) + 2u) & ~3u;
    if (w == 0) w = gpu->display_width_hint ? gpu->display_width_hint : 320u;
    if (w > VRAM_WIDTH) w = VRAM_WIDTH;
    uint16_t disp_w = (uint16_t)w;

    /* Height is Y2-Y1 with no rounding (:707-709), clamped to the frame, and
     * doubled when interlace is on. */
    const uint32_t total_lines = (gpu->vmode == Pal) ? 314u : 263u;
    uint32_t y1 = gpu->display_line_start;
    uint32_t y2 = gpu->display_line_end;
    if (y2 > total_lines) y2 = total_lines;
    uint32_t h = (y2 > y1) ? (y2 - y1) : 240u;
    /* The VRAM rectangle is twice the window only in 480-lines mode, which is
     * 480 lines *and* interlace — not interlace on its own.
     *
     * GP1(05h) states the size flatly, with no interlace term at all:
     * "size=((X2-X1)/cycles_per_pix), (Y2-Y1)"
     * (psx-spx-docs/docs/graphicsprocessingunitgpu.md:703-704). What makes a
     * 480-line framebuffer is GP1(08h).2, and that bit is documented as
     * "Vertical Resolution (0=240, 1=480, when Bit5=1)" (:772) — it needs
     * interlace to mean anything, but interlace does not imply it. GPUSTAT.31
     * settles the direction: "In 480-lines mode, bit31 changes per frame. And
     * in 240-lines mode, the bit changes per scanline" (:919-920). Per frame
     * means each field takes a different half of a 480-line buffer; per
     * scanline means both fields walk the same (Y2-Y1) lines.
     *
     * pcsx-redux doubles on the interlace bit alone (display.cc:111-113). That
     * is what we copied, and on a 240-line title with interlace on it scans out
     * twice the window: the picture fills the top half of the screen and the
     * rest is whatever else happens to be in VRAM — textures, an old frame —
     * which is what "the VRAM shows up instead of a black screen" looked like
     * in Monsters & Co. */
    if (gpu->interlaced && gpu->vres == Y480Lines) h *= 2u;
    if (h > VRAM_HEIGHT) h = VRAM_HEIGHT;

    uint16_t vram_y = gpu->display_vram_y_start;
    if (gpu_overscan_crop(gpu)) {
        uint32_t crop = GPU_OVERSCAN_LINES *
                        ((gpu->interlaced && gpu->vres == Y480Lines) ? 2u : 1u);
        if (h > 2u * crop) {
            h -= 2u * crop;
            vram_y = (uint16_t)((vram_y + crop) & (VRAM_HEIGHT - 1));
        }
    }
    uint16_t disp_h = (uint16_t)h;

    // Store computed display area into CRTC state for the VBlank blit path.
    gpu->crtc.display_vram_x = gpu->display_vram_x_start;
    gpu->crtc.display_vram_y = vram_y;
    gpu->crtc.display_width  = disp_w;
    gpu->crtc.display_height = disp_h;

    // Keep full VRAM dimensions so polygon coordinates (0..1023 × 0..511) are
    // visible — full VRAM view, VRAM objects remain visible as the user prefers.
    renderer_set_screen_scale(&gpu->renderer, VRAM_WIDTH, VRAM_HEIGHT);
    renderer_set_draw_offset(&gpu->renderer, gpu->drawing_x_offset, gpu->drawing_y_offset);
    renderer_set_display_region(&gpu->renderer,
        gpu->crtc.display_vram_x, gpu->crtc.display_vram_y,
        gpu->crtc.display_width,  gpu->crtc.display_height);
    renderer_set_display_depth24(&gpu->renderer, gpu->display_depth == D24Bits);
    /* Also the path that re-syncs the blank flag after a reset or a savestate
     * load, neither of which goes through GP1(03). */
    renderer_set_display_blank(&gpu->renderer, gpu->display_disabled);

    /* Probe: the whole VRAM-to-screen mapping on one line, inputs and outputs
     * together, emitted only when the result changes — this function is called
     * from four GP1 handlers and would otherwise repeat itself several times a
     * field. Geometry only: nothing here is a timing measurement.
     *
     * Reading it: `win` is what the game asked for via GP1(06)/(07) in CRTC
     * cycles and scanlines, `out` is the VRAM rectangle we scan out. If out.h is
     * twice the height of the picture actually on screen, the interlace doubling
     * below is firing on a 240-line mode; if out.y is 0 while the frames land at
     * y=8, the window and the upload disagree. */
    {
        /* A seen-set, not a single previous value. A double-buffered game flips
         * the display address between two halves of VRAM every field — Crash 3
         * alternates out x=0 and x=512 — so a one-slot gate is re-armed by the
         * flip itself and the line comes back ~25 times a second. Holding the
         * states already reported lets the pair log once each and then go quiet,
         * while a genuinely new mapping still gets through. Same idiom as the
         * GP0(E1)/GP0(E2) gates in gpu_commands.c. */
        enum { DISPMAP_SEEN_MAX = 16 };
        static uint64_t seen[DISPMAP_SEEN_MAX];
        static int      seen_n = 0;
        uint64_t key = ((uint64_t)gpu->crtc.display_vram_x << 48)
                     | ((uint64_t)gpu->crtc.display_vram_y << 32)
                     | ((uint64_t)disp_w << 16) | (uint64_t)disp_h;
        bool known = false;
        for (int i = 0; i < seen_n; i++) if (seen[i] == key) { known = true; break; }
        if (!known && seen_n < DISPMAP_SEEN_MAX) {
            seen[seen_n++] = key;
            LOG_GPU_INFO("[GPU] display map: win x1=%u x2=%u y1=%u y2=%u cyc=%u | "
                         "out x=%u y=%u w=%u h=%u | %s vres=%s %s %s",
                         x1, x2, y1, y2, cyc_per_pix,
                         gpu->crtc.display_vram_x, gpu->crtc.display_vram_y,
                         disp_w, disp_h,
                         gpu->interlaced ? "interlaced" : "progressive",
                         (gpu->vres == Y480Lines) ? "480" : "240",
                         (gpu->display_depth == D24Bits) ? "24bpp" : "15bpp",
                         gpu->display_disabled ? "BLANK" : "on");
        }
    }
}

// ---------------------------------------------------------------------------
// CRTC tick — advance scanline counter and STAT[31] once per VBlank period.
// Call from evq_handle_vblank() with the number of CPU cycles since last VBlank.
// ---------------------------------------------------------------------------
#define CPU_CYCLES_PER_SCANLINE 2146u

/* CPU cycles in one video frame, for the current video mode.
 *
 * Derived from the real clock relationship rather than assumed: the GPU runs
 * 3413 ticks × 263 lines (NTSC) or 3406 × 314 (PAL) per frame, and GPU ticks
 * convert to system ticks by sysclk*715909/451584 (NTSC) / sysclk*709379/451584
 * (PAL) (DOCS/graphicsprocessingunitgpu.md:1305-1306). That gives 566203 cycles
 * (59.82 Hz) NTSC and 680823 (49.75 Hz) PAL.
 *
 * This used to be a single hardcoded 564480 (= 33868800/60) for both modes, so
 * every PAL title ran its whole timebase — VBlank rate, and with it FMV and
 * audio pacing — about 20% too fast. */
uint32_t gpu_cycles_per_frame(const Gpu* gpu) {
    return (gpu->vmode == Pal) ? 680823u : 566203u;
}

/* CRTC (video) clock in Hz for the active video mode — the source both the
 * dotclock and the hblank/line clock divide down from: NTSC 53'693'175,
 * PAL 53'203'425 (measured on hardware, DOCS/graphicsprocessingunitgpu.md). */
static double gpu_crtc_hz(const Gpu* gpu) {
    return (gpu->vmode == Pal) ? 53203425.0 : 53693175.0;
}

/* Timer0 dotclock rate. dot = CRTC / divider, divider chosen by the GPUSTAT
 * horizontal-resolution field ({10,8,5,4,7} → 256/320/512/640/368,
 * DOCS/graphicsprocessingunitgpu.md:1325-1335). */
double gpu_dotclock_hz(const Gpu* gpu) {
    /* Same divider the display window uses — one decoder for both, so 368 mode
     * cannot decode one way here and another way there. */
    return gpu_crtc_hz(gpu) / (double)gpu_cycles_per_pixel(gpu);
}

/* Timer1 hblank rate = scanline rate = CRTC / ticks-per-line
 * (3413 NTSC / 3406 PAL, DOCS/graphicsprocessingunitgpu.md:1305-1306). */
double gpu_hblank_hz(const Gpu* gpu) {
    return gpu_crtc_hz(gpu) / ((gpu->vmode == Pal) ? 3406.0 : 3413.0);
}

void gpu_crtc_tick(Gpu* gpu, uint32_t cpu_cycles_elapsed) {
    if (s_gp1_03_writes_this_field) {
        LOG_GPU_DEBUG("[GPU] GP1(03) writes this field: %u (display %s)",
                      s_gp1_03_writes_this_field,
                      gpu->display_disabled ? "off" : "on");
        s_gp1_03_writes_this_field = 0;
    }

    // Update vertical total from current video mode
    gpu->crtc.vertical_total = (gpu->vmode == Pal) ? 314 : 263;

    // Advance scanline counter (at least 1 scanline per call)
    uint32_t scanlines = cpu_cycles_elapsed / CPU_CYCLES_PER_SCANLINE;
    if (scanlines == 0) scanlines = 1;
    gpu->crtc.current_scanline = (uint16_t)(
        ((uint32_t)gpu->crtc.current_scanline + scanlines) % gpu->crtc.vertical_total);

    // Update in_vblank from vertical display range (set by GP1(07))
    if (gpu->display_line_start < gpu->display_line_end) {
        gpu->crtc.in_vblank = (gpu->crtc.current_scanline < gpu->display_line_start ||
                               gpu->crtc.current_scanline >= gpu->display_line_end);
    } else {
        gpu->crtc.in_vblank = false;
    }

    // Toggle STAT[31] (odd/even line) each VBlank period
    gpu->crtc.active_line_lsb ^= 1u;

    // In interlaced 480i mode: also alternate the field tag
    if (gpu->interlaced && gpu->vres == Y480Lines) {
        gpu->crtc.interlaced_field ^= 1u;
        gpu->field = gpu->crtc.interlaced_field ? Top : Bottom;
    }

    LOG_GPU_DEBUG("[GPU] tick: scanline=%u vblank=%d lsb=%u",
                  gpu->crtc.current_scanline, (int)gpu->crtc.in_vblank,
                  gpu->crtc.active_line_lsb);

    // Timer1's external gate is VBlank (PSX-SPX). Timer0's gate is hblank,
    // which this CRTC model doesn't track at per-scanline granularity yet, so it
    // isn't wired up here.
    if (gpu->inter)
        timers_set_gate(&gpu->inter->timers_state, 1, gpu->crtc.in_vblank);
}

// ---------------------------------------------------------------------------
// GP1 handlers
// ---------------------------------------------------------------------------

static void gp1_reset(Gpu* gpu, uint32_t value) {
    (void)value;
    LOG_GPU_DEBUG("[GPU] GPU: Soft Reset (GP1 0x00)");
    gpu_soft_reset(gpu);
}

static void gp1_reset_command_buffer(Gpu* gpu, uint32_t value) {
    (void)value;
    LOG_GPU_DEBUG("[GPU] GPU: Reset Command Buffer (GP1 0x01)");
    gpu_clear_cmd_buf(gpu);
    gpu->gp0_words_remaining = 0;
    gpu->gp0_mode = GP0_MODE_COMMAND;
    gpu->gp0_current_opcode = 0xFF;
    gpu->gp0_command_method = NULL;
    gpu->gp0_fifo_head = 0; gpu->gp0_fifo_tail = 0; gpu->gp0_fifo_count = 0;
}

static void gp1_acknowledge_irq(Gpu* gpu, uint32_t value) {
    (void)value;
    gpu->interrupt = false;
    /* Deassert the GPU IRQ line so the edge detector can latch the next
     * GP0(0x1F) — pairs with the raise in gp0_interrupt_request. */
    if (gpu->inter)
        interconnect_set_irq_line(gpu->inter, IRQ_GPU, false);
    LOG_GPU_DEBUG("[GPU] GPU: Acknowledge IRQ (GP1 0x02)");
}

static void gp1_display_enable(Gpu* gpu, uint32_t value) {
    gpu->display_disabled = (value & 1) != 0;
    /* Off means a black picture on hardware (DOCS/graphicsprocessingunitgpu.md:647),
     * not "keep showing what the display window covers". */
    renderer_set_display_blank(&gpu->renderer, gpu->display_disabled);
    s_gp1_03_writes_this_field++;
    LOG_GPU_DEBUG("[GPU] GPU: Display %s (GP1 0x03)", gpu->display_disabled ? "Disabled" : "Enabled");
}

static void gp1_dma_direction(Gpu* gpu, uint32_t value) {
    switch (value & 3) {
        case 0: gpu->dma_setting = GPU_DMA_Off;       break;
        case 1: gpu->dma_setting = GPU_DMA_Fifo;      break;
        case 2: gpu->dma_setting = GPU_DMA_CpuToGp0;  break;
        case 3: gpu->dma_setting = GPU_DMA_VRamToCpu; break;
    }
    LOG_GPU_DEBUG("[GPU] GPU: DMA Direction = %d (GP1 0x04)", gpu->dma_setting);
}

static void gp1_display_vram_start(Gpu* gpu, uint32_t value) {
    /* X is a halfword address in bits 0-9, full 0..1023 range
     * (DOCS/graphicsprocessingunitgpu.md:670-676). Masking it to 0x3FE dropped
     * every odd start, which is a one-pixel horizontal shift some games use. */
    gpu->display_vram_x_start = (uint16_t)(value & 0x3FF);
    gpu->display_vram_y_start = (uint16_t)((value >> 10) & 0x1FF);
    LOG_GPU_DEBUG("[GPU] Display address start <- 0x%08x", value & 0x000FFFFF);
    gpu_update_display_mapping(gpu);
}

static void gp1_display_horizontal_range(Gpu* gpu, uint32_t value) {
    gpu->display_horiz_start = (uint16_t)(value & 0xFFF);
    gpu->display_horiz_end   = (uint16_t)((value >> 12) & 0xFFF);
    LOG_GPU_DEBUG("[GPU] Horizontal display range <- 0x%08x", value & 0x00FFFFFF);
    gpu_update_display_mapping(gpu);
}

static void gp1_display_vertical_range(Gpu* gpu, uint32_t value) {
    gpu->display_line_start = (uint16_t)(value & 0x3FF);
    gpu->display_line_end   = (uint16_t)((value >> 10) & 0x3FF);
    LOG_GPU_DEBUG("[GPU] Vertical display range <- 0x%08x", value & 0x000FFFFF);
    gpu_update_display_mapping(gpu);
}

static void gp1_display_mode(Gpu* gpu, uint32_t value) {
    gpu->hres_raw.hr1   = (uint8_t)(value & 3);
    gpu->hres_raw.hr2   = (uint8_t)((value >> 6) & 1);
    gpu->vres           = ((value >> 2) & 1) ? Y480Lines : Y240Lines;
    gpu->vmode          = ((value >> 3) & 1) ? Pal : Ntsc;
    gpu->display_depth  = ((value >> 4) & 1) ? D24Bits : D15Bits;
    gpu->interlaced     = ((value >> 5) & 1) != 0;
    if ((value >> 7) & 1)
        LOG_GPU_WARN("[GPU] GP1(0x08): Reverseflag bit set (unsupported)");

    /* Bit6 (hr2) means 368 regardless of bits 0-1; the old
     * `hr1 | (hr2 << 2)` switch fell through to 256 whenever both were set. */
    uint16_t width;
    if (gpu->hres_raw.hr2) {
        width = 368;
    } else switch (gpu->hres_raw.hr1) {
        case 0:  width = 256; break;
        case 1:  width = 320; break;
        case 2:  width = 512; break;
        default: width = 640; break;
    }
    /* Same rule as the display mapping: 480 lines needs both bits (:772). */
    uint16_t height = (gpu->interlaced && gpu->vres == Y480Lines) ? 480 : 240;
    gpu->display_width_hint  = width;
    gpu->display_height_hint = height;
    gpu_update_display_mapping(gpu);
    LOG_GPU_DEBUG("[GPU] Set display mode <- 0x%08x", value & 0xFF);
}

// ---------------------------------------------------------------------------
// GP1(0x10–0x1F): GetGPUInfo — stores result into gpu_info_latch for GPUREAD
// ---------------------------------------------------------------------------
/* The register indices, as a v2 GPU answers them (psx-spx
 * gpu/display-control-commands-gp1.md:191-203; gpu/versions.md:11-14 for the
 * v2 column), which is the GPU this core reports itself as through index 7:
 *   00h-01h  nothing (the latch keeps its old value)
 *   02h      texture window, GP0(E2h)          20 bits, MSBs = nothing
 *   03h      drawing area top left, GP0(E3h)   20 bits, MSBs = nothing
 *   04h      drawing area bottom right, GP0(E4h) 20 bits, MSBs = nothing
 *   05h      drawing offset, GP0(E5h)          22 bits
 *   06h      nothing
 *   07h      GPU version: 2
 *   08h      00000000h
 *   09h-0Fh  nothing
 *   10h-FFFFFFh mirrors of 00h..0Fh
 * "Nothing" is defined there as "old value in GP0.read remains unchanged", so
 * for 02h-04h the bits above the 20 returned ones are kept from the old latch.
 *
 * This used to answer one index late (02h gave the texture page, 03h the
 * texture window, 04h the top left, 05h the bottom right, 06h the offset) and
 * to zero the latch for the indices that return nothing. Software that probes
 * the drawing area or the offset this way (GPU detection reads index 4, then
 * 7: versions.md:70-79) got the wrong register. */
static void gp1_get_gpu_info(Gpu* gpu, uint32_t value) {
    const uint32_t index = value & 0xF;
    const uint32_t old   = gpu->gpu_info_latch;
    uint32_t result;
    switch (index) {
        case 0x02:
            result  = (uint32_t)gpu->texture_window_x_mask;
            result |= (uint32_t)gpu->texture_window_y_mask   << 5;
            result |= (uint32_t)gpu->texture_window_x_offset << 10;
            result |= (uint32_t)gpu->texture_window_y_offset << 15;
            result  = (old & ~0xFFFFFu) | (result & 0xFFFFFu);
            break;
        case 0x03:
            result  = (uint32_t)gpu->drawing_area_left;
            result |= (uint32_t)gpu->drawing_area_top << 10;
            result  = (old & ~0xFFFFFu) | (result & 0xFFFFFu);
            break;
        case 0x04:
            result  = (uint32_t)gpu->drawing_area_right;
            result |= (uint32_t)gpu->drawing_area_bottom << 10;
            result  = (old & ~0xFFFFFu) | (result & 0xFFFFFu);
            break;
        case 0x05:
            result  = (uint32_t)(gpu->drawing_x_offset & 0x7FF);
            result |= (uint32_t)((gpu->drawing_y_offset & 0x7FF) << 11);
            break;
        case 0x07:
            result = 0x00000002;   /* GPU type: version 2 */
            break;
        case 0x08:
            result = 0x00000000;
            break;
        default:                   /* 00h, 01h, 06h, 09h-0Fh: nothing */
            result = old;
            break;
    }
    gpu->gpu_info_latch = result;
    LOG_GPU_DEBUG("[GPU] GP1(0x10): GetGPUInfo index=%u -> 0x%08x", index, result);
}

// ---------------------------------------------------------------------------
// Public GP1 entry point
// ---------------------------------------------------------------------------
void gpu_gp1(Gpu* gpu, uint32_t command) {
    /* "GP1(40h..FFh) - N/A (Mirrors) Mirrors of GP1(00h..3Fh)" (psx-spx
     * gpu/display-control-commands-gp1.md:243-244). */
    uint32_t opcode = (command >> 24) & 0x3F;
    switch (opcode) {
        case 0x00: gp1_reset(gpu, command); break;
        case 0x01: gp1_reset_command_buffer(gpu, command); break;
        case 0x02: gp1_acknowledge_irq(gpu, command); break;
        case 0x03: gp1_display_enable(gpu, command); break;
        case 0x04: gp1_dma_direction(gpu, command); break;
        case 0x05: gp1_display_vram_start(gpu, command); break;
        case 0x06: gp1_display_horizontal_range(gpu, command); break;
        case 0x07: gp1_display_vertical_range(gpu, command); break;
        case 0x08: gp1_display_mode(gpu, command); break;
        case 0x09:
            // GP1(0x09): Allow texture disable (stored in texture_disable controlled via GP0(E1))
            LOG_GPU_DEBUG("[GPU] GP1(0x09): Allow texture disable (no-op in LLE)");
            break;
        case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15:
        case 0x16: case 0x17: case 0x18: case 0x19: case 0x1A: case 0x1B:
        case 0x1C: case 0x1D: case 0x1E: case 0x1F:
            gp1_get_gpu_info(gpu, command);
            break;
        default:
            LOG_GPU_WARN("[GPU] Unhandled GP1 opcode 0x%02x (cmd 0x%08x) @ 0x%08x", opcode, command);
            break;
    }
}

// ---------------------------------------------------------------------------
// GPUSTAT register read
// ---------------------------------------------------------------------------
uint32_t gpu_read_status(Gpu* gpu) {
    uint32_t r = 0;
    r |= (uint32_t)gpu->page_base_x << 0;
    r |= (uint32_t)gpu->page_base_y << 4;
    r |= (uint32_t)gpu->semi_transparency << 5;
    r |= (uint32_t)gpu->texture_depth << 7;
    r |= (uint32_t)gpu->dithering << 9;
    r |= (uint32_t)gpu->draw_to_display << 10;
    r |= (uint32_t)gpu->force_set_mask_bit << 11;
    r |= (uint32_t)gpu->preserve_masked_pixels << 12;
    /* "Interlace Field (or, always 1 when GP1(08h).5=0)" (:897), and the GPU
     * version table repeats it for v2: "GPUSTAT.13 when interlace=off — always
     * 1" (:953). Without the override the tag stays wherever 480i left it, so a
     * game that switches interlace off reads 0 there for the rest of the run. */
    r |= (uint32_t)(gpu->interlaced ? gpu->field : 1u) << 13;
    r |= (uint32_t)gpu->texture_disable << 15;
    /* GPUSTAT.16 is Horizontal Resolution 2 (GP1(08h).6) and GPUSTAT.17-18 are
     * Horizontal Resolution 1 (GP1(08h).0-1)
     * (psx-spx-docs/docs/graphicsprocessingunitgpu.md:902-904).
     *
     * This used to pack `(hr2 << 2) | hr1` and then scatter that word's bits 0,
     * 1, 2 to GPUSTAT 16, 17, 18 — which puts hr1's low bit in the 368 flag and
     * hr2 in the top bit of the resolution index. Every resolution read back by
     * a game was wrong, and it read as 368 for 320 and 640 alike. Found by
     * cross-checking scripts/display_map_probe.lua, which derives the width
     * from GPUSTAT, against the width gpu_update_display_mapping() computes from
     * the same registers: 364 against 320 for the same field. */
    r |= (uint32_t)(gpu->hres_raw.hr2 & 1) << 16;
    r |= (uint32_t)(gpu->hres_raw.hr1 & 3) << 17;
    r |= ((uint32_t)(gpu->vres == Y480Lines ? 1 : 0)) << 19;
    r |= ((uint32_t)gpu->vmode) << 20;
    r |= ((uint32_t)gpu->display_depth) << 21;
    r |= ((uint32_t)gpu->interlaced) << 22;
    r |= ((uint32_t)gpu->display_disabled) << 23;
    // Bit 24: GPU IRQ flag — use actual state (cleared by GP1(02))
    r |= ((uint32_t)gpu->interrupt) << 24;
    // DMA request (bit 25)
    if (gpu->dma_setting != GPU_DMA_Off) r |= (1 << 25);
    // GPU-1 FIX: Correct ready bits and DMA request per GPUSTAT spec.

    // Bit 26: Ready to receive Cmd Word — always 1 (synchronous emulator, never busy).
    bool ready_to_recv_cmd = true;
    if (ready_to_recv_cmd) r |= (1u << 26);

    // Bit 27: Ready to send VRAM to CPU — only when IMAGE_STORE transfer is pending.
    bool vram_send_ready = (gpu->gp0_mode == GP0_MODE_IMAGE_STORE && gpu->gp0_words_remaining > 0);
    if (vram_send_ready) r |= (1u << 27);

    // Bit 28: Ready to receive DMA Block — always 1 (synchronous emulator).
    bool dma_recv_ready = true;
    if (dma_recv_ready) r |= (1u << 28);

    // Bit 25: DMA / Data Request — meaning depends on GP1(04h) DMA Direction:
    //   Off       → Always 0
    //   FIFO(1)   → FIFO state: 0=Full, 1=Not Full  (same as bit28)
    //   CpuToGP0  → Same as GPUSTAT.28
    //   VRamToCPU → Same as GPUSTAT.27
    {
        bool dma_request;
        switch (gpu->dma_setting) {
            case GPU_DMA_Off:       dma_request = false;           break;
            case GPU_DMA_Fifo:      dma_request = dma_recv_ready;  break;
            case GPU_DMA_CpuToGp0:  dma_request = dma_recv_ready;  break;
            case GPU_DMA_VRamToCpu: dma_request = vram_send_ready; break;
            default:                dma_request = false;           break;
        }
        if (dma_request) r |= (1u << 25);
    }

    // DMA direction (bits 30:29)
    r |= ((uint32_t)gpu->dma_setting) << 29;
    // Bit 31: odd/even line — from CRTC scanline counter
    r |= ((uint32_t)gpu->crtc.active_line_lsb) << 31;
    return r;
}

// ---------------------------------------------------------------------------
// GPUREAD data port
// ---------------------------------------------------------------------------
uint32_t gpu_read_data(Gpu* gpu) {
    // IMAGE_STORE: pack two 16-bit VRAM pixels per 32-bit word
    if (gpu->gp0_mode == GP0_MODE_IMAGE_STORE) {
        if (gpu->gp0_words_remaining == 0) {
            LOG_GPU_WARN("[GPU] GPUREAD: No words remaining in Image Store transfer");
            return 0xFFFFFFFF;
        }
        uint32_t idx = gpu->vram_load_count;
        uint16_t pixel1 = 0, pixel2 = 0;
        uint32_t word;

        uint16_t x1 = (gpu->vram_load_x + (uint16_t)(idx % gpu->vram_load_w)) & 0x3FF;
        uint16_t y1 = (gpu->vram_load_y + (uint16_t)(idx / gpu->vram_load_w)) & 0x1FF;
        pixel1 = vram_load16(&gpu->vram,
            (uint32_t)y1 * VRAM_WIDTH * VRAM_BPP + (uint32_t)x1 * VRAM_BPP);
        gpu->vram_load_count++;

        uint32_t total = (uint32_t)gpu->vram_load_w * gpu->vram_load_h;
        if (gpu->vram_load_count < total) {
            idx = gpu->vram_load_count;
            uint16_t x2 = (gpu->vram_load_x + (uint16_t)(idx % gpu->vram_load_w)) & 0x3FF;
            uint16_t y2 = (gpu->vram_load_y + (uint16_t)(idx / gpu->vram_load_w)) & 0x1FF;
            pixel2 = vram_load16(&gpu->vram,
                (uint32_t)y2 * VRAM_WIDTH * VRAM_BPP + (uint32_t)x2 * VRAM_BPP);
            gpu->vram_load_count++;
        }

        gpu->gp0_words_remaining--;
        if (gpu->gp0_words_remaining == 0) {
            gpu->gp0_mode = GP0_MODE_COMMAND;
            LOG_GPU_DEBUG("[GPU] GP0(0xC0): VRAM→CPU transfer COMPLETE");
        }
        word = (uint32_t)pixel1 | ((uint32_t)pixel2 << 16);
        gpu->gpu_info_latch = word;   /* GPUREAD is one register: it keeps the last word */
        return word;
    }

    /* "The selected data is latched in GP0, the same/latched value can be read
     * multiple times" (psx-spx gpu/display-control-commands-gp1.md:204-205).
     * It used to be cleared by the first read, and a read of an empty latch
     * returned an incrementing counter. */
    return gpu->gpu_info_latch;
}

// ---------------------------------------------------------------------------
// VBlank hook
// ---------------------------------------------------------------------------
void gpu_trigger_vblank_irq(Gpu* gpu) {
    (void)gpu;
    // VBlank IRQ0 is handled by the event scheduler — no direct action here.
    LOG_GPU_DEBUG("[GPU] VBlank event (IRQ0 via event scheduler)");
}

// ---------------------------------------------------------------------------
// Full initialization (power-on / hard reset — clears VRAM)
// ---------------------------------------------------------------------------
static void gpu_reset_state(Gpu* gpu) {
    gpu->interrupt = false;
    gpu->page_base_x = 0; gpu->page_base_y = 0;
    gpu->semi_transparency = 0;
    gpu->texture_depth = T4Bit;
    gpu->texture_window_x_mask = 0; gpu->texture_window_y_mask = 0;
    gpu->texture_window_x_offset = 0; gpu->texture_window_y_offset = 0;
    gpu->dithering = false; gpu->draw_to_display = false;
    gpu->texture_disable = false;
    gpu->rectangle_texture_x_flip = false;
    gpu->rectangle_texture_y_flip = false;
    // Initialize drawing area to full VRAM bounds (unrestricted)
    gpu->drawing_area_left = 0; gpu->drawing_area_top = 0;
    gpu->drawing_area_right = 1023; gpu->drawing_area_bottom = 511;
    gpu->drawing_x_offset = 0; gpu->drawing_y_offset = 0;
    gpu->force_set_mask_bit = false;
    gpu->preserve_masked_pixels = false;
    gpu->dma_setting = GPU_DMA_Off;
    gpu->display_disabled = true;
    gpu->display_vram_x_start = 0; gpu->display_vram_y_start = 0;
    gpu->hres_raw.hr1 = 0; gpu->hres_raw.hr2 = 0;
    gpu->vres = Y240Lines; gpu->vmode = Ntsc;
    /* Reset means GP1(08h)=0, so interlace is OFF — the documented post-reset
     * GPUSTAT is 14802000h (psx-spx-docs/docs/graphicsprocessingunitgpu.md:648)
     * and bit 22 is clear in it. Bit 13 *is* set there, which is gpu->field
     * (Top = 1), not this.
     *
     * Starting interlaced meant every reset — the cold one and the one the BIOS
     * issues when it hands over to the disc — put the machine in interlace with
     * vres=240 until the game wrote GP1(08h), which is exactly the combination
     * that used to double the display height. */
    gpu->interlaced = false; gpu->display_depth = D15Bits;
    gpu->display_horiz_start = 0x200; gpu->display_horiz_end = 0xC00;
    gpu->display_line_start = 0x10; gpu->display_line_end = 0x100;
    gpu->field = Top;
    gpu->display_width_hint = 320; gpu->display_height_hint = 240;
    gpu->vram_dirty = false;
    gpu->gpu_info_latch = 0;
    gpu->polyline_count = 0; gpu->polyline_shaded = false; gpu->polyline_semi_trans = false;
    gpu_clear_cmd_buf(gpu);
    gpu->gp0_words_remaining = 0;
    gpu->gp0_mode = GP0_MODE_COMMAND;
    gpu->gp0_command_method = NULL;
    gpu->gp0_current_opcode = 0xFF;
    gpu->gp0_fifo_head = 0; gpu->gp0_fifo_tail = 0; gpu->gp0_fifo_count = 0;
    gpu->vram_load_x = 0; gpu->vram_load_y = 0;
    gpu->vram_load_w = 0; gpu->vram_load_h = 0;
    gpu->vram_load_count = 0;
    // Reset CRTC state
    gpu->crtc.current_scanline  = 0;
    gpu->crtc.vertical_total    = 263; // NTSC default
    gpu->crtc.in_vblank         = false;
    gpu->crtc.active_line_lsb   = 0;
    gpu->crtc.interlaced_field  = 0;
    gpu->crtc.display_vram_x    = 0;
    gpu->crtc.display_vram_y    = 0;
    gpu->crtc.display_width     = 320;
    gpu->crtc.display_height    = 240;
    renderer_set_texture_window(&gpu->renderer, 0, 0, 0, 0);
    // Set initial drawing area to full VRAM bounds
    renderer_set_drawing_area(&gpu->renderer,
        gpu->drawing_area_left, gpu->drawing_area_top,
        gpu->drawing_area_right, gpu->drawing_area_bottom);
    // Set default display region (320x240 at VRAM origin) for correct blit before GP1(05-08)
    renderer_set_display_region(&gpu->renderer, 0, 0, 320, 240);
    renderer_set_display_blank(&gpu->renderer, gpu->display_disabled);
}

void gpu_init_full(Gpu* gpu, Interconnect* inter) {
    LOG_GPU_DEBUG("[GPU] GPU full initialization (with VRAM)");
    vram_init(&gpu->vram);
    gpu->inter = inter;
    gpu_reset_state(gpu);
    LOG_GPU_DEBUG("[GPU] GPU Initialized.");
}

/* Push everything the renderer keeps on our behalf back down to it.
 *
 * The ten renderer_set_* values are backend state, not Gpu state: a fresh
 * backend starts at its own defaults, and the guest has no reason to re-send
 * GP0(E2..E6) or GP1(05..08) just because the host swapped its graphics API.
 * Without this, the first frame after a hot switch draws with a zero draw
 * offset, a full-VRAM scissor and the mask flags cleared.
 *
 * Every value here is read from Gpu, which is the authority — the same
 * expressions the GP0/GP1 handlers use. Per-primitive state (dither,
 * semi-transparency, texture and raw-texture mode) is not here on purpose:
 * gpu_commands.c sets all four immediately before each push, so the next
 * primitive carries them anyway. */
void gpu_reapply_renderer_state(Gpu* gpu) {
    if (!gpu) return;

    renderer_set_screen_scale(&gpu->renderer, VRAM_WIDTH, VRAM_HEIGHT);
    renderer_set_draw_offset(&gpu->renderer, gpu->drawing_x_offset, gpu->drawing_y_offset);
    renderer_set_drawing_area(&gpu->renderer,
        gpu->drawing_area_left, gpu->drawing_area_top,
        gpu->drawing_area_right, gpu->drawing_area_bottom);
    renderer_set_texture_window(&gpu->renderer,
        gpu->texture_window_x_mask,   gpu->texture_window_y_mask,
        gpu->texture_window_x_offset, gpu->texture_window_y_offset);
    renderer_set_mask_mode(&gpu->renderer, gpu->force_set_mask_bit);
    renderer_set_mask_test(&gpu->renderer, gpu->preserve_masked_pixels);

    renderer_set_display_region(&gpu->renderer,
        gpu->crtc.display_vram_x, gpu->crtc.display_vram_y,
        gpu->crtc.display_width,  gpu->crtc.display_height);
    renderer_set_display_depth24(&gpu->renderer, gpu->display_depth == D24Bits);
    renderer_set_display_blank(&gpu->renderer, gpu->display_disabled);

    LOG_GPU_INFO("[GPU] Renderer state re-applied: offset(%d,%d) area[%u..%u,%u..%u] "
                 "texwin(%u,%u,%u,%u) mask(set=%d test=%d) display(%u,%u %ux%u) d24=%d blank=%d",
                 gpu->drawing_x_offset, gpu->drawing_y_offset,
                 gpu->drawing_area_left, gpu->drawing_area_right,
                 gpu->drawing_area_top,  gpu->drawing_area_bottom,
                 gpu->texture_window_x_mask, gpu->texture_window_y_mask,
                 gpu->texture_window_x_offset, gpu->texture_window_y_offset,
                 (int)gpu->force_set_mask_bit, (int)gpu->preserve_masked_pixels,
                 gpu->crtc.display_vram_x, gpu->crtc.display_vram_y,
                 gpu->crtc.display_width,  gpu->crtc.display_height,
                 (int)(gpu->display_depth == D24Bits), (int)gpu->display_disabled);
}

void gpu_soft_reset(Gpu* gpu) {
    LOG_GPU_DEBUG("[GPU] GPU soft reset (VRAM preserved)");
    gpu_reset_state(gpu);
    LOG_GPU_DEBUG("[GPU] GPU Soft Reset complete.");
}

// gpu_init (legacy alias used by some callers)
void gpu_init(Gpu* gpu, Interconnect* inter) {
    gpu_init_full(gpu, inter);
}
