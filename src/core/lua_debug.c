/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 ZioZoni95
 *
 * Part of ZoniStation One, a PlayStation 1 emulator.
 * See LICENSE for the full licence text and THIRD-PARTY.md for the
 * components of this project that have other authors.
 */
/*
 * lua_debug.c — see lua_debug.h.
 *
 * Owns a single lua_State, a console text ring-buffer, and two registry-ref
 * callback slots (on_break, on_event) — one each, not per-address: this is
 * sized for one interactive debugging session at a time, not a general
 * hook-table framework.
 */
#include "lua_debug.h"
#include "interconnect.h"
#include "spu.h"
#include "cdrom.h"
#include "cpu.h"
#include "debugger.h"
#include "ram.h"
#include "bios.h"
#include "gte.h"
#include "savestate.h"
#include "cdrom_audio.h"
#include "log.h"
#include <SDL3/SDL.h>

#include <string.h>
#include <stdio.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#define LUA_DEBUG_CONSOLE_CAP (64 * 1024)

static lua_State*    g_L = NULL;
static Interconnect* g_inter = NULL;
static Cpu*          g_cpu = NULL;
static bool          g_active = false;

static int g_break_cb_ref = LUA_NOREF;
static int g_event_cb_ref = LUA_NOREF;

static char   g_console[LUA_DEBUG_CONSOLE_CAP];
static size_t g_console_len = 0;
static FILE*  g_log_file = NULL;

/* ---- console buffer ------------------------------------------------------
 * Also mirrors everything to logs/Lua.log, flushed on every line — unlike
 * the ImGui per-category log windows (debug_ui.cpp), which batch-flush every
 * 64 writes. A debug script's whole point is "run it, then go read what
 * happened" within the same short session, so immediate durability matters
 * more here than the small extra I/O cost. */

static void console_append(const char* s) {
    size_t n = strlen(s);
    if (n >= LUA_DEBUG_CONSOLE_CAP) {
        /* Pathological single write larger than the whole buffer: keep tail. */
        s += (n - (LUA_DEBUG_CONSOLE_CAP - 1));
        n = LUA_DEBUG_CONSOLE_CAP - 1;
    }
    if (g_console_len + n >= LUA_DEBUG_CONSOLE_CAP) {
        /* Drop the oldest half to make room, keep it simple/cheap. */
        size_t keep = g_console_len / 2;
        memmove(g_console, g_console + (g_console_len - keep), keep);
        g_console_len = keep;
    }
    memcpy(g_console + g_console_len, s, n);
    g_console_len += n;
    g_console[g_console_len] = '\0';

    if (g_log_file) {
        fputs(s, g_log_file);
        fflush(g_log_file);
    }
}

const char* lua_debug_console_text(void) {
    return g_console;
}

void lua_debug_console_clear(void) {
    g_console_len = 0;
    g_console[0] = '\0';
}

/* ---- direct memory peek (RAM/BIOS/scratchpad only — see header) -------- */

static bool peek_bytes(uint32_t vaddr, uint32_t size, uint32_t* out) {
    uint32_t phys = vaddr & 0x1FFFFFFF;
    const uint8_t* p = NULL;

    if (phys < RAM_SIZE) {
        p = &g_inter->ram->data[phys];
        if (phys + size > RAM_SIZE) return false;
    } else if (phys >= 0x1FC00000 && phys < 0x1FC80000) {
        uint32_t off = phys - 0x1FC00000;
        if (off + size > BIOS_SIZE) return false;
        p = &g_inter->bios->data[off];
    } else if (phys >= 0x1F800000 && phys < 0x1F800400) {
        uint32_t off = phys - 0x1F800000;
        if (off + size > SCRATCHPAD_SIZE) return false;
        p = &g_inter->scratchpad[off];
    } else {
        return false;
    }

    uint32_t v = 0;
    for (uint32_t i = 0; i < size; i++) v |= ((uint32_t)p[i]) << (8 * i);
    *out = v;
    return true;
}

/* ---- emu.* bindings ------------------------------------------------------ */

static int l_emu_log(lua_State* L) {
    const char* msg = luaL_checkstring(L, 1);
    console_append(msg);
    console_append("\n");
    LOG_DEBUGGER_INFO("[LUA] %s", msg);
    return 0;
}

/* print() override: same sink as emu.log, so plain `print(...)` in scripts
 * shows up in the console panel without scripts needing to know emu.log. */
static int l_print(lua_State* L) {
    int n = lua_gettop(L);
    for (int i = 1; i <= n; i++) {
        if (i > 1) console_append("\t");
        const char* s = luaL_tolstring(L, i, NULL);
        console_append(s);
        lua_pop(L, 1);
    }
    console_append("\n");
    return 0;
}

static int l_emu_cycles(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)g_inter->cpu_cycle_counter);
    return 1;
}

static int l_emu_gte_data(lua_State* L) {
    lua_Integer i = luaL_checkinteger(L, 1);
    luaL_argcheck(L, i >= 0 && i < 32, 1, "GTE data register index 0-31");
    lua_pushinteger(L, (lua_Integer)gte_read_data_register(&g_cpu->gte, (uint32_t)i));
    return 1;
}

/* emu.disasm(addr) -> "mnemonic operands" string, via the real disassembler
 * (src/cpu/cpu_disasm.c) — use this instead of hand-decoding hex in scripts,
 * it's the same disassembler the Live Disasm/Exec Trace panels use. */
static int l_emu_disasm(lua_State* L) {
    uint32_t addr = (uint32_t)luaL_checkinteger(L, 1);
    uint32_t v = 0;
    if (!peek_bytes(addr, 4, &v)) return luaL_error(L, "disasm: address 0x%08x out of range", addr);
    const char* s = disassemble_mips(v, addr);
    lua_pushstring(L, s ? s : "?");
    return 1;
}

static int l_emu_pc(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)(uint32_t)g_cpu->current_pc);
    return 1;
}

static int l_emu_reg(lua_State* L) {
    lua_Integer i = luaL_checkinteger(L, 1);
    luaL_argcheck(L, i >= 0 && i < 32, 1, "register index 0-31");
    lua_pushinteger(L, (lua_Integer)(uint32_t)g_cpu->regs[i]);
    return 1;
}

static int l_emu_read_u8(lua_State* L) {
    uint32_t addr = (uint32_t)luaL_checkinteger(L, 1);
    uint32_t v = 0;
    if (!peek_bytes(addr, 1, &v)) return luaL_error(L, "read_u8: address 0x%08x out of range", addr);
    lua_pushinteger(L, (lua_Integer)v);
    return 1;
}
static int l_emu_read_u16(lua_State* L) {
    uint32_t addr = (uint32_t)luaL_checkinteger(L, 1);
    uint32_t v = 0;
    if (!peek_bytes(addr, 2, &v)) return luaL_error(L, "read_u16: address 0x%08x out of range", addr);
    lua_pushinteger(L, (lua_Integer)v);
    return 1;
}
static int l_emu_read_u32(lua_State* L) {
    uint32_t addr = (uint32_t)luaL_checkinteger(L, 1);
    uint32_t v = 0;
    if (!peek_bytes(addr, 4, &v)) return luaL_error(L, "read_u32: address 0x%08x out of range", addr);
    lua_pushinteger(L, (lua_Integer)v);
    return 1;
}

static int l_emu_add_breakpoint(lua_State* L) {
    uint32_t addr = (uint32_t)luaL_checkinteger(L, 1);
    lua_pushboolean(L, debugger_add_breakpoint(&g_inter->debugger, addr));
    return 1;
}
static int l_emu_remove_breakpoint(lua_State* L) {
    uint32_t addr = (uint32_t)luaL_checkinteger(L, 1);
    lua_pushboolean(L, debugger_remove_breakpoint(&g_inter->debugger, addr));
    return 1;
}
static int l_emu_add_write_watch(lua_State* L) {
    uint32_t addr = (uint32_t)luaL_checkinteger(L, 1);
    lua_pushboolean(L, debugger_add_write_watchpoint(&g_inter->debugger, addr));
    return 1;
}
static int l_emu_remove_write_watch(lua_State* L) {
    uint32_t addr = (uint32_t)luaL_checkinteger(L, 1);
    lua_pushboolean(L, debugger_remove_write_watchpoint(&g_inter->debugger, addr));
    return 1;
}
static int l_emu_add_read_watch(lua_State* L) {
    uint32_t addr = (uint32_t)luaL_checkinteger(L, 1);
    lua_pushboolean(L, debugger_add_read_watchpoint(&g_inter->debugger, addr));
    return 1;
}
static int l_emu_remove_read_watch(lua_State* L) {
    uint32_t addr = (uint32_t)luaL_checkinteger(L, 1);
    lua_pushboolean(L, debugger_remove_read_watchpoint(&g_inter->debugger, addr));
    return 1;
}

static int l_emu_pause(lua_State* L) {
    (void)L;
    g_inter->debugger.paused = true;
    return 0;
}
static int l_emu_resume(lua_State* L) {
    (void)L;
    g_inter->debugger.paused = false;
    return 0;
}
static int l_emu_is_paused(lua_State* L) {
    lua_pushboolean(L, g_inter->debugger.paused);
    return 1;
}

static int l_emu_on_break(lua_State* L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    if (g_break_cb_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, g_break_cb_ref);
    lua_pushvalue(L, 1);
    g_break_cb_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    return 0;
}
static int l_emu_on_event(lua_State* L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    if (g_event_cb_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, g_event_cb_ref);
    lua_pushvalue(L, 1);
    g_event_cb_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    return 0;
}

static int l_emu_gp0_opcode(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)g_inter->gpu.gp0_current_opcode);
    return 1;
}

static int l_emu_gp0_word_count(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)g_inter->gpu.gp0_command_buffer.count);
    return 1;
}

static int l_emu_gp0_word(lua_State* L) {
    lua_Integer i = luaL_checkinteger(L, 1);
    luaL_argcheck(L, i >= 0 && i < MAX_GPU_COMMAND_WORDS, 1, "gp0 word index out of range");
    lua_pushinteger(L, (lua_Integer)g_inter->gpu.gp0_command_buffer.buffer[i]);
    return 1;
}

/* One pixel of the macroblock MDEC has just finished decoding: block_rgb is the
 * 16x16 RGB result, before the output FIFO / DMA / VRAM ever see it, so this
 * isolates "is the decoder producing a real image?" from every downstream
 * placement question. Index 0..255, row-major within the 16x16 block. */
static int l_emu_mdec_block(lua_State* L) {
    lua_Integer i = luaL_checkinteger(L, 1);
    luaL_argcheck(L, i >= 0 && i < 256, 1, "mdec block index out of range");
    lua_pushinteger(L, (lua_Integer)g_inter->mdec.block_rgb[i]);
    return 1;
}

/* Live timer counter + mode/source, without a bus read (bypasses side effects).
 * counter is whatever timers_step last left — lets a script see how stale it is
 * between chunks. */
static int l_emu_timer(lua_State* L) {
    lua_Integer i = luaL_checkinteger(L, 1);
    luaL_argcheck(L, i >= 0 && i < 3, 1, "timer index out of range");
    const Timer* t = &g_inter->timers_state.timers[i];
    lua_pushinteger(L, (lua_Integer)t->counter);
    lua_pushinteger(L, (lua_Integer)t->clock_source);
    lua_pushboolean(L, t->counting_enabled);
    return 3;
}

/* Read one VRAM halfword directly from the CPU-side buffer at (x,y), same
 * store the viewer's readout uses — for checking whether A0 uploads actually
 * landed. */
static int l_emu_vram16(lua_State* L) {
    lua_Integer x = luaL_checkinteger(L, 1);
    lua_Integer y = luaL_checkinteger(L, 2);
    if (x < 0 || x >= 1024 || y < 0 || y >= 512) { lua_pushnil(L); return 1; }
    const uint16_t* v = (const uint16_t*)g_inter->gpu.vram.data;
    lua_pushinteger(L, (lua_Integer)v[(size_t)y * 1024 + (size_t)x]);
    return 1;
}

/* Peek the MDEC input FIFO without consuming it: index 0 is the next halfword
 * the RLE decoder will read. Lets a script see the raw compressed stream as
 * MDEC sees it, to tell a bad bitstream apart from a bad decoder. */
static int l_emu_mdec_in_peek(lua_State* L) {
    lua_Integer i = luaL_checkinteger(L, 1);
    const Mdec* m = &g_inter->mdec;
    if (i < 0 || (uint32_t)i >= m->in_count) { lua_pushnil(L); return 1; }
    lua_pushinteger(L, (lua_Integer)m->in_buf[(m->in_head + (uint32_t)i) % MDEC_IN_FIFO_HW]);
    return 1;
}

static int l_emu_mdec_in_count(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)g_inter->mdec.in_count);
    return 1;
}

/* Live MDEC DMA cursors: where ch0 is reading the compressed stream from and
 * where ch1 is writing decoded output to, plus words left on each. Lets a
 * script read the same RAM the DMA is feeding MDEC and compare. */
static int l_emu_mdec_dma(lua_State* L) {
    const Dma* d = &g_inter->dma;
    lua_pushinteger(L, (lua_Integer)d->mdec_in_addr);
    lua_pushinteger(L, (lua_Integer)d->mdec_in_remaining);
    lua_pushinteger(L, (lua_Integer)d->mdec_out_addr);
    lua_pushinteger(L, (lua_Integer)d->mdec_out_remaining);
    return 4;
}

/* MDEC decode context: output depth, halfwords left in the command, and the
 * quant/scale tables the IDCT is running with. */
static int l_emu_mdec_info(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)g_inter->mdec.output_depth);
    lua_pushinteger(L, (lua_Integer)g_inter->mdec.remaining_halfwords);
    lua_pushinteger(L, (lua_Integer)g_inter->mdec.current_q_scale);
    return 3;
}

static int l_emu_mdec_scale(lua_State* L) {
    lua_Integer i = luaL_checkinteger(L, 1);
    luaL_argcheck(L, i >= 0 && i < 64, 1, "scale table index out of range");
    lua_pushinteger(L, (lua_Integer)g_inter->mdec.scale_table[i]);
    return 1;
}

static int l_emu_mdec_qtable(lua_State* L) {
    lua_Integer i = luaL_checkinteger(L, 1);
    int chroma = lua_toboolean(L, 2);
    luaL_argcheck(L, i >= 0 && i < 64, 1, "quant table index out of range");
    lua_pushinteger(L, (lua_Integer)(chroma ? g_inter->mdec.iq_uv[i]
                                            : g_inter->mdec.iq_y[i]));
    return 1;
}

/* Renderer staging-pool telemetry: used, peak, queued updates, dropped rects. */
static int l_emu_gpu_pool(lua_State* L) {
    uint32_t used = 0, peak = 0, updates = 0, skips = 0;
    renderer_get_pool_stats(&g_inter->gpu.renderer, &used, &peak, &updates, &skips);
    lua_pushinteger(L, (lua_Integer)used);
    lua_pushinteger(L, (lua_Integer)peak);
    lua_pushinteger(L, (lua_Integer)updates);
    lua_pushinteger(L, (lua_Integer)skips);
    return 4;
}

/* Geometry of the most recent GP0(0xA0) CPU/DMA→VRAM upload, in VRAM
 * halfword units — pairs with the "gp0_vram_upload" event. */
static int l_emu_vram_upload_rect(lua_State* L) {
    Gpu* g = &g_inter->gpu;
    lua_pushinteger(L, (lua_Integer)g->vram_load_x);
    lua_pushinteger(L, (lua_Integer)g->vram_load_y);
    lua_pushinteger(L, (lua_Integer)g->vram_load_w);
    lua_pushinteger(L, (lua_Integer)g->vram_load_h);
    return 4;
}

/* GPUSTAT as the CPU would read it — display depth (bit 21), video mode,
 * resolution, display-disable etc. without having to breakpoint the port read. */
static int l_emu_gpustat(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)gpu_read_status(&g_inter->gpu));
    return 1;
}

/* Display area origin/size as programmed by GP1(05)/(06)/(07). */
static int l_emu_display_area(lua_State* L) {
    Gpu* g = &g_inter->gpu;
    lua_pushinteger(L, (lua_Integer)g->display_vram_x_start);
    lua_pushinteger(L, (lua_Integer)g->display_vram_y_start);
    lua_pushinteger(L, (lua_Integer)g->display_horiz_start);
    lua_pushinteger(L, (lua_Integer)g->display_horiz_end);
    lua_pushinteger(L, (lua_Integer)g->display_line_start);
    lua_pushinteger(L, (lua_Integer)g->display_line_end);
    return 6;
}

/* Drawing area (GP0 E3/E4) and drawing offset (GP0 E5). The GL path scissors
 * every batch to this area, so it decides which primitives can reach the
 * unified VRAM texture — a fill written unclipped into the CPU-side VRAM can
 * still be clipped away on its way to the texture. */
static int l_emu_draw_area(lua_State* L) {
    Gpu* g = &g_inter->gpu;
    lua_pushinteger(L, (lua_Integer)g->drawing_area_left);
    lua_pushinteger(L, (lua_Integer)g->drawing_area_top);
    lua_pushinteger(L, (lua_Integer)g->drawing_area_right);
    lua_pushinteger(L, (lua_Integer)g->drawing_area_bottom);
    lua_pushinteger(L, (lua_Integer)g->drawing_x_offset);
    lua_pushinteger(L, (lua_Integer)g->drawing_y_offset);
    return 6;
}

/* SPU output-path health: samples produced from the emulated clock, samples
 * dropped because the output ring was full, and how full that ring is right
 * now. Sample count against emulated time is the direct check that generation
 * is paced by the guest and not by the host. */
static int l_emu_spu_stats(lua_State* L) {
    Spu* spu = &g_inter->spu;
    int head = spu->sample_buf_head, tail = spu->sample_buf_tail;
    int used = (tail - head + SPU_SAMPLE_BUFFER_SIZE) % SPU_SAMPLE_BUFFER_SIZE;
    lua_pushinteger(L, (lua_Integer)spu->total_samples_generated);
    lua_pushinteger(L, (lua_Integer)spu->dropped_samples);
    lua_pushinteger(L, (lua_Integer)used);
    lua_pushinteger(L, (lua_Integer)SPU_SAMPLE_BUFFER_SIZE);
    lua_pushinteger(L, (lua_Integer)spu->total_key_on_events);
    return 5;
}

/* Reverb internal state — what a register poll cannot see. Lets a Lua trace tell
 * "the game switched reverb off" (control/vol/EON) from "the reverb network's own
 * tail is decaying wrong" (out_l/out_r while still enabled). Returns:
 *   control, reverb_enable(bool), vol_l, vol_r, eon_mask, base, cur_addr,
 *   in_l, in_r, out_l, out_r */
static int l_emu_reverb(lua_State* L) {
    Spu* spu = &g_inter->spu;
    lua_pushinteger(L, (lua_Integer)spu->control);
    lua_pushboolean(L, (spu->control & (1u << 7)) != 0);      /* SPUCNT reverb master enable */
    lua_pushinteger(L, (lua_Integer)spu->reverb_vol_left);
    lua_pushinteger(L, (lua_Integer)spu->reverb_vol_right);
    lua_pushinteger(L, (lua_Integer)spu->reverb_on);          /* per-voice EON mask */
    lua_pushinteger(L, (lua_Integer)spu->reverb_base);
    lua_pushinteger(L, (lua_Integer)spu->reverb_current_addr);
    lua_pushinteger(L, (lua_Integer)spu->reverb_in_l);
    lua_pushinteger(L, (lua_Integer)spu->reverb_in_r);
    lua_pushinteger(L, (lua_Integer)spu->reverb_out_l);
    lua_pushinteger(L, (lua_Integer)spu->reverb_out_r);
    return 11;
}

/* CD audio path health: FIFO depth, samples the drive has fed in, samples the
 * SPU has taken out, and samples lost to overflow. During FMV playback the XA
 * stream is the audio source, so a starving or overflowing FIFO here is heard
 * directly. Also reports SPUCNT, whose reverb/CD-audio enables decide what the
 * mixer is even supposed to be doing. */
/* emu.vram_compare() — the CPU-side VRAM model against what the GPU actually holds.
 *
 * The renderer rasterises into a GL texture; gpu.vram.data only ever receives
 * what the CPU or DMA wrote there. Anything a game draws with GP0 primitives and
 * then samples back as a texture therefore exists on one side and not the other,
 * and that gap is invisible from either side alone.
 *
 * Asynchronous by necessity: the GL context belongs to the GPU thread, so the
 * first call asks for a readback and returns nil, and a later call returns the
 * result once it has landed. Returns:
 *   differing, colour_diff, mask_diff, gpu_only, first_x, first_y, seq
 * where gpu_only counts pixels the GPU has and the CPU model reads as zero —
 * that is the count that matters for "did we sample something that was never
 * uploaded". */
static int l_emu_vram_compare(lua_State* L) {
    static uint32_t s_last_seq = 0;
    static bool     s_awaiting = false;

    Renderer* r = &g_inter->gpu.renderer;
    uint32_t seq = 0;
    const uint16_t* gpu_vram = renderer_get_vram_readback(&seq);

    if (s_awaiting && gpu_vram && seq != s_last_seq) {
        s_last_seq = seq;
        s_awaiting = false;

        const uint16_t* cpu_vram = (const uint16_t*)g_inter->gpu.vram.data;
        uint32_t total = 0, colour = 0, mask = 0, gpu_only = 0;
        int fx = -1, fy = -1;
        for (uint32_t i = 0; i < 1024u * 512u; i++) {
            uint16_t a = cpu_vram[i], b = gpu_vram[i];
            if (a == b) continue;
            total++;
            if ((a & 0x7FFF) != (b & 0x7FFF)) colour++; else mask++;
            if (a == 0 && b != 0) gpu_only++;
            if (fx < 0) { fx = (int)(i % 1024u); fy = (int)(i / 1024u); }
        }
        lua_pushinteger(L, (lua_Integer)total);
        lua_pushinteger(L, (lua_Integer)colour);
        lua_pushinteger(L, (lua_Integer)mask);
        lua_pushinteger(L, (lua_Integer)gpu_only);
        lua_pushinteger(L, (lua_Integer)fx);
        lua_pushinteger(L, (lua_Integer)fy);
        lua_pushinteger(L, (lua_Integer)seq);
        return 7;
    }

    if (!s_awaiting) {
        s_awaiting = true;
        renderer_request_vram_readback(r);
    }
    return 0;   /* nothing yet — call again on a later frame */
}

/* emu.vram_map(tile_w, tile_h) -> map, cols, rows, source
 *
 * Classifies the whole 1024x512 VRAM as a grid of tiles and returns the grid as
 * one string, row-major, no separators. One character per tile:
 *
 *   '.'  every halfword is 0000h — nothing has ever been written here
 *   '-'  uniform non-zero: a fill, a cleared framebuffer, a flat background
 *   ':'  mostly one value with a little variation
 *   '#'  varied: a real picture, a texture page, a decoded frame
 *
 * A per-tile summary rather than pixels because the point is *where* content
 * lives, not what it looks like: 1024x512 is 524288 halfwords, and pulling that
 * through one Lua call per pixel cannot run at field rate.
 *
 * Source: the GPU readback when a fresh one has arrived, so rasterised
 * primitives are included — the CPU-side mirror only ever receives uploads,
 * fills and DMA, and a scene drawn from polygons is invisible in it. The
 * readback is asynchronous, so this requests one and answers from the CPU
 * mirror until it lands; the returned source says which was used, and a caller
 * comparing the two must check it. */
static int l_emu_vram_map(lua_State* L) {
    lua_Integer tw = luaL_optinteger(L, 1, 16);
    lua_Integer th = luaL_optinteger(L, 2, 16);
    if (tw < 1 || tw > 1024 || th < 1 || th > 512) {
        return luaL_error(L, "tile size out of range");
    }
    const uint32_t cols = (uint32_t)((1024 + tw - 1) / tw);
    const uint32_t rows = (uint32_t)((512  + th - 1) / th);
    if (cols * rows > 8192u) return luaL_error(L, "grid too fine (max 8192 tiles)");

    static uint32_t s_last_seq = 0;
    uint32_t seq = 0;
    const uint16_t* gpu_vram = renderer_get_vram_readback(&seq);
    const uint16_t* src;
    const char* src_name;
    if (gpu_vram && seq != s_last_seq) {
        s_last_seq = seq;
        src = gpu_vram; src_name = "gpu";
    } else {
        src = (const uint16_t*)g_inter->gpu.vram.data; src_name = "cpu";
    }
    renderer_request_vram_readback(&g_inter->gpu.renderer);

    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (uint32_t ty = 0; ty < rows; ty++) {
        for (uint32_t tx = 0; tx < cols; tx++) {
            const uint32_t x0 = tx * (uint32_t)tw, y0 = ty * (uint32_t)th;
            const uint32_t x1 = (x0 + tw > 1024u) ? 1024u : x0 + (uint32_t)tw;
            const uint32_t y1 = (y0 + th > 512u)  ? 512u  : y0 + (uint32_t)th;
            uint16_t first = src[(size_t)y0 * 1024u + x0];
            uint32_t n = 0, nonzero = 0, differing = 0;
            for (uint32_t y = y0; y < y1; y++) {
                const uint16_t* row = src + (size_t)y * 1024u;
                for (uint32_t x = x0; x < x1; x++) {
                    uint16_t v = row[x];
                    n++;
                    if (v) nonzero++;
                    if (v != first) differing++;
                }
            }
            char c;
            if (nonzero == 0)            c = '.';
            else if (differing == 0)     c = '-';
            else if (differing * 16 < n) c = ':';
            else                         c = '#';
            luaL_addchar(&b, c);
        }
    }
    luaL_pushresult(&b);
    lua_pushinteger(L, (lua_Integer)cols);
    lua_pushinteger(L, (lua_Integer)rows);
    lua_pushstring(L, src_name);
    return 4;
}

/* emu.vram_row_stats(y, x0, w) -> nonzero, distinct_ish, first
 *
 * One VRAM row, summarised: how many of the w halfwords starting at x0 are
 * non-zero, how many differ from the first, and what the first one is. Written
 * for the question "are the 8 lines above the picture black, or stale?", which
 * a tile map answers too coarsely — 8 lines inside a 16-line tile are averaged
 * away with the picture below them. */
static int l_emu_vram_row_stats(lua_State* L) {
    lua_Integer y  = luaL_checkinteger(L, 1);
    lua_Integer x0 = luaL_optinteger(L, 2, 0);
    lua_Integer w  = luaL_optinteger(L, 3, 1024);
    if (y < 0 || y >= 512 || x0 < 0 || x0 >= 1024) { lua_pushnil(L); return 1; }
    if (x0 + w > 1024) w = 1024 - x0;

    uint32_t seq = 0;
    const uint16_t* gpu_vram = renderer_get_vram_readback(&seq);
    const uint16_t* src = gpu_vram ? gpu_vram : (const uint16_t*)g_inter->gpu.vram.data;
    const uint16_t* row = src + (size_t)y * 1024u;

    uint16_t first = row[x0];
    uint32_t nonzero = 0, differing = 0;
    for (lua_Integer i = 0; i < w; i++) {
        uint16_t v = row[x0 + i];
        if (v) nonzero++;
        if (v != first) differing++;
    }
    lua_pushinteger(L, (lua_Integer)nonzero);
    lua_pushinteger(L, (lua_Integer)differing);
    lua_pushinteger(L, (lua_Integer)first);
    return 3;
}

static int l_emu_cd_audio(lua_State* L) {
    Cdrom* cd = &g_inter->cdrom;
    lua_pushinteger(L, (lua_Integer)cd->audio_fifo.count);
    lua_pushinteger(L, (lua_Integer)cd->audio_fifo.total_pushed);
    lua_pushinteger(L, (lua_Integer)cd->audio_fifo.total_popped);
    lua_pushinteger(L, (lua_Integer)cd->audio_fifo.total_dropped);
    lua_pushinteger(L, (lua_Integer)cd->audio_fifo.total_starved);
    lua_pushinteger(L, (lua_Integer)g_inter->spu.control);
    lua_pushinteger(L, (lua_Integer)cd->sectors_read_total);
    lua_pushinteger(L, (lua_Integer)cd->xa_sectors_total);
    return 8;
}

/* ---- read-only audio state: emu.spu_voice / emu.spu_irq / emu.cd_state ----
 * Straight reads of the structs, each returned as a table of named fields. No
 * spu_catch_up and no FIFO pop, so calling them changes nothing: a probe that
 * read through the register path would advance the SPU it is measuring. The
 * values are as of the last catch-up, at most one SPU event (64 samples) old. */

static void audio_field_int(lua_State* L, const char* key, lua_Integer v) {
    lua_pushinteger(L, v);
    lua_setfield(L, -2, key);
}

static void audio_field_bool(lua_State* L, const char* key, bool v) {
    lua_pushboolean(L, v);
    lua_setfield(L, -2, key);
}

/* emu.spu_voice(n), n = 0..23: one voice's registers and internal state.
 * Addresses are byte addresses in SPU RAM except the *_reg fields, which are
 * the registers as written (units of 8 bytes). */
static int l_emu_spu_voice(lua_State* L) {
    lua_Integer n = luaL_checkinteger(L, 1);
    luaL_argcheck(L, n >= 0 && n < NUM_VOICES, 1, "voice index out of range (0..23)");
    const Spu* spu = &g_inter->spu;
    const SpuVoice* v = &spu->voices[n];
    lua_createtable(L, 0, 24);
    audio_field_bool(L, "on",            v->on);
    audio_field_int(L,  "curr_addr",     (lua_Integer)v->curr_addr);
    audio_field_int(L,  "start_reg",     (lua_Integer)v->start_address);
    audio_field_int(L,  "repeat_reg",    (lua_Integer)v->repeat_address);
    audio_field_int(L,  "repeat_addr",   (lua_Integer)v->repeat_address * 8);
    audio_field_int(L,  "sbpos",         (lua_Integer)v->SBPos);
    audio_field_bool(L, "end_mute_owed", v->reach_end);
    audio_field_int(L,  "adsr_state",    (lua_Integer)v->adsr_state);   /* 0 A, 1 D, 2 S, 3 R, 4 off */
    audio_field_int(L,  "envelope",      (lua_Integer)v->EnvelopeVol);
    audio_field_int(L,  "envx",          (lua_Integer)(uint16_t)v->adsr_volume);
    audio_field_int(L,  "adsr1",         (lua_Integer)v->adsr_low);
    audio_field_int(L,  "adsr2",         (lua_Integer)v->adsr_high);
    audio_field_int(L,  "vol_left_reg",  (lua_Integer)v->volume_left);
    audio_field_int(L,  "vol_right_reg", (lua_Integer)v->volume_right);
    audio_field_int(L,  "vol_left",      (lua_Integer)v->vol_left);     /* current, signed */
    audio_field_int(L,  "vol_right",     (lua_Integer)v->vol_right);
    audio_field_int(L,  "pitch",         (lua_Integer)v->pitch);
    audio_field_int(L,  "out",           (lua_Integer)v->sval);         /* VxOUTX, after ADSR */
    audio_field_bool(L, "endx",          (spu->endx >> n) & 1u);
    audio_field_bool(L, "reverb",        (spu->reverb_on >> n) & 1u);
    audio_field_bool(L, "noise",         (spu->noise_mode >> n) & 1u);
    audio_field_bool(L, "pitch_mod",     (spu->pitch_mod >> n) & 1u);
    return 1;
}

/* emu.spu_irq(): IRQ9 state, the transfer pointer and the global volumes. */
static int l_emu_spu_irq(lua_State* L) {
    const Spu* spu = &g_inter->spu;
    lua_createtable(L, 0, 20);
    audio_field_int(L,  "irq_reg",          (lua_Integer)spu->irq_addr);
    audio_field_int(L,  "irq_addr",         (lua_Integer)spu->irq_addr * 8);
    audio_field_bool(L, "irq9_flag",        spu->irq9_flag);
    audio_field_bool(L, "irq9_enable",      (spu->control & SPU_CTRL_IRQ9_ENABLE) != 0);
    audio_field_int(L,  "control",          (lua_Integer)spu->control);
    audio_field_int(L,  "status",           (lua_Integer)spu->status);
    audio_field_int(L,  "transfer_reg",     (lua_Integer)spu->transfer_addr_reg);
    audio_field_int(L,  "transfer_addr",    (lua_Integer)spu->transfer_addr);
    audio_field_int(L,  "manual_fifo",      (lua_Integer)spu->manual_fifo_count);
    audio_field_int(L,  "main_vol_left_reg",  (lua_Integer)spu->main_vol_left);
    audio_field_int(L,  "main_vol_right_reg", (lua_Integer)spu->main_vol_right);
    audio_field_int(L,  "main_vol_left",    (lua_Integer)spu->main_vol_left_cur);
    audio_field_int(L,  "main_vol_right",   (lua_Integer)spu->main_vol_right_cur);
    audio_field_int(L,  "cd_vol_left",      (lua_Integer)spu->cd_vol_left);
    audio_field_int(L,  "cd_vol_right",     (lua_Integer)spu->cd_vol_right);
    audio_field_int(L,  "cd_in_left",       (lua_Integer)spu->cd_audio_left);
    audio_field_int(L,  "cd_in_right",      (lua_Integer)spu->cd_audio_right);
    audio_field_bool(L, "muted",            spu->muted);
    audio_field_int(L,  "endx",             (lua_Integer)spu->endx);
    audio_field_int(L,  "capture_pos",      (lua_Integer)spu->capture_pos);
    return 1;
}

/* emu.cd_state(): the drive's state, mode, XA filter, output stage and the
 * GetlocL latch. header is the 8 bytes GetlocL answers with: mm, ss, ff
 * (BCD), mode, file, channel, submode, coding info. */
static int l_emu_cd_state(lua_State* L) {
    const Cdrom* cd = &g_inter->cdrom;
    lua_createtable(L, 0, 28);
    audio_field_int(L,  "drive_state",      (lua_Integer)cd->drive_state); /* 0 idle 1 spinup 2 seek 3 read 4 play 5 pausing 6 stopping */
    audio_field_int(L,  "interrupt_flag",   (lua_Integer)cd->interrupt_flag);
    audio_field_int(L,  "interrupt_enable", (lua_Integer)cd->interrupt_enable);
    audio_field_int(L,  "mode",             (lua_Integer)cd->mode);
    audio_field_bool(L, "xa_adpcm",         cd->xa_adpcm_enable);
    audio_field_bool(L, "xa_filter",        cd->xa_filter_enable);
    audio_field_int(L,  "filter_file",      (lua_Integer)cd->xa_filter_file);
    audio_field_int(L,  "filter_channel",   (lua_Integer)cd->xa_filter_channel);
    audio_field_bool(L, "muted",            cd->muted);
    audio_field_bool(L, "adpmute",          cd->xa_mute);
    audio_field_bool(L, "adpbusy",          cdrom_xa_playing(cd));
    audio_field_int(L,  "atv_ll",           (lua_Integer)cd->vol_ll);
    audio_field_int(L,  "atv_lr",           (lua_Integer)cd->vol_lr);
    audio_field_int(L,  "atv_rl",           (lua_Integer)cd->vol_rl);
    audio_field_int(L,  "atv_rr",           (lua_Integer)cd->vol_rr);
    audio_field_int(L,  "current_lba",      (lua_Integer)cd->current_lba);
    audio_field_int(L,  "head_lba",         (lua_Integer)cd->head_lba);
    audio_field_bool(L, "seek_phase",       cd->seek_phase);
    audio_field_bool(L, "auto_pause",       cd->auto_pause);
    audio_field_int(L,  "subq_track",       (lua_Integer)cd->last_subq.track_bcd);
    audio_field_int(L,  "sectors",          (lua_Integer)cd->sectors_read_total);
    audio_field_int(L,  "xa_sectors",       (lua_Integer)cd->xa_sectors_total);
    audio_field_int(L,  "audio_fifo",       (lua_Integer)cd->audio_fifo.count);
    audio_field_bool(L, "header_valid",     cd->last_header_valid);
    lua_createtable(L, 8, 0);
    for (int i = 0; i < 8; i++) {
        lua_pushinteger(L, (lua_Integer)cd->last_header[i]);
        lua_rawseti(L, -2, i + 1);
    }
    lua_setfield(L, -2, "header");
    return 1;
}

/* Host wall-clock milliseconds. Emulated cycles against this is the emulator's
 * real-time speed, which is what decides whether the audio device can be fed at
 * the rate it drains. */
static int l_emu_host_ms(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)SDL_GetTicks());
    return 1;
}

/* Interrupt controller state: I_STAT (latched requests), I_MASK (enables), and
 * the per-source line levels. A source that latches in I_STAT but is masked, or
 * that stays latched forever, means the guest's handler is not running — which
 * looks from the guest's side like an event that never arrives. */
static int l_emu_irq(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)g_inter->irq_status);
    lua_pushinteger(L, (lua_Integer)g_inter->irq_mask);
    lua_pushinteger(L, (lua_Integer)(uint32_t)g_cpu->sr);
    lua_pushinteger(L, (lua_Integer)(uint32_t)g_cpu->cause);
    return 4;
}


/* emu.save_state(path) / emu.load_state(path) — reach a state once by hand,
 * then re-enter it from a script instead of replaying the boot every run. */
static char g_pending_state_save[512];
static bool g_have_pending_state_save;

static int l_emu_save_state(lua_State* L) {
    const char* path = luaL_optstring(L, 1, SAVESTATE_DEFAULT_PATH);
    if (!g_inter || !g_cpu) { lua_pushboolean(L, 0); return 1; }
    /* Deferred for the same reason as the load, and with a sharper consequence:
     * a script's callbacks run from inside the event dispatch, and the VBlank
     * handler re-arms itself *after* notifying scripts. Writing the machine out
     * from in there captures a state with no VBlank scheduled, which on reload
     * never produces another frame — black screen, silent SPU. */
    snprintf(g_pending_state_save, sizeof(g_pending_state_save), "%s", path);
    g_have_pending_state_save = true;
    lua_pushboolean(L, 1);
    return 1;
}

bool lua_debug_take_pending_state_save(char* out, size_t out_size) {
    if (!g_have_pending_state_save) return false;
    g_have_pending_state_save = false;
    if (out && out_size) snprintf(out, out_size, "%s", g_pending_state_save);
    return true;
}

/* Deferred on purpose. A script's callbacks run from inside the event dispatch,
 * which is itself inside cpu_run_next_instruction: restoring the PC, the
 * downcount and the whole event queue underneath that call returns into a
 * machine that no longer matches the frame the caller is still executing. The
 * request is parked and the host loop applies it between frames. */
static char g_pending_state_load[512];
static bool g_have_pending_state_load;

static int l_emu_load_state(lua_State* L) {
    const char* path = luaL_optstring(L, 1, SAVESTATE_DEFAULT_PATH);
    if (!g_inter || !g_cpu) { lua_pushboolean(L, 0); return 1; }
    snprintf(g_pending_state_load, sizeof(g_pending_state_load), "%s", path);
    g_have_pending_state_load = true;
    lua_pushboolean(L, 1);
    return 1;
}

bool lua_debug_take_pending_state_load(char* out, size_t out_size) {
    if (!g_have_pending_state_load) return false;
    g_have_pending_state_load = false;
    if (out && out_size) snprintf(out, out_size, "%s", g_pending_state_load);
    return true;
}

/* emu.audio_stats() — the two opposite delivery failures, side by side.
 * Returns: cd_pushed, cd_popped, cd_dropped, cd_queued,
 *          spu_generated, spu_ring_drops, spu_underrun_events,
 *          spu_underrun_samples, spu_ring_used */
static int l_emu_audio_stats(lua_State* L) {
    if (!g_inter) return 0;
    const AudioFifo* f = &g_inter->cdrom.audio_fifo;
    const Spu* spu = &g_inter->spu;
    lua_pushinteger(L, (lua_Integer)f->total_pushed);
    lua_pushinteger(L, (lua_Integer)f->total_popped);
    lua_pushinteger(L, (lua_Integer)f->total_dropped);
    lua_pushinteger(L, (lua_Integer)f->count);
    lua_pushinteger(L, (lua_Integer)spu->total_samples_generated);
    lua_pushinteger(L, (lua_Integer)spu->dropped_samples);
    lua_pushinteger(L, (lua_Integer)spu->underrun_events);
    lua_pushinteger(L, (lua_Integer)spu->underrun_samples);
    lua_pushinteger(L, (lua_Integer)spu_ring_used(spu));
    return 9;
}

/* emu.stretch() — what the consumer-side time-stretch is doing.
 * Returns: tempo, active(bool), periods, blocks_stretched, queued_frames.
 * The stretcher runs on the audio thread; these are read without a lock, which
 * is right for a probe — a torn read costs one wrong line, a lock would change
 * the timing being measured. */
static int l_emu_stretch(lua_State* L) {
    if (!g_inter) return 0;
    const Spu* spu = &g_inter->spu;
    lua_pushnumber(L, spu->stretch_tempo);
    lua_pushboolean(L, spu->stretch_active);
    lua_pushinteger(L, (lua_Integer)spu->stretch_periods);
    lua_pushinteger(L, (lua_Integer)spu->stretch.blocks_stretched);
    lua_pushinteger(L, (lua_Integer)spu_stretch_queued(&spu->stretch));
    return 5;
}

static const luaL_Reg s_emu_funcs[] = {
    {"stretch",           l_emu_stretch},
    {"log",               l_emu_log},
    {"save_state",        l_emu_save_state},
    {"load_state",        l_emu_load_state},
    {"audio_stats",       l_emu_audio_stats},
    {"pc",                l_emu_pc},
    {"cycles",            l_emu_cycles},
    {"gte_data",          l_emu_gte_data},
    {"disasm",            l_emu_disasm},
    {"reg",               l_emu_reg},
    {"read_u8",           l_emu_read_u8},
    {"read_u16",          l_emu_read_u16},
    {"read_u32",          l_emu_read_u32},
    {"add_breakpoint",    l_emu_add_breakpoint},
    {"remove_breakpoint", l_emu_remove_breakpoint},
    {"add_write_watch",   l_emu_add_write_watch},
    {"remove_write_watch",l_emu_remove_write_watch},
    {"add_read_watch",    l_emu_add_read_watch},
    {"remove_read_watch", l_emu_remove_read_watch},
    {"pause",             l_emu_pause},
    {"resume",            l_emu_resume},
    {"is_paused",         l_emu_is_paused},
    {"on_break",          l_emu_on_break},
    {"on_event",          l_emu_on_event},
    {"gp0_opcode",        l_emu_gp0_opcode},
    {"gp0_word_count",    l_emu_gp0_word_count},
    {"gp0_word",          l_emu_gp0_word},
    {"gpustat",           l_emu_gpustat},
    {"draw_area",         l_emu_draw_area},
    {"spu_stats",         l_emu_spu_stats},
    {"reverb",            l_emu_reverb},
    {"cd_audio",          l_emu_cd_audio},
    {"spu_voice",         l_emu_spu_voice},
    {"spu_irq",           l_emu_spu_irq},
    {"cd_state",          l_emu_cd_state},
    {"vram_compare",      l_emu_vram_compare},
    {"host_ms",           l_emu_host_ms},
    {"irq",               l_emu_irq},
    {"vram_upload_rect",  l_emu_vram_upload_rect},
    {"gpu_pool",          l_emu_gpu_pool},
    {"mdec_block",        l_emu_mdec_block},
    {"mdec_in_peek",      l_emu_mdec_in_peek},
    {"mdec_in_count",     l_emu_mdec_in_count},
    {"mdec_dma",          l_emu_mdec_dma},
    {"vram16",            l_emu_vram16},
    {"vram_map",          l_emu_vram_map},
    {"vram_row_stats",    l_emu_vram_row_stats},
    {"timer",             l_emu_timer},
    {"mdec_info",         l_emu_mdec_info},
    {"mdec_scale",        l_emu_mdec_scale},
    {"mdec_qtable",       l_emu_mdec_qtable},
    {"display_area",      l_emu_display_area},
    {NULL, NULL}
};

/* ---- lifecycle ----------------------------------------------------------- */

void lua_debug_init(struct Interconnect* inter, struct Cpu* cpu) {
    g_inter = (Interconnect*)inter;
    g_cpu   = (Cpu*)cpu;
    g_console_len = 0;
    g_console[0] = '\0';
    g_break_cb_ref = LUA_NOREF;
    g_event_cb_ref = LUA_NOREF;
    g_log_file = fopen("logs/Lua.log", "w");

    g_L = luaL_newstate();
    if (!g_L) {
        LOG_DEBUGGER_ERROR("[LUA] luaL_newstate failed");
        return;
    }
    /* Individually require the libs we want instead of luaL_openlibs(), since
     * loadlib.c (package/require/dlopen) is deliberately excluded from the
     * build — a debug script has no legitimate use for dynamic native-lib
     * loading, and skipping it drops the -ldl link requirement too. */
    static const luaL_Reg libs[] = {
        {"_G",     luaopen_base},
        {"table",  luaopen_table},
        {"string", luaopen_string},
        {"math",   luaopen_math},
        {"os",     luaopen_os},
        {NULL, NULL}
    };
    for (const luaL_Reg* l = libs; l->func; l++) {
        luaL_requiref(g_L, l->name, l->func, 1);
        lua_pop(g_L, 1);
    }

    /* Global print() -> console panel (in addition to emu.log). */
    lua_pushcfunction(g_L, l_print);
    lua_setglobal(g_L, "print");

    /* emu.* table */
    luaL_newlib(g_L, s_emu_funcs);
    lua_setglobal(g_L, "emu");

    g_active = true;
    console_append("Lua 5.4 debug console ready.\n");
}

void lua_debug_shutdown(void) {
    if (g_L) {
        lua_close(g_L);
        g_L = NULL;
    }
    if (g_log_file) {
        fclose(g_log_file);
        g_log_file = NULL;
    }
    g_active = false;
}

static bool report_lua_error(int status) {
    if (status != LUA_OK) {
        const char* msg = lua_tostring(g_L, -1);
        console_append("error: ");
        console_append(msg ? msg : "(unknown)");
        console_append("\n");
        lua_pop(g_L, 1);
        return false;
    }
    return true;
}

bool lua_debug_run_string(const char* code) {
    if (!g_active) return false;
    int status = luaL_loadstring(g_L, code);
    if (status == LUA_OK) status = lua_pcall(g_L, 0, 0, 0);
    return report_lua_error(status);
}

/* Evaluate one expression and format its result as a short string.
 *
 * This is what the pinned watch tiles are built on: a tile is an expression
 * plus its value, refreshed while the panel is visible. Errors are returned in
 * `out` instead of being logged — a watch with a typo in it would otherwise
 * write a line to the console on every refresh, which is exactly the kind of
 * instrumentation cost the panels are supposed to avoid.
 *
 * A table result is summarised as its numeric fields rather than as an address,
 * because most of the emu.* accessors (audio_stats, spu_stats, reverb) return
 * one. */
bool lua_debug_eval_expr(const char* expr, char* out, size_t out_size) {
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    if (!g_active) { snprintf(out, out_size, "lua not running"); return false; }
    if (!expr || !expr[0]) return false;

    char buf[512];
    snprintf(buf, sizeof(buf), "return (%s)", expr);

    int base = lua_gettop(g_L);
    int status = luaL_loadstring(g_L, buf);
    if (status != LUA_OK) {
        snprintf(out, out_size, "%s", lua_tostring(g_L, -1) ? lua_tostring(g_L, -1) : "syntax error");
        lua_settop(g_L, base);
        return false;
    }
    status = lua_pcall(g_L, 0, 1, 0);
    if (status != LUA_OK) {
        const char* msg = lua_tostring(g_L, -1);
        /* Strip the chunk-name prefix Lua puts on runtime errors; the tile has
         * no room for [string "return (...)"]:1: */
        const char* colon = msg ? strrchr(msg, ':') : NULL;
        snprintf(out, out_size, "%s", colon ? colon + 2 : (msg ? msg : "error"));
        lua_settop(g_L, base);
        return false;
    }

    int t = lua_type(g_L, -1);
    if (t == LUA_TNUMBER) {
        lua_Number n = lua_tonumber(g_L, -1);
        if (n == (lua_Number)(long long)n && n < 1e15 && n > -1e15) {
            long long i = (long long)n;
            if (i >= 0x1000 || i <= -0x1000) snprintf(out, out_size, "%lld  (0x%llX)", i, (unsigned long long)i);
            else snprintf(out, out_size, "%lld", i);
        } else {
            snprintf(out, out_size, "%.4f", (double)n);
        }
    } else if (t == LUA_TTABLE) {
        size_t used = 0;
        out[0] = '\0';
        lua_pushnil(g_L);
        while (lua_next(g_L, -2) != 0) {
            if (lua_type(g_L, -2) == LUA_TSTRING && lua_isnumber(g_L, -1)) {
                const char* k = lua_tostring(g_L, -2);
                double v = lua_tonumber(g_L, -1);
                int wrote = snprintf(out + used, out_size - used, "%s%s=%g",
                                     used ? " " : "", k, v);
                if (wrote > 0) used += (size_t)wrote;
                if (used >= out_size - 1) { lua_pop(g_L, 2); break; }
            }
            lua_pop(g_L, 1);
        }
        if (!out[0]) snprintf(out, out_size, "table");
    } else if (t == LUA_TNIL) {
        snprintf(out, out_size, "nil");
    } else if (t == LUA_TBOOLEAN) {
        snprintf(out, out_size, "%s", lua_toboolean(g_L, -1) ? "true" : "false");
    } else {
        const char* sv = lua_tostring(g_L, -1);
        snprintf(out, out_size, "%s", sv ? sv : lua_typename(g_L, t));
    }
    lua_settop(g_L, base);
    return true;
}

bool lua_debug_run_file(const char* path) {
    if (!g_active) return false;
    int status = luaL_loadfile(g_L, path);
    if (status == LUA_OK) status = lua_pcall(g_L, 0, 0, 0);
    return report_lua_error(status);
}

/* ---- native -> Lua dispatch ----------------------------------------------- */

void lua_debug_notify(const char* event_name) {
    if (!g_active || g_event_cb_ref == LUA_NOREF) return;
    lua_rawgeti(g_L, LUA_REGISTRYINDEX, g_event_cb_ref);
    lua_pushstring(g_L, event_name);
    int status = lua_pcall(g_L, 1, 0, 0);
    report_lua_error(status);
}

void lua_debug_dispatch_break(const char* reason) {
    if (!g_active || g_break_cb_ref == LUA_NOREF) return;
    lua_rawgeti(g_L, LUA_REGISTRYINDEX, g_break_cb_ref);
    lua_pushstring(g_L, reason);
    int status = lua_pcall(g_L, 1, 0, 0);
    report_lua_error(status);
}
