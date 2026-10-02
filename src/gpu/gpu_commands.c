/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 ZioZoni95
 *
 * Part of ZoniStation One, a PlayStation 1 emulator.
 * See LICENSE for the full licence text and THIRD-PARTY.md for the
 * components of this project that have other authors.
 */
/**
 * gpu_commands.c
 * All GP0 command handlers for the PlayStation GPU.
 * Includes command dispatch, primitive rendering (poly, rect, line),
 * VRAM transfer operations, and state-setting commands.
 *
 * Modular split from gpu.c: one file per command family.
 */

#include "gpu.h"
#include "gpu_helpers.h"
#include "renderer.h"
#include "vram.h"
#include "log.h"
#include "interconnect.h"
#include "lua_debug.h"
#include "frame_events.h"
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static void gp0_nop(Gpu* gpu);
static void gp0_clear_cache(Gpu* gpu);
static void gp0_fill_rectangle(Gpu* gpu);
static void gp0_draw_mode(Gpu* gpu);
static void gp0_texture_window(Gpu* gpu);
static void gp0_drawing_area_top_left(Gpu* gpu);
static void gp0_drawing_area_bottom_right(Gpu* gpu);
static void gp0_drawing_offset(Gpu* gpu);
static void gp0_mask_bit_setting(Gpu* gpu);

// Polygon handlers
static void gp0_tri_mono_opaque(Gpu* gpu);
static void gp0_tri_mono_semi(Gpu* gpu);
static void gp0_tri_tex_blend_opaque(Gpu* gpu);
static void gp0_tri_tex_blend_semi(Gpu* gpu);
static void gp0_tri_tex_raw_opaque(Gpu* gpu);
static void gp0_tri_tex_raw_semi(Gpu* gpu);
static void gp0_quad_mono_opaque(Gpu* gpu);
static void gp0_quad_mono_semi(Gpu* gpu);
static void gp0_quad_tex_blend_opaque(Gpu* gpu);
static void gp0_quad_tex_blend_semi(Gpu* gpu);
static void gp0_quad_tex_raw_opaque(Gpu* gpu);
static void gp0_quad_tex_raw_semi(Gpu* gpu);
static void gp0_tri_shaded_opaque(Gpu* gpu);
static void gp0_tri_shaded_semi(Gpu* gpu);
static void gp0_quad_shaded_opaque(Gpu* gpu);
static void gp0_quad_shaded_semi(Gpu* gpu);
static void gp0_tri_shaded_tex_blend_opaque(Gpu* gpu);
static void gp0_tri_shaded_tex_blend_semi(Gpu* gpu);
static void gp0_tri_shaded_tex_raw_opaque(Gpu* gpu);
static void gp0_tri_shaded_tex_raw_semi(Gpu* gpu);
static void gp0_quad_shaded_tex_blend_opaque(Gpu* gpu);
static void gp0_quad_shaded_tex_blend_semi(Gpu* gpu);
static void gp0_quad_shaded_tex_raw_opaque(Gpu* gpu);
static void gp0_quad_shaded_tex_raw_semi(Gpu* gpu);

// Rectangle handlers
static void gp0_rect_variable_opaque(Gpu* gpu);
static void gp0_rect_variable_semi(Gpu* gpu);
static void gp0_rect_tex_variable_opaque(Gpu* gpu);
static void gp0_rect_tex_variable_semi(Gpu* gpu);
static void gp0_rect_1x1_opaque(Gpu* gpu);
static void gp0_rect_1x1_semi(Gpu* gpu);
static void gp0_rect_tex_1x1_opaque(Gpu* gpu);
static void gp0_rect_tex_1x1_semi(Gpu* gpu);
static void gp0_rect_8x8_opaque(Gpu* gpu);
static void gp0_rect_8x8_semi(Gpu* gpu);
static void gp0_rect_tex_8x8_opaque(Gpu* gpu);
static void gp0_rect_tex_8x8_semi(Gpu* gpu);
static void gp0_rect_16x16_opaque(Gpu* gpu);
static void gp0_rect_16x16_semi(Gpu* gpu);
static void gp0_rect_tex_16x16_opaque(Gpu* gpu);
static void gp0_rect_tex_16x16_semi(Gpu* gpu);

// VRAM ops
static void gp0_copy_rectangle(Gpu* gpu);
static void gp0_image_load(Gpu* gpu);
static void gp0_image_store(Gpu* gpu);

// Line handlers (simple 2-vertex forms)
static void gp0_line_mono_opaque(Gpu* gpu);
static void gp0_line_mono_semi(Gpu* gpu);
static void gp0_line_shaded_opaque(Gpu* gpu);
static void gp0_line_shaded_semi(Gpu* gpu);

// Polyline (variable-length, starts state machine in gpu_gp0_handle_word)
static void gp0_polyline_mono_opaque(Gpu* gpu);
static void gp0_polyline_mono_semi(Gpu* gpu);
static void gp0_polyline_shaded_opaque(Gpu* gpu);
static void gp0_polyline_shaded_semi(Gpu* gpu);

// Internal helpers
static void draw_rectangle(Gpu* gpu, int16_t x, int16_t y, uint16_t w, uint16_t h,
                            RendererColor col, bool textured, bool raw_texture,
                            RendererTexCoord* tex, uint16_t clut, uint16_t tpage,
                            bool semi_trans);
static void vram_write_masked(Gpu* gpu, uint32_t offset, uint16_t pixel);
static void flush_polyline(Gpu* gpu);

// ---------------------------------------------------------------------------
// Shared helpers (also used by gpu.c via non-static linkage)
// ---------------------------------------------------------------------------

void gpu_clear_cmd_buf(Gpu* gpu) {
    gpu->gp0_command_buffer.count = 0;
}

void gpu_push_cmd_word(Gpu* gpu, uint32_t word) {
    if (gpu->gp0_command_buffer.count >= MAX_GPU_COMMAND_WORDS) {
        LOG_GPU_ERROR("[GPU] FATAL: GP0 Command Buffer Overflow! Opcode: 0x%02x @ 0x%08x",
                      gpu->gp0_current_opcode);
        exit(EXIT_FAILURE);
    }
    gpu->gp0_command_buffer.buffer[gpu->gp0_command_buffer.count++] = word;
}

// ---------------------------------------------------------------------------
// VRAM masked write (respects force_set_mask_bit / preserve_masked_pixels)
// ---------------------------------------------------------------------------
static void vram_write_masked(Gpu* gpu, uint32_t offset, uint16_t pixel) {
    if (gpu->force_set_mask_bit)
        pixel |= 0x8000;
    if (gpu->preserve_masked_pixels) {
        uint16_t dst = vram_load16(&gpu->vram, offset);
        if (dst & 0x8000)
            return;
    }
    vram_store16(&gpu->vram, offset, pixel);
}

/* The CPU copy of VRAM as halfwords. Every renderer call already hands it over
 * this way; the array is a byte array, so its offset in Gpu is checked here
 * rather than assumed. */
typedef char gpu_vram_is_halfword_aligned[(offsetof(Gpu, vram) % 2u == 0u) ? 1 : -1];
static inline uint16_t* vram16(Gpu* gpu) { return (uint16_t*)(void*)gpu->vram.data; }

/* ZS1_GPU_TRACE_BATCH, read once: it is tested on every completed GP0(A0h) and
 * every GP0(C0h), and getenv() walks the whole environment each time. */
static int trace_batch_enabled(void) {
    static int cached = -1;
    if (cached < 0) cached = getenv("ZS1_GPU_TRACE_BATCH") ? 1 : 0;
    return cached;
}

// ---------------------------------------------------------------------------
// Moving VRAM between the CPU copy and the renderer
//
// gpu.vram.data holds what the CPU, DMA and MDEC wrote; what the rasteriser
// drew exists only in the renderer. GP0(80h) and GP0(C0h) need the real pixels,
// and a readback is a synchronous round trip through the GPU thread (on GL it
// also waits for the previous field, swap included). vram.c keeps a 16x16-tile
// map of where the rasteriser may have drawn since the CPU copy was last made
// authoritative there; a rectangle with no such tile is already right in
// gpu.vram.data and is not read back.
//
// Marking is conservative: every rasterised primitive marks the whole drawing
// area it is clipped to (gpu_note_raster). Tiles are cleared only where the CPU
// copy has been pushed to the renderer or pulled back from it in full.
// ZS1_FORCE_READBACK=1 reads back every time, as before, for the A/B.
// ---------------------------------------------------------------------------
static bool force_readback(void) {
    static int cached = -1;
    if (cached < 0) {
        const char* v = getenv("ZS1_FORCE_READBACK");
        cached = (v && v[0] == '1') ? 1 : 0;
        if (cached) LOG_GPU_INFO("[GPU] ZS1_FORCE_READBACK=1: every GP0(80h)/(C0h) reads VRAM back");
    }
    return cached != 0;
}

/* A rasterised primitive is about to be submitted: it can only land inside the
 * drawing area: "The Render commands GP0(20h..7Fh) are automatically clipping
 * any pixels that are outside of this region" (psx-spx
 * gpu/rendering-attributes.md:139-140). */
static inline void gpu_note_raster(const Gpu* gpu) {
    vram_raster_mark_area(gpu->drawing_area_left, gpu->drawing_area_top,
                          gpu->drawing_area_right, gpu->drawing_area_bottom);
}

/* Make the CPU copy authoritative for a (possibly wrapped) rectangle. */
static void gpu_vram_pull(Gpu* gpu, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    VramRect p[4];
    const int n = vram_split_rect(x, y, w, h, p);
    const bool force = force_readback();
    for (int i = 0; i < n; i++) {
        if (!force && !vram_raster_any(p[i].x, p[i].y, p[i].w, p[i].h)) continue;
        if (renderer_read_vram_rect(&gpu->renderer, vram16(gpu), p[i].x, p[i].y, p[i].w, p[i].h))
            vram_raster_clear(p[i].x, p[i].y, p[i].w, p[i].h);
    }
}

/* Push a (possibly wrapped) rectangle of the CPU copy to the renderer. It goes
 * as in-bounds pieces: the renderers copy w*h halfwords row by row from
 * vram.data, so a rectangle hanging past line 511 used to read past the end of
 * the buffer, and one past column 1023 wrote the wrong pixels
 * (psx-spx gpu/memory-transfer-commands.md:95-98). renderer_upload_vram_rect()
 * flushes the primitives still pending first, so the write keeps its place in
 * the order. Afterwards the CPU copy is authoritative in the rectangle. */
static void gpu_vram_push(Gpu* gpu, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    VramRect p[4];
    const int n = vram_split_rect(x, y, w, h, p);
    for (int i = 0; i < n; i++) {
        renderer_upload_vram_rect(&gpu->renderer, vram16(gpu), p[i].x, p[i].y, p[i].w, p[i].h);
        vram_raster_clear(p[i].x, p[i].y, p[i].w, p[i].h);
    }
}

// ---------------------------------------------------------------------------
// Helper: Draw a rectangle as a quad
// ---------------------------------------------------------------------------
static void draw_rectangle(Gpu* gpu, int16_t x, int16_t y, uint16_t w, uint16_t h,
                            RendererColor col, bool textured, bool raw_texture,
                            RendererTexCoord* tex, uint16_t clut, uint16_t tpage,
                            bool semi_trans)
{
    RendererPosition p[4];
    RendererColor c[4] = {col, col, col, col};
    RendererTexCoord t[4];

    p[0].x = x;       p[0].y = y;
    p[1].x = x + w;   p[1].y = y;
    p[2].x = x;       p[2].y = y + h;
    p[3].x = x + w;   p[3].y = y + h;

    bool use_texture = textured && tex;

    // Rectangles NEVER dither (per PSX spec)
    renderer_set_dither_mode(&gpu->renderer, false);
    renderer_set_semi_trans_mode(&gpu->renderer, semi_trans, gpu->semi_transparency);

    if (use_texture) {
        // Apply texture flip if enabled (for rectangle primitives)
        // Force neutral gray color for raw texture mode (no vertex color modulation)
        if (raw_texture) {
            for (int i = 0; i < 4; i++) {
                c[i].r = 128;
                c[i].g = 128;
                c[i].b = 128;
            }
        }

        // Unpack tpage for diagnostic logging
        uint16_t dbg_page_x = (uint16_t)((tpage & 0xF) * 64);   // 16-bit: no overflow
        uint8_t  dbg_page_y = ((tpage >> 4) & 1);
        uint8_t  dbg_depth  = (tpage >> 7) & 3;
        uint16_t dbg_clut_x = (clut & 0x3F) * 16;
        uint16_t dbg_clut_y = (clut >> 6) & 0x1FF;
        LOG_GPU_DEBUG("[GPU] rect tex x=%d y=%d w=%u h=%u uv=(%d,%d) page_x=%u page_y=%u depth=%u clut=(%u,%u) semi=%d raw=%d flip=(%d,%d) tpage_raw=0x%04x",
                      x, y, w, h, tex->u, tex->v,
                      dbg_page_x, (uint16_t)(dbg_page_y * 256),
                      dbg_depth, dbg_clut_x, dbg_clut_y,
                      (int)semi_trans, (int)raw_texture,
                      (int)gpu->rectangle_texture_x_flip, (int)gpu->rectangle_texture_y_flip,
                      tpage);

        t[0].u = tex->u;       t[0].v = tex->v;
        t[1].u = tex->u + w;   t[1].v = tex->v;
        t[2].u = tex->u;       t[2].v = tex->v + h;
        t[3].u = tex->u + w;   t[3].v = tex->v + h;
        if (gpu->rectangle_texture_x_flip) {
            t[0].u = tex->u + w - 1; t[1].u = tex->u;
            t[2].u = tex->u + w - 1; t[3].u = tex->u;
        }
        if (gpu->rectangle_texture_y_flip) {
            t[0].v = tex->v + h - 1; t[1].v = tex->v + h - 1;
            t[2].v = tex->v;         t[3].v = tex->v;
        }
        renderer_set_raw_texture_mode(&gpu->renderer, raw_texture);
        renderer_set_texture_mode(&gpu->renderer, true);
        renderer_push_quad(&gpu->renderer, p, c, t, clut, tpage);
    } else {
        renderer_set_raw_texture_mode(&gpu->renderer, false);
        renderer_set_texture_mode(&gpu->renderer, false);
        renderer_push_quad(&gpu->renderer, p, c, NULL, 0, 0);
    }
}

// ---------------------------------------------------------------------------
// Helper: make tpage word from current GPU state
// ---------------------------------------------------------------------------
static inline uint16_t make_tpage(Gpu* gpu) {
    return (uint16_t)(gpu->page_base_x | (gpu->page_base_y << 4) |
                      ((uint16_t)gpu->texture_depth << 7));
}

/* ZS1_GPU_TRACE_BATCH=1: dump the whole command for the first few distinct
 * textured primitives. The batch trace shows the texpage we ended up with; this
 * shows the words it was taken from, which is the only way to tell "the guest
 * asked for page 11" from "we read the wrong word of the command". */
static void trace_textured_primitive(const Gpu* gpu, const char* what,
                                     uint16_t clut, uint16_t texpage) {
    static int s_trace = -1;
    if (s_trace < 0) s_trace = getenv("ZS1_GPU_TRACE_BATCH") ? 1 : 0;
    if (!s_trace) return;

    static uint32_t seen[24]; static int n = 0;
    uint32_t key = ((uint32_t)texpage << 16) | clut;
    for (int i = 0; i < n; i++) if (seen[i] == key) return;
    if (n >= 24) return;
    seen[n++] = key;

    char words[256]; int off = 0;
    uint32_t count = gpu->gp0_command_buffer.count;
    if (count > 12) count = 12;
    for (uint32_t i = 0; i < count && off < (int)sizeof(words) - 12; i++)
        off += snprintf(words + off, sizeof(words) - (size_t)off, "%08x ",
                        gpu->gp0_command_buffer.buffer[i]);
    LOG_GPU_INFO("[GPU] %s cmd=0x%02x clut=0x%04x tpage=0x%04x (pageX=%u depth=%u) | %s",
                 what, (unsigned)(gpu->gp0_command_buffer.buffer[0] >> 24),
                 clut, texpage, texpage & 0xF, (texpage >> 7) & 3, words);
}

// ---------------------------------------------------------------------------
// Helper: apply a Texpage attribute to the draw-mode state
//
// PSX-SPX "Texpage Attribute (Parameter for Textured-Polygons commands)":
//   bits 0-8  same as GP0(E1).0-8, bits 9-10 unused (dither and draw-to-display
//   can only be changed via GP0(E1)), bit 11 same as GP0(E1).11.
// A textured polygon therefore *is* a draw-mode write: the page base, the
// semi-transparency mode and the texture depth it carries persist, and the
// following rectangles and lines — which take their page from GP0(E1) state —
// must see them. Without this the primitive drew correctly but every sprite
// after it sampled a stale page.
// ---------------------------------------------------------------------------
// ZS1_GPU_NO_TEXPAGE_ATTR=1 restores the previous behaviour (the attribute is
// used for the primitive only and does not persist) so a regression can be
// bisected against this one change in a single run.
static int texpage_attr_disabled(void) {
    static int cached = -1;
    if (cached < 0) cached = getenv("ZS1_GPU_NO_TEXPAGE_ATTR") ? 1 : 0;
    return cached;
}

static void apply_texpage_attribute(Gpu* gpu, uint16_t texpage) {
    if (texpage_attr_disabled()) return;
    gpu->page_base_x       = (uint8_t)(texpage & 0xF);
    gpu->page_base_y       = (uint8_t)((texpage >> 4) & 1);
    gpu->semi_transparency = (uint8_t)((texpage >> 5) & 3);
    switch ((texpage >> 7) & 3) {
        case 0: gpu->texture_depth = T4Bit;  break;
        case 1: gpu->texture_depth = T8Bit;  break;
        default: gpu->texture_depth = T15Bit; break;  // 3 (reserved) behaves as 2
    }
    /* Bit 11 is deliberately not applied. On a retail v0 GPU it only means
     * "texture disable" once GP1(09h).0 has enabled it, which this core treats
     * as a no-op, and writing it here would flip GPUSTAT.15 on every textured
     * polygon for no modelled effect. */
}

// ---------------------------------------------------------------------------
// NOP / Clear Cache
// ---------------------------------------------------------------------------
static void gp0_nop(Gpu* gpu) { (void)gpu; }

static void gp0_clear_cache(Gpu* gpu) {
    LOG_GPU_DEBUG("[GPU] GP0(0x01): Clear Cache (no-op)");
    (void)gpu;
}

// ---------------------------------------------------------------------------
// GP0(0x02): Fill Rectangle in VRAM
// ---------------------------------------------------------------------------
static void gp0_fill_rectangle(Gpu* gpu) {
    if (gpu->gp0_command_buffer.count < 3) {
        LOG_GPU_ERROR("[GPU] GP0(0x02) Error: Expected 3 words, got %u", gpu->gp0_command_buffer.count);
        return;
    }

    uint32_t color_val = gpu->gp0_command_buffer.buffer[0];
    uint32_t pos_val   = gpu->gp0_command_buffer.buffer[1];
    uint32_t dim_val   = gpu->gp0_command_buffer.buffer[2];

    // GPU-2 FIX: Fill rectangle coordinate masking per PSX-SPX docs:
    //   Xpos = Xpos AND 3F0h  (16-aligned, 10-bit)
    //   Ypos = Ypos AND 1FFh  (9-bit)
    //   Xsiz = ((Xsiz AND 3FFh) + 0Fh) AND NOT 0Fh  (rounded up to 16, max 0x400)
    //   Ysiz = Ysiz AND 1FFh  (max 511 - a Ysiz=0 means no fill)
    // (psx-spx gpu/memory-transfer-commands.md:38-47)
    uint16_t x = (uint16_t)(pos_val & 0x3F0u);
    uint16_t y = (uint16_t)((pos_val >> 16) & 0x1FFu);
    uint16_t w = (uint16_t)(((dim_val & 0x3FFu) + 0x0Fu) & ~0x0Fu);
    uint16_t h = (uint16_t)((dim_val >> 16) & 0x1FFu);

    // Skip zero-size fills (docs: fill does NOT occur when Xsiz=0 or Ysiz=0)
    if (w == 0 || h == 0) return;

    LOG_GPU_DEBUG("[GPU] GP0(0x02): Fill Rect (%u,%u) %ux%u Color=%06x", x, y, w, h, color_val & 0xFFFFFF);
    LOG_VRAM_DEBUG("Fill VRAM rectangle offset=(%u,%u), size=(%u,%u) color=%06x",
                   x, y, w, h, color_val & 0xFFFFFF);

    /* "The hardware linearly converts the 24bit RGB value to 15bit RGB by
     * dropping the lower 3 bits of each color value and additionally sets the
     * mask bit (bit15) to 0", and "Rectangle filling is not affected by the
     * GP0(E6h) mask setting" (psx-spx gpu/i-o-ports-dma-channels-commands-vram.md:86-91).
     * Absolute VRAM coordinates, no drawing offset, no drawing area
     * (memory-transfer-commands.md:85-87), wrapping at both edges with no carry
     * between X and Y (:95-98). */
    const uint16_t pixel = (uint16_t)(((color_val >> 3) & 0x1Fu) |
                                      (((color_val >> 11) & 0x1Fu) << 5) |
                                      (((color_val >> 19) & 0x1Fu) << 10));
    uint16_t* const v = vram16(gpu);
    const uint32_t first = ((uint32_t)x + w > VRAM_WIDTH) ? VRAM_WIDTH - x : w;
    for (uint32_t iy = 0; iy < (uint32_t)h; iy++) {
        uint16_t* row = v + (((uint32_t)y + iy) & 0x1FFu) * VRAM_WIDTH;
        for (uint32_t ix = 0; ix < first; ix++) row[x + ix] = pixel;
        for (uint32_t ix = first; ix < (uint32_t)w; ix++) row[ix - first] = pixel;
    }

    /* The renderer gets the result as a VRAM write, not as a primitive. This
     * used to draw a quad, which the renderer clips to the drawing area and
     * passes through the mask settings like any primitive: a buffer cleared
     * wider than the current drawing area (a 24bpp FMV buffer is 480 halfwords
     * wide) stayed dirty on screen beyond it, and with GP0(E6h).0 set the fill
     * came out with bit 15 on. It also never wrapped. As an upload it lands
     * exactly as the CPU copy holds it, and the flush in the upload keeps it in
     * order with the primitives around it. */
    gpu_vram_push(gpu, x, y, w, h);
    lua_debug_notify("gp0_fill");
}

// ---------------------------------------------------------------------------
// State-setting commands (GP0 E1–E6)
// ---------------------------------------------------------------------------
static void gp0_draw_mode(Gpu* gpu) {
    uint32_t value = gpu->gp0_command_buffer.buffer[0];
    /* ZS1_GPU_TRACE_BATCH=1 also reports every distinct GP0(E1) the guest writes,
     * so a wrong texture depth can be attributed to the guest asking for it or to
     * this decode getting it wrong — the batch trace alone cannot tell them apart. */
    {
        static int s_trace = -1;
        if (s_trace < 0) s_trace = getenv("ZS1_GPU_TRACE_BATCH") ? 1 : 0;
        if (s_trace) {
            static uint32_t seen[32]; static int n = 0;
            bool known = false;
            for (int i = 0; i < n; i++) if (seen[i] == (value & 0x3FFF)) { known = true; break; }
            if (!known && n < 32) {
                seen[n++] = value & 0x3FFF;
                LOG_GPU_INFO("[GPU] GP0(E1) raw=0x%08x -> pageX=%u pageY=%u semi=%u depth=%u "
                             "dither=%u todisp=%u texdis=%u",
                             value, value & 0xF, (value >> 4) & 1, (value >> 5) & 3,
                             (value >> 7) & 3, (value >> 9) & 1, (value >> 10) & 1,
                             (value >> 11) & 1);
            }
        }
    }
    gpu->page_base_x = (uint8_t)(value & 0xF);
    gpu->page_base_y = (uint8_t)((value >> 4) & 1);
    gpu->semi_transparency = (uint8_t)((value >> 5) & 3);
    switch ((value >> 7) & 3) {
        case 0: gpu->texture_depth = T4Bit;  break;
        case 1: gpu->texture_depth = T8Bit;  break;
        case 2: gpu->texture_depth = T15Bit; break;
        default: LOG_GPU_WARN("[GPU] GP0(E1) unknown texture depth %d", (value >> 7) & 3); break;
    }
    gpu->dithering                  = ((value >> 9) & 1) != 0;
    gpu->draw_to_display            = ((value >> 10) & 1) != 0;
    gpu->texture_disable            = ((value >> 11) & 1) != 0;
    gpu->rectangle_texture_x_flip   = ((value >> 12) & 1) != 0;
    gpu->rectangle_texture_y_flip   = ((value >> 13) & 1) != 0;
    LOG_GPU_DEBUG("[GPU] GP0(0xE1): Draw Mode page=(%u,%u) depth=%d semi=%u draw_to_disp=%u tex_disable=%u flip=(%u,%u)",
                 gpu->page_base_x, gpu->page_base_y, (int)gpu->texture_depth,
                 gpu->semi_transparency, (int)gpu->draw_to_display,
                 (int)gpu->texture_disable,
                 (int)gpu->rectangle_texture_x_flip, (int)gpu->rectangle_texture_y_flip);
}

static void gp0_texture_window(Gpu* gpu) {
    uint32_t value = gpu->gp0_command_buffer.buffer[0];
    gpu->texture_window_x_mask   = (uint8_t)(value & 0x1F);
    gpu->texture_window_y_mask   = (uint8_t)((value >> 5) & 0x1F);
    gpu->texture_window_x_offset = (uint8_t)((value >> 10) & 0x1F);
    gpu->texture_window_y_offset = (uint8_t)((value >> 15) & 0x1F);
    renderer_set_texture_window(&gpu->renderer,
        gpu->texture_window_x_mask, gpu->texture_window_y_mask,
        gpu->texture_window_x_offset, gpu->texture_window_y_offset);
    LOG_GPU_DEBUG("[GPU] GP0(0xE2): Texture Window -> Mask(%u,%u) Offset(%u,%u)",
        gpu->texture_window_x_mask, gpu->texture_window_y_mask,
        gpu->texture_window_x_offset, gpu->texture_window_y_offset);
    {
        static int s_trace = -1;
        if (s_trace < 0) s_trace = getenv("ZS1_GPU_TRACE_BATCH") ? 1 : 0;
        if (s_trace) {
            static uint32_t seen[16]; static int n = 0;
            bool known = false;
            for (int i = 0; i < n; i++) if (seen[i] == (value & 0xFFFFF)) { known = true; break; }
            if (!known && n < 16) {
                seen[n++] = value & 0xFFFFF;
                LOG_GPU_INFO("[GPU] GP0(E2) raw=0x%08x -> maskX=%u maskY=%u offX=%u offY=%u "
                             "(window %ux%u at %u,%u)",
                             value, gpu->texture_window_x_mask, gpu->texture_window_y_mask,
                             gpu->texture_window_x_offset, gpu->texture_window_y_offset,
                             (unsigned)((~(gpu->texture_window_x_mask * 8u) & 0xFF) + 1),
                             (unsigned)((~(gpu->texture_window_y_mask * 8u) & 0xFF) + 1),
                             (unsigned)((gpu->texture_window_x_offset & gpu->texture_window_x_mask) * 8u),
                             (unsigned)((gpu->texture_window_y_offset & gpu->texture_window_y_mask) * 8u));
            }
        }
    }
}

static void gp0_drawing_area_top_left(Gpu* gpu) {
    uint32_t value = gpu->gp0_command_buffer.buffer[0];
    gpu->drawing_area_left = (uint16_t)(value & 0x3FF);
    gpu->drawing_area_top  = (uint16_t)((value >> 10) & 0x3FF);
    LOG_GPU_DEBUG("[GPU] GP0(0xE3): Drawing Area TL=(%u, %u)", gpu->drawing_area_left, gpu->drawing_area_top);
    renderer_set_drawing_area(&gpu->renderer,
        gpu->drawing_area_left, gpu->drawing_area_top,
        gpu->drawing_area_right, gpu->drawing_area_bottom);
}

static void gp0_drawing_area_bottom_right(Gpu* gpu) {
    uint32_t value = gpu->gp0_command_buffer.buffer[0];
    gpu->drawing_area_right  = (uint16_t)(value & 0x3FF);
    gpu->drawing_area_bottom = (uint16_t)((value >> 10) & 0x3FF);
    LOG_GPU_DEBUG("[GPU] GP0(0xE4): Drawing Area BR=(%u, %u), Full bounds: [%u..%u, %u..%u]",
        gpu->drawing_area_right, gpu->drawing_area_bottom,
        gpu->drawing_area_left, gpu->drawing_area_right,
        gpu->drawing_area_top, gpu->drawing_area_bottom);
    renderer_set_drawing_area(&gpu->renderer,
        gpu->drawing_area_left, gpu->drawing_area_top,
        gpu->drawing_area_right, gpu->drawing_area_bottom);
}

static void gp0_drawing_offset(Gpu* gpu) {
    uint32_t value = gpu->gp0_command_buffer.buffer[0];
    uint16_t x_raw = (uint16_t)(value & 0x7FF);
    uint16_t y_raw = (uint16_t)((value >> 11) & 0x7FF);
    gpu->drawing_x_offset = (int16_t)(x_raw << 5) >> 5;
    gpu->drawing_y_offset = (int16_t)(y_raw << 5) >> 5;
    LOG_GPU_DEBUG("[GPU] GP0(0xE5): Drawing Offset (%d, %d)", gpu->drawing_x_offset, gpu->drawing_y_offset);
    renderer_set_draw_offset(&gpu->renderer, gpu->drawing_x_offset, gpu->drawing_y_offset);
    gpu_update_display_mapping(gpu);
}

static void gp0_mask_bit_setting(Gpu* gpu) {
    uint32_t value = gpu->gp0_command_buffer.buffer[0];
    gpu->force_set_mask_bit      = (value & 1) != 0;
    gpu->preserve_masked_pixels  = ((value >> 1) & 1) != 0;
    LOG_GPU_DEBUG("[GPU] GP0(0xE6): Mask Bit force=%d preserve=%d",
                  gpu->force_set_mask_bit, gpu->preserve_masked_pixels);
    /* The rasterizer writes bit 15 through the texture's alpha channel, so the
     * force-set flag has to follow the drawing state, not just the CPU-side
     * vram_write_masked() path. */
    renderer_set_mask_mode(&gpu->renderer, gpu->force_set_mask_bit);
    /* Bit 1 is the *test*: skip a pixel whose destination already carries bit 15.
     * The CPU-side path has always honoured it (vram_write_masked), but rasterized
     * primitives ignored it entirely, so anything a game protected by setting the
     * mask bit got overpainted anyway. */
    renderer_set_mask_test(&gpu->renderer, gpu->preserve_masked_pixels);
}

// ---------------------------------------------------------------------------
// GP0(0x1F): Interrupt Request
// ---------------------------------------------------------------------------
static void gp0_interrupt_request(Gpu* gpu) {
    gpu->interrupt = true;
    /* Assert the GPU IRQ line (I_STAT bit 1). Previously this only set the
     * GPUSTAT.24 status flag and never notified the interrupt controller, so
     * any code waiting on the GPU interrupt (games/BIOS place GP0(0x1F) inside
     * an ordering table to fire when the GPU processes that point, then wait on
     * the GPU IRQ / its kernel event to sync frames) would hang. Edge-triggered
     * line, lowered by the GP1(02) acknowledge. */
    if (gpu->inter)
        interconnect_set_irq_line(gpu->inter, IRQ_GPU, true);
    LOG_GPU_DEBUG("[GPU] GP0(0x1F): Interrupt Request");
}

// ---------------------------------------------------------------------------
// Monochrome Triangle (0x20–0x23)
// ---------------------------------------------------------------------------
static void gp0_tri_mono_opaque(Gpu* gpu) {
    if (gpu->gp0_command_buffer.count < 4) return;
    RendererColor col;
    col.r = (uint8_t)(gpu->gp0_command_buffer.buffer[0] & 0xFF);
    col.g = (uint8_t)((gpu->gp0_command_buffer.buffer[0] >> 8) & 0xFF);
    col.b = (uint8_t)((gpu->gp0_command_buffer.buffer[0] >> 16) & 0xFF);
    RendererColor colors[3] = {col, col, col};
    RendererPosition p[3];
    for (int i = 0; i < 3; i++) {
        uint32_t v = gpu->gp0_command_buffer.buffer[i + 1];
        p[i].x = (int16_t)(int16_t)(v & 0xFFFF);
        p[i].y = (int16_t)(int16_t)(v >> 16);
    }
    renderer_set_dither_mode(&gpu->renderer, false); // mono: no dither
    renderer_set_semi_trans_mode(&gpu->renderer, false, 0);
    renderer_set_texture_mode(&gpu->renderer, false);
    LOG_GPU_TRACE("[GPU] Render three-point opaque mono polygon");
    renderer_push_triangle(&gpu->renderer, p, colors, NULL, 0, 0);
}

static void gp0_tri_mono_semi(Gpu* gpu) {
    if (gpu->gp0_command_buffer.count < 4) return;
    RendererColor col;
    col.r = (uint8_t)(gpu->gp0_command_buffer.buffer[0] & 0xFF);
    col.g = (uint8_t)((gpu->gp0_command_buffer.buffer[0] >> 8) & 0xFF);
    col.b = (uint8_t)((gpu->gp0_command_buffer.buffer[0] >> 16) & 0xFF);
    RendererColor colors[3] = {col, col, col};
    RendererPosition p[3];
    for (int i = 0; i < 3; i++) {
        uint32_t v = gpu->gp0_command_buffer.buffer[i + 1];
        p[i].x = (int16_t)(int16_t)(v & 0xFFFF);
        p[i].y = (int16_t)(int16_t)(v >> 16);
    }
    renderer_set_dither_mode(&gpu->renderer, false); // mono: no dither
    renderer_set_semi_trans_mode(&gpu->renderer, true, gpu->semi_transparency);
    renderer_set_texture_mode(&gpu->renderer, false);
    LOG_GPU_TRACE("[GPU] Render three-point semi-transparent mono polygon");
    renderer_push_triangle(&gpu->renderer, p, colors, NULL, 0, 0);
}

// ---------------------------------------------------------------------------
// Textured Triangle (0x24–0x27): cmd, v0, uv0+clut, v1, uv1+tpage, v2, uv2
// ---------------------------------------------------------------------------
static void gp0_tri_tex_impl(Gpu* gpu, bool semi_trans, bool raw_texture) {
    if (gpu->gp0_command_buffer.count < 7) return;
    RendererColor col;
    col.r = (uint8_t)(gpu->gp0_command_buffer.buffer[0] & 0xFF);
    col.g = (uint8_t)((gpu->gp0_command_buffer.buffer[0] >> 8) & 0xFF);
    col.b = (uint8_t)((gpu->gp0_command_buffer.buffer[0] >> 16) & 0xFF);
    RendererColor colors[3] = {col, col, col};
    RendererPosition p[3];
    RendererTexCoord t[3];
    uint16_t clut = 0, texpage = 0;

    p[0].x = (int16_t)(int16_t)(gpu->gp0_command_buffer.buffer[1] & 0xFFFF);
    p[0].y = (int16_t)(int16_t)(gpu->gp0_command_buffer.buffer[1] >> 16);
    t[0].u = (int16_t)(gpu->gp0_command_buffer.buffer[2] & 0xFF);
    t[0].v = (int16_t)((gpu->gp0_command_buffer.buffer[2] >> 8) & 0xFF);
    clut   = (uint16_t)(gpu->gp0_command_buffer.buffer[2] >> 16);

    p[1].x = (int16_t)(int16_t)(gpu->gp0_command_buffer.buffer[3] & 0xFFFF);
    p[1].y = (int16_t)(int16_t)(gpu->gp0_command_buffer.buffer[3] >> 16);
    t[1].u = (int16_t)(gpu->gp0_command_buffer.buffer[4] & 0xFF);
    t[1].v = (int16_t)((gpu->gp0_command_buffer.buffer[4] >> 8) & 0xFF);
    texpage = (uint16_t)(gpu->gp0_command_buffer.buffer[4] >> 16);
    apply_texpage_attribute(gpu, texpage);

    p[2].x = (int16_t)(int16_t)(gpu->gp0_command_buffer.buffer[5] & 0xFFFF);
    p[2].y = (int16_t)(int16_t)(gpu->gp0_command_buffer.buffer[5] >> 16);
    t[2].u = (int16_t)(gpu->gp0_command_buffer.buffer[6] & 0xFF);
    t[2].v = (int16_t)((gpu->gp0_command_buffer.buffer[6] >> 8) & 0xFF);

    // Validate texture coordinates
    uint8_t page_x, page_y, depth;
    bool raw_tex_unused;
    trace_textured_primitive(gpu, "poly", clut, texpage);
    gpu_unpack_tpage(texpage, &page_x, &page_y, &depth, &raw_tex_unused);

    for (int i = 0; i < 3; i++) {
        if (!gpu_validate_texture_coords(page_x, page_y, depth, t[i].u, t[i].v)) {
            LOG_GPU_WARN("[GPU] Textured triangle vertex %d UV out of bounds", i);
        }
    }

    // Validate CLUT if using paletted texture
    if (depth != 2) {  // not 15-bit direct color
        if (!gpu_validate_clut_coords(clut, depth)) {
            LOG_GPU_WARN("[GPU] Textured triangle CLUT out of bounds");
        }
    }

    // Force neutral gray color for raw texture mode (no vertex color modulation)
    if (raw_texture) {
        for (int i = 0; i < 3; i++) {
            colors[i].r = 128;
            colors[i].g = 128;
            colors[i].b = 128;
        }
    }

    // Dither: textured+blend (modulation) uses dithering; raw texture does not
    renderer_set_dither_mode(&gpu->renderer, gpu->dithering && !raw_texture);
    renderer_set_semi_trans_mode(&gpu->renderer, semi_trans, gpu->semi_transparency);
    renderer_set_raw_texture_mode(&gpu->renderer, raw_texture);
    renderer_set_texture_mode(&gpu->renderer, true);
    LOG_GPU_TRACE("[GPU] Render three-point %s textured-%s polygon",
                  semi_trans ? "semi-transparent" : "opaque",
                  raw_texture ? "raw" : "blend");
    renderer_push_triangle(&gpu->renderer, p, colors, t, clut, texpage);
}

static void gp0_tri_tex_blend_opaque(Gpu* gpu) { gp0_tri_tex_impl(gpu, false, false); }
static void gp0_tri_tex_blend_semi(Gpu* gpu)   { gp0_tri_tex_impl(gpu, true,  false); }
static void gp0_tri_tex_raw_opaque(Gpu* gpu)   { gp0_tri_tex_impl(gpu, false, true);  }
static void gp0_tri_tex_raw_semi(Gpu* gpu)     { gp0_tri_tex_impl(gpu, true,  true);  }

// ---------------------------------------------------------------------------
// Shaded Triangle (0x30–0x37)
// ---------------------------------------------------------------------------
static void gp0_tri_shaded_impl(Gpu* gpu, bool semi_trans) {
    if (gpu->gp0_command_buffer.count < 6) return;
    RendererColor c[3]; RendererPosition p[3];
    for (int i = 0; i < 3; i++) {
        uint32_t cw = gpu->gp0_command_buffer.buffer[i * 2];
        uint32_t vw = gpu->gp0_command_buffer.buffer[i * 2 + 1];
        c[i].r = (uint8_t)(cw & 0xFF);
        c[i].g = (uint8_t)((cw >> 8) & 0xFF);
        c[i].b = (uint8_t)((cw >> 16) & 0xFF);
        p[i].x = (int16_t)(int16_t)(vw & 0xFFFF);
        p[i].y = (int16_t)(int16_t)(vw >> 16);
    }
    // Gouraud shaded: always dither when dithering enabled (per PSX spec)
    renderer_set_dither_mode(&gpu->renderer, gpu->dithering);
    renderer_set_semi_trans_mode(&gpu->renderer, semi_trans, gpu->semi_transparency);
    renderer_set_texture_mode(&gpu->renderer, false);
    LOG_GPU_TRACE("[GPU] Render three-point %s shaded polygon rgb0=(%u,%u,%u) v=(%d,%d)(%d,%d)(%d,%d)",
                  semi_trans ? "semi-transparent" : "opaque", c[0].r, c[0].g, c[0].b,
                  p[0].x, p[0].y, p[1].x, p[1].y, p[2].x, p[2].y);
    renderer_push_triangle(&gpu->renderer, p, c, NULL, 0, 0);
}

static void gp0_tri_shaded_opaque(Gpu* gpu) { gp0_tri_shaded_impl(gpu, false); }
static void gp0_tri_shaded_semi(Gpu* gpu)   { gp0_tri_shaded_impl(gpu, true);  }

// ---------------------------------------------------------------------------
// Shaded+Textured Triangle (0x34–0x37)
// Words: c0/cmd, v0, uv0+clut, c1, v1, uv1+tpage, c2, v2, uv2
// ---------------------------------------------------------------------------
static void gp0_tri_shaded_tex_impl(Gpu* gpu, bool semi_trans, bool raw_texture) {
    if (gpu->gp0_command_buffer.count < 9) return;
    RendererColor c[3]; RendererPosition p[3]; RendererTexCoord t[3];
    uint16_t clut = 0, texpage = 0;
    // v0
    c[0].r = (uint8_t)(gpu->gp0_command_buffer.buffer[0] & 0xFF);
    c[0].g = (uint8_t)((gpu->gp0_command_buffer.buffer[0] >> 8) & 0xFF);
    c[0].b = (uint8_t)((gpu->gp0_command_buffer.buffer[0] >> 16) & 0xFF);
    p[0].x = (int16_t)(int16_t)(gpu->gp0_command_buffer.buffer[1] & 0xFFFF);
    p[0].y = (int16_t)(int16_t)(gpu->gp0_command_buffer.buffer[1] >> 16);
    t[0].u = (int16_t)(gpu->gp0_command_buffer.buffer[2] & 0xFF);
    t[0].v = (int16_t)((gpu->gp0_command_buffer.buffer[2] >> 8) & 0xFF);
    clut   = (uint16_t)(gpu->gp0_command_buffer.buffer[2] >> 16);
    // v1
    c[1].r = (uint8_t)(gpu->gp0_command_buffer.buffer[3] & 0xFF);
    c[1].g = (uint8_t)((gpu->gp0_command_buffer.buffer[3] >> 8) & 0xFF);
    c[1].b = (uint8_t)((gpu->gp0_command_buffer.buffer[3] >> 16) & 0xFF);
    p[1].x = (int16_t)(int16_t)(gpu->gp0_command_buffer.buffer[4] & 0xFFFF);
    p[1].y = (int16_t)(int16_t)(gpu->gp0_command_buffer.buffer[4] >> 16);
    t[1].u = (int16_t)(gpu->gp0_command_buffer.buffer[5] & 0xFF);
    t[1].v = (int16_t)((gpu->gp0_command_buffer.buffer[5] >> 8) & 0xFF);
    texpage = (uint16_t)(gpu->gp0_command_buffer.buffer[5] >> 16);
    apply_texpage_attribute(gpu, texpage);
    // v2
    c[2].r = (uint8_t)(gpu->gp0_command_buffer.buffer[6] & 0xFF);
    c[2].g = (uint8_t)((gpu->gp0_command_buffer.buffer[6] >> 8) & 0xFF);
    c[2].b = (uint8_t)((gpu->gp0_command_buffer.buffer[6] >> 16) & 0xFF);
    p[2].x = (int16_t)(int16_t)(gpu->gp0_command_buffer.buffer[7] & 0xFFFF);
    p[2].y = (int16_t)(int16_t)(gpu->gp0_command_buffer.buffer[7] >> 16);
    t[2].u = (int16_t)(gpu->gp0_command_buffer.buffer[8] & 0xFF);
    t[2].v = (int16_t)((gpu->gp0_command_buffer.buffer[8] >> 8) & 0xFF);

    // Validate texture coordinates. raw/blend mode comes from the GP0 opcode
    // (bit24), not from tpage bit15 which is reserved/always 0 on real hardware.
    uint8_t page_x, page_y, depth;
    bool raw_tex_unused;
    trace_textured_primitive(gpu, "poly", clut, texpage);
    gpu_unpack_tpage(texpage, &page_x, &page_y, &depth, &raw_tex_unused);

    for (int i = 0; i < 3; i++) {
        if (!gpu_validate_texture_coords(page_x, page_y, depth, t[i].u, t[i].v)) {
            LOG_GPU_WARN("[GPU] Shaded textured triangle vertex %d UV out of bounds", i);
        }
    }

    // Validate CLUT if using paletted texture
    if (depth != 2) {  // not 15-bit direct color
        if (!gpu_validate_clut_coords(clut, depth)) {
            LOG_GPU_WARN("[GPU] Shaded textured triangle CLUT out of bounds");
        }
    }

    // Force neutral gray color for raw texture mode (no vertex color modulation)
    if (raw_texture) {
        for (int i = 0; i < 3; i++) {
            c[i].r = 128;
            c[i].g = 128;
            c[i].b = 128;
        }
    }

    // Shaded+textured blend: dither when enabled (gouraud + modulation)
    renderer_set_dither_mode(&gpu->renderer, gpu->dithering && !raw_texture);
    renderer_set_semi_trans_mode(&gpu->renderer, semi_trans, gpu->semi_transparency);
    renderer_set_raw_texture_mode(&gpu->renderer, raw_texture);
    renderer_set_texture_mode(&gpu->renderer, true);
    LOG_GPU_TRACE("[GPU] Render three-point %s shaded-textured-%s polygon",
                  semi_trans ? "semi-transparent" : "opaque",
                  raw_texture ? "raw" : "blend");
    renderer_push_triangle(&gpu->renderer, p, c, t, clut, texpage);
}

static void gp0_tri_shaded_tex_blend_opaque(Gpu* gpu) { gp0_tri_shaded_tex_impl(gpu, false, false); }
static void gp0_tri_shaded_tex_blend_semi(Gpu* gpu)   { gp0_tri_shaded_tex_impl(gpu, true,  false); }
static void gp0_tri_shaded_tex_raw_opaque(Gpu* gpu)   { gp0_tri_shaded_tex_impl(gpu, false, true);  }
static void gp0_tri_shaded_tex_raw_semi(Gpu* gpu)     { gp0_tri_shaded_tex_impl(gpu, true,  true);  }

// ---------------------------------------------------------------------------
// Monochrome Quad (0x28–0x2B)
// ---------------------------------------------------------------------------
static void gp0_quad_mono_impl(Gpu* gpu, bool semi_trans) {
    if (gpu->gp0_command_buffer.count < 5) return;
    RendererColor col;
    col.r = (uint8_t)(gpu->gp0_command_buffer.buffer[0] & 0xFF);
    col.g = (uint8_t)((gpu->gp0_command_buffer.buffer[0] >> 8) & 0xFF);
    col.b = (uint8_t)((gpu->gp0_command_buffer.buffer[0] >> 16) & 0xFF);
    RendererColor colors[4] = {col, col, col, col};
    RendererPosition p[4];
    for (int i = 0; i < 4; i++) {
        uint32_t v = gpu->gp0_command_buffer.buffer[i + 1];
        p[i].x = (int16_t)(int16_t)(v & 0xFFFF);
        p[i].y = (int16_t)(int16_t)(v >> 16);
    }
    renderer_set_dither_mode(&gpu->renderer, false); // mono quad: no dither
    renderer_set_semi_trans_mode(&gpu->renderer, semi_trans, gpu->semi_transparency);
    renderer_set_texture_mode(&gpu->renderer, false);
    LOG_GPU_TRACE("[GPU] Render four-point %s mono polygon rgb=(%u,%u,%u) v=(%d,%d)(%d,%d)(%d,%d)(%d,%d)",
                  semi_trans ? "semi-transparent" : "opaque", col.r, col.g, col.b,
                  p[0].x, p[0].y, p[1].x, p[1].y, p[2].x, p[2].y, p[3].x, p[3].y);
    renderer_push_quad(&gpu->renderer, p, colors, NULL, 0, 0);
}

static void gp0_quad_mono_opaque(Gpu* gpu) { gp0_quad_mono_impl(gpu, false); }
static void gp0_quad_mono_semi(Gpu* gpu)   { gp0_quad_mono_impl(gpu, true);  }

// ---------------------------------------------------------------------------
// Textured Quad (0x2C–0x2F)
// Words: cmd, v0, uv0+clut, v1, uv1+tpage, v2, uv2, v3, uv3
// ---------------------------------------------------------------------------
static void gp0_quad_tex_impl(Gpu* gpu, bool semi_trans, bool raw_texture) {
    if (gpu->gp0_command_buffer.count < 9) return;
    RendererColor col;
    col.r = (uint8_t)(gpu->gp0_command_buffer.buffer[0] & 0xFF);
    col.g = (uint8_t)((gpu->gp0_command_buffer.buffer[0] >> 8) & 0xFF);
    col.b = (uint8_t)((gpu->gp0_command_buffer.buffer[0] >> 16) & 0xFF);
    RendererColor c[4] = {col, col, col, col};
    RendererPosition p[4]; RendererTexCoord t[4];
    uint16_t clut = 0, texpage = 0;

    p[0].x = (int16_t)(int16_t)(gpu->gp0_command_buffer.buffer[1] & 0xFFFF);
    p[0].y = (int16_t)(int16_t)(gpu->gp0_command_buffer.buffer[1] >> 16);
    t[0].u = (int16_t)(gpu->gp0_command_buffer.buffer[2] & 0xFF);
    t[0].v = (int16_t)((gpu->gp0_command_buffer.buffer[2] >> 8) & 0xFF);
    clut   = (uint16_t)(gpu->gp0_command_buffer.buffer[2] >> 16);

    p[1].x = (int16_t)(int16_t)(gpu->gp0_command_buffer.buffer[3] & 0xFFFF);
    p[1].y = (int16_t)(int16_t)(gpu->gp0_command_buffer.buffer[3] >> 16);
    t[1].u = (int16_t)(gpu->gp0_command_buffer.buffer[4] & 0xFF);
    t[1].v = (int16_t)((gpu->gp0_command_buffer.buffer[4] >> 8) & 0xFF);
    texpage = (uint16_t)(gpu->gp0_command_buffer.buffer[4] >> 16);
    apply_texpage_attribute(gpu, texpage);

    p[2].x = (int16_t)(int16_t)(gpu->gp0_command_buffer.buffer[5] & 0xFFFF);
    p[2].y = (int16_t)(int16_t)(gpu->gp0_command_buffer.buffer[5] >> 16);
    t[2].u = (int16_t)(gpu->gp0_command_buffer.buffer[6] & 0xFF);
    t[2].v = (int16_t)((gpu->gp0_command_buffer.buffer[6] >> 8) & 0xFF);

    p[3].x = (int16_t)(int16_t)(gpu->gp0_command_buffer.buffer[7] & 0xFFFF);
    p[3].y = (int16_t)(int16_t)(gpu->gp0_command_buffer.buffer[7] >> 16);
    t[3].u = (int16_t)(gpu->gp0_command_buffer.buffer[8] & 0xFF);
    t[3].v = (int16_t)((gpu->gp0_command_buffer.buffer[8] >> 8) & 0xFF);

    // Validate texture coordinates
    uint8_t page_x, page_y, depth;
    bool raw_tex_unused;
    trace_textured_primitive(gpu, "poly", clut, texpage);
    gpu_unpack_tpage(texpage, &page_x, &page_y, &depth, &raw_tex_unused);

    for (int i = 0; i < 4; i++) {
        if (!gpu_validate_texture_coords(page_x, page_y, depth, t[i].u, t[i].v)) {
            LOG_GPU_WARN("[GPU] Textured quad vertex %d UV out of bounds", i);
        }
    }

    // Validate CLUT if using paletted texture
    if (depth != 2) {  // not 15-bit direct color
        if (!gpu_validate_clut_coords(clut, depth)) {
            LOG_GPU_WARN("[GPU] Textured quad CLUT out of bounds");
        }
    }

    // Force neutral gray color for raw texture mode (no vertex color modulation)
    if (raw_texture) {
        for (int i = 0; i < 4; i++) {
            c[i].r = 128;
            c[i].g = 128;
            c[i].b = 128;
        }
    }

    // Dither: textured+blend (modulation) uses dithering; raw texture does not
    renderer_set_dither_mode(&gpu->renderer, gpu->dithering && !raw_texture);
    renderer_set_semi_trans_mode(&gpu->renderer, semi_trans, gpu->semi_transparency);
    renderer_set_raw_texture_mode(&gpu->renderer, raw_texture);
    renderer_set_texture_mode(&gpu->renderer, true);
    {
        uint16_t dbg_page_x = (uint16_t)((texpage & 0xF) * 64);   // 16-bit: pages 0-15 → 0-960
        uint8_t  dbg_page_y = ((texpage >> 4) & 1);
        uint8_t  dbg_depth  = (texpage >> 7) & 3;
        uint16_t dbg_clut_x = (clut & 0x3F) * 16;
        uint16_t dbg_clut_y = (clut >> 6) & 0x1FF;
        LOG_GPU_DEBUG("[GPU] quad tex v0=(%d,%d) v3=(%d,%d) uv0=(%d,%d) uv3=(%d,%d) page_x=%u page_y=%u depth=%u clut=(%u,%u) semi=%d raw=%d tpage_raw=0x%04x",
                      p[0].x, p[0].y, p[3].x, p[3].y,
                      t[0].u, t[0].v, t[3].u, t[3].v,
                      dbg_page_x, (uint16_t)(dbg_page_y * 256),
                      dbg_depth, dbg_clut_x, dbg_clut_y,
                      (int)semi_trans, (int)raw_texture,
                      texpage);
    }
    renderer_push_quad(&gpu->renderer, p, c, t, clut, texpage);
}

static void gp0_quad_tex_blend_opaque(Gpu* gpu) { gp0_quad_tex_impl(gpu, false, false); }
static void gp0_quad_tex_blend_semi(Gpu* gpu)   { gp0_quad_tex_impl(gpu, true,  false); }
static void gp0_quad_tex_raw_opaque(Gpu* gpu)   { gp0_quad_tex_impl(gpu, false, true);  }
static void gp0_quad_tex_raw_semi(Gpu* gpu)     { gp0_quad_tex_impl(gpu, true,  true);  }

// ---------------------------------------------------------------------------
// Shaded Quad (0x38–0x3B)
// ---------------------------------------------------------------------------
static void gp0_quad_shaded_impl(Gpu* gpu, bool semi_trans) {
    if (gpu->gp0_command_buffer.count < 8) return;
    RendererColor c[4]; RendererPosition p[4];
    for (int i = 0; i < 4; i++) {
        uint32_t cw = gpu->gp0_command_buffer.buffer[i * 2];
        uint32_t vw = gpu->gp0_command_buffer.buffer[i * 2 + 1];
        c[i].r = (uint8_t)(cw & 0xFF);
        c[i].g = (uint8_t)((cw >> 8) & 0xFF);
        c[i].b = (uint8_t)((cw >> 16) & 0xFF);
        p[i].x = (int16_t)(int16_t)(vw & 0xFFFF);
        p[i].y = (int16_t)(int16_t)(vw >> 16);
    }
    // Gouraud shaded quad: dither when enabled
    renderer_set_dither_mode(&gpu->renderer, gpu->dithering);
    // Gouraud shaded quad: dither when enabled
    renderer_set_dither_mode(&gpu->renderer, gpu->dithering);
    renderer_set_semi_trans_mode(&gpu->renderer, semi_trans, gpu->semi_transparency);
    renderer_set_texture_mode(&gpu->renderer, false);
    LOG_GPU_TRACE("[GPU] Render four-point %s shaded polygon rgb0=(%u,%u,%u) v=(%d,%d)(%d,%d)(%d,%d)(%d,%d)",
                  semi_trans ? "semi-transparent" : "opaque", c[0].r, c[0].g, c[0].b,
                  p[0].x, p[0].y, p[1].x, p[1].y, p[2].x, p[2].y, p[3].x, p[3].y);
    renderer_push_quad(&gpu->renderer, p, c, NULL, 0, 0);
}

static void gp0_quad_shaded_opaque(Gpu* gpu) { gp0_quad_shaded_impl(gpu, false); }
static void gp0_quad_shaded_semi(Gpu* gpu)   { gp0_quad_shaded_impl(gpu, true);  }

// ---------------------------------------------------------------------------
// Shaded+Textured Quad (0x3C–0x3F)
// Words: c0/cmd, v0, uv0+clut, c1, v1, uv1+tpage, c2, v2, uv2, c3, v3, uv3
// ---------------------------------------------------------------------------
static void gp0_quad_shaded_tex_impl(Gpu* gpu, bool semi_trans, bool raw_texture) {
    if (gpu->gp0_command_buffer.count < 12) return;
    RendererColor c[4]; RendererPosition p[4]; RendererTexCoord t[4];
    uint16_t clut = 0, texpage = 0;
    for (int i = 0; i < 4; i++) {
        c[i].r = (uint8_t)(gpu->gp0_command_buffer.buffer[i * 3] & 0xFF);
        c[i].g = (uint8_t)((gpu->gp0_command_buffer.buffer[i * 3] >> 8) & 0xFF);
        c[i].b = (uint8_t)((gpu->gp0_command_buffer.buffer[i * 3] >> 16) & 0xFF);
        p[i].x = (int16_t)(int16_t)(gpu->gp0_command_buffer.buffer[i * 3 + 1] & 0xFFFF);
        p[i].y = (int16_t)(int16_t)(gpu->gp0_command_buffer.buffer[i * 3 + 1] >> 16);
        t[i].u = (int16_t)(gpu->gp0_command_buffer.buffer[i * 3 + 2] & 0xFF);
        t[i].v = (int16_t)((gpu->gp0_command_buffer.buffer[i * 3 + 2] >> 8) & 0xFF);
        if (i == 0) clut    = (uint16_t)(gpu->gp0_command_buffer.buffer[2] >> 16);
        if (i == 1) texpage = (uint16_t)(gpu->gp0_command_buffer.buffer[5] >> 16);
    }
    apply_texpage_attribute(gpu, texpage);

    // raw/blend mode comes from the GP0 opcode (bit24), not from tpage bit15
    // which is reserved/always 0 on real hardware.
    uint8_t page_x, page_y, depth;
    bool raw_tex_unused;
    trace_textured_primitive(gpu, "poly", clut, texpage);
    gpu_unpack_tpage(texpage, &page_x, &page_y, &depth, &raw_tex_unused);

    // Validate texture coordinates
    for (int i = 0; i < 4; i++) {
        if (!gpu_validate_texture_coords(page_x, page_y, depth, t[i].u, t[i].v)) {
            LOG_GPU_WARN("[GPU] Shaded textured quad vertex %d UV out of bounds", i);
        }
    }

    // Validate CLUT if using paletted texture
    if (depth != 2) {  // not 15-bit direct color
        if (!gpu_validate_clut_coords(clut, depth)) {
            LOG_GPU_WARN("[GPU] Shaded textured quad CLUT out of bounds");
        }
    }

    // Force neutral gray color for raw texture mode (no vertex color modulation)
    if (raw_texture) {
        for (int i = 0; i < 4; i++) {
            c[i].r = 128;
            c[i].g = 128;
            c[i].b = 128;
        }
    }

    // Shaded+textured quad: dither for blend mode (gouraud + modulation)
    renderer_set_dither_mode(&gpu->renderer, gpu->dithering && !raw_texture);
    renderer_set_semi_trans_mode(&gpu->renderer, semi_trans, gpu->semi_transparency);
    renderer_set_raw_texture_mode(&gpu->renderer, raw_texture);
    renderer_set_texture_mode(&gpu->renderer, true);
    LOG_GPU_TRACE("[GPU] Render four-point %s shaded-textured polygon",
                  semi_trans ? "semi-transparent" : "opaque");
    renderer_push_quad(&gpu->renderer, p, c, t, clut, texpage);
}

static void gp0_quad_shaded_tex_blend_opaque(Gpu* gpu) { gp0_quad_shaded_tex_impl(gpu, false, false); }
static void gp0_quad_shaded_tex_blend_semi(Gpu* gpu)   { gp0_quad_shaded_tex_impl(gpu, true,  false); }
static void gp0_quad_shaded_tex_raw_opaque(Gpu* gpu)   { gp0_quad_shaded_tex_impl(gpu, false, true);  }
static void gp0_quad_shaded_tex_raw_semi(Gpu* gpu)     { gp0_quad_shaded_tex_impl(gpu, true,  true);  }

// ---------------------------------------------------------------------------
// Line commands (0x40–0x5F)
// ---------------------------------------------------------------------------

// Monochrome line: cmd+color, v0, v1  (3 words total)
static void gp0_line_mono_impl(Gpu* gpu, bool semi_trans) {
    if (gpu->gp0_command_buffer.count < 3) return;
    RendererColor col;
    col.r = (uint8_t)(gpu->gp0_command_buffer.buffer[0] & 0xFF);
    col.g = (uint8_t)((gpu->gp0_command_buffer.buffer[0] >> 8) & 0xFF);
    col.b = (uint8_t)((gpu->gp0_command_buffer.buffer[0] >> 16) & 0xFF);
    RendererPosition p[2];
    RendererColor c[2] = {col, col};
    p[0].x = (int16_t)(int16_t)(gpu->gp0_command_buffer.buffer[1] & 0xFFFF);
    p[0].y = (int16_t)(int16_t)(gpu->gp0_command_buffer.buffer[1] >> 16);
    p[1].x = (int16_t)(int16_t)(gpu->gp0_command_buffer.buffer[2] & 0xFFFF);
    p[1].y = (int16_t)(int16_t)(gpu->gp0_command_buffer.buffer[2] >> 16);
    // Lines always dither (per PSX spec), regardless of mono/shaded
    renderer_set_dither_mode(&gpu->renderer, gpu->dithering);
    renderer_set_semi_trans_mode(&gpu->renderer, semi_trans, gpu->semi_transparency);
    LOG_GPU_TRACE("[GPU] Render %s mono line", semi_trans ? "semi-transparent" : "opaque");
    renderer_push_line(&gpu->renderer, p, c);
}

static void gp0_line_mono_opaque(Gpu* gpu) { gp0_line_mono_impl(gpu, false); }
static void gp0_line_mono_semi(Gpu* gpu)   { gp0_line_mono_impl(gpu, true);  }

// Shaded line: cmd+c0, v0, c1, v1  (4 words total)
static void gp0_line_shaded_impl(Gpu* gpu, bool semi_trans) {
    if (gpu->gp0_command_buffer.count < 4) return;
    RendererPosition p[2];
    RendererColor c[2];
    c[0].r = (uint8_t)(gpu->gp0_command_buffer.buffer[0] & 0xFF);
    c[0].g = (uint8_t)((gpu->gp0_command_buffer.buffer[0] >> 8) & 0xFF);
    c[0].b = (uint8_t)((gpu->gp0_command_buffer.buffer[0] >> 16) & 0xFF);
    p[0].x = (int16_t)(int16_t)(gpu->gp0_command_buffer.buffer[1] & 0xFFFF);
    p[0].y = (int16_t)(int16_t)(gpu->gp0_command_buffer.buffer[1] >> 16);
    c[1].r = (uint8_t)(gpu->gp0_command_buffer.buffer[2] & 0xFF);
    c[1].g = (uint8_t)((gpu->gp0_command_buffer.buffer[2] >> 8) & 0xFF);
    c[1].b = (uint8_t)((gpu->gp0_command_buffer.buffer[2] >> 16) & 0xFF);
    p[1].x = (int16_t)(int16_t)(gpu->gp0_command_buffer.buffer[3] & 0xFFFF);
    p[1].y = (int16_t)(int16_t)(gpu->gp0_command_buffer.buffer[3] >> 16);
    // Lines always dither
    renderer_set_dither_mode(&gpu->renderer, gpu->dithering);
    renderer_set_semi_trans_mode(&gpu->renderer, semi_trans, gpu->semi_transparency);
    LOG_GPU_TRACE("[GPU] Render %s shaded line", semi_trans ? "semi-transparent" : "opaque");
    renderer_push_line(&gpu->renderer, p, c);
}

static void gp0_line_shaded_opaque(Gpu* gpu) { gp0_line_shaded_impl(gpu, false); }
static void gp0_line_shaded_semi(Gpu* gpu)   { gp0_line_shaded_impl(gpu, true);  }

// Polyline initiators — actual data consumed in the handle_word state machine
static void gp0_polyline_mono_opaque(Gpu* gpu)   {
    gpu->polyline_shaded = false; gpu->polyline_semi_trans = false;
    gpu->polyline_buffer[0] = gpu->gp0_command_buffer.buffer[0];
    gpu->polyline_count = 1;
    gpu->gp0_mode = GP0_MODE_POLYLINE;
}
static void gp0_polyline_mono_semi(Gpu* gpu)    {
    gpu->polyline_shaded = false; gpu->polyline_semi_trans = true;
    gpu->polyline_buffer[0] = gpu->gp0_command_buffer.buffer[0];
    gpu->polyline_count = 1;
    gpu->gp0_mode = GP0_MODE_POLYLINE;
}
static void gp0_polyline_shaded_opaque(Gpu* gpu) {
    gpu->polyline_shaded = true; gpu->polyline_semi_trans = false;
    gpu->polyline_buffer[0] = gpu->gp0_command_buffer.buffer[0];
    gpu->polyline_count = 1;
    gpu->gp0_mode = GP0_MODE_POLYLINE;
}
static void gp0_polyline_shaded_semi(Gpu* gpu) {
    gpu->polyline_shaded = true; gpu->polyline_semi_trans = true;
    gpu->polyline_buffer[0] = gpu->gp0_command_buffer.buffer[0];
    gpu->polyline_count = 1;
    gpu->gp0_mode = GP0_MODE_POLYLINE;
}

// Flush accumulated polyline vertices as line segments
static void flush_polyline(Gpu* gpu) {
    if (gpu->polyline_count < 3) { // Need at least 1 segment (color, v0, v1)
        gpu->polyline_count = 0;
        gpu->gp0_mode = GP0_MODE_COMMAND;
        return;
    }
    LOG_GPU_TRACE("[GPU] Polyline: flush with %u entries", gpu->polyline_count);

    gpu_note_raster(gpu);
    renderer_set_semi_trans_mode(&gpu->renderer, gpu->polyline_semi_trans,
                                  gpu->semi_transparency);

    if (!gpu->polyline_shaded) {
        // Monochrome: buf[0]=cmd+color, buf[1]=v0, buf[2]=v1, buf[3]=v2, ...
        RendererColor col;
        col.r = (uint8_t)(gpu->polyline_buffer[0] & 0xFF);
        col.g = (uint8_t)((gpu->polyline_buffer[0] >> 8) & 0xFF);
        col.b = (uint8_t)((gpu->polyline_buffer[0] >> 16) & 0xFF);
        for (uint32_t i = 1; i + 1 < gpu->polyline_count; i++) {
            RendererPosition p[2];
            RendererColor c[2] = {col, col};
            p[0].x = (int16_t)(int16_t)(gpu->polyline_buffer[i] & 0xFFFF);
            p[0].y = (int16_t)(int16_t)(gpu->polyline_buffer[i] >> 16);
            p[1].x = (int16_t)(int16_t)(gpu->polyline_buffer[i + 1] & 0xFFFF);
            p[1].y = (int16_t)(int16_t)(gpu->polyline_buffer[i + 1] >> 16);
            renderer_push_line(&gpu->renderer, p, c);
        }
    } else {
        // Shaded: buf[0]=cmd+c0, buf[1]=v0, buf[2]=c1, buf[3]=v1, buf[4]=c2, buf[5]=v2, ...
        // Segment k: c0=buf[2k], v0=buf[2k+1], c1=buf[2k+2], v1=buf[2k+3]
        for (uint32_t k = 0; k * 2 + 3 < gpu->polyline_count; k++) {
            uint32_t c0_idx = k * 2;
            uint32_t v0_idx = k * 2 + 1;
            uint32_t c1_idx = k * 2 + 2;
            uint32_t v1_idx = k * 2 + 3;
            RendererPosition p[2];
            RendererColor c[2];
            c[0].r = (uint8_t)(gpu->polyline_buffer[c0_idx] & 0xFF);
            c[0].g = (uint8_t)((gpu->polyline_buffer[c0_idx] >> 8) & 0xFF);
            c[0].b = (uint8_t)((gpu->polyline_buffer[c0_idx] >> 16) & 0xFF);
            p[0].x = (int16_t)(int16_t)(gpu->polyline_buffer[v0_idx] & 0xFFFF);
            p[0].y = (int16_t)(int16_t)(gpu->polyline_buffer[v0_idx] >> 16);
            c[1].r = (uint8_t)(gpu->polyline_buffer[c1_idx] & 0xFF);
            c[1].g = (uint8_t)((gpu->polyline_buffer[c1_idx] >> 8) & 0xFF);
            c[1].b = (uint8_t)((gpu->polyline_buffer[c1_idx] >> 16) & 0xFF);
            p[1].x = (int16_t)(int16_t)(gpu->polyline_buffer[v1_idx] & 0xFFFF);
            p[1].y = (int16_t)(int16_t)(gpu->polyline_buffer[v1_idx] >> 16);
            renderer_push_line(&gpu->renderer, p, c);
        }
    }

    gpu->polyline_count = 0;
    gpu->gp0_mode = GP0_MODE_COMMAND;
}

// ---------------------------------------------------------------------------
// Rectangle commands (0x60–0x7F)
// ---------------------------------------------------------------------------

// Variable-size monochrome rect: cmd+color, vtx, w+h
static void gp0_rect_var_mono_impl(Gpu* gpu, bool semi_trans) {
    if (gpu->gp0_command_buffer.count < 3) return;
    uint32_t cmd = gpu->gp0_command_buffer.buffer[0];
    uint32_t vtx = gpu->gp0_command_buffer.buffer[1];
    uint32_t dim = gpu->gp0_command_buffer.buffer[2];
    RendererColor col = { .r=(uint8_t)(cmd&0xFF), .g=(uint8_t)((cmd>>8)&0xFF), .b=(uint8_t)((cmd>>16)&0xFF) };
    int16_t x = (int16_t)(vtx & 0xFFFF); int16_t y = (int16_t)(vtx >> 16);
    uint16_t w = (uint16_t)(dim & 0xFFFF); uint16_t h = (uint16_t)(dim >> 16);
    if (w == 0) w = 1;
    if (h == 0) h = 1;
    draw_rectangle(gpu, x, y, w, h, col, false, false, NULL, 0, 0, semi_trans);
}

static void gp0_rect_variable_opaque(Gpu* gpu) { gp0_rect_var_mono_impl(gpu, false); }
static void gp0_rect_variable_semi(Gpu* gpu)   { gp0_rect_var_mono_impl(gpu, true);  }

// Variable-size textured rect: cmd+color, vtx, uv+clut, w+h
static void gp0_rect_var_tex_impl(Gpu* gpu, bool semi_trans, bool raw_texture) {
    if (gpu->gp0_command_buffer.count < 4) return;
    uint32_t cmd     = gpu->gp0_command_buffer.buffer[0];
    uint32_t vtx     = gpu->gp0_command_buffer.buffer[1];
    uint32_t uv_clut = gpu->gp0_command_buffer.buffer[2];
    uint32_t dim     = gpu->gp0_command_buffer.buffer[3];
    RendererColor col = { .r=(uint8_t)(cmd&0xFF), .g=(uint8_t)((cmd>>8)&0xFF), .b=(uint8_t)((cmd>>16)&0xFF) };
    int16_t x = (int16_t)(vtx & 0xFFFF); int16_t y = (int16_t)(vtx >> 16);
    RendererTexCoord tex = { .u=(int16_t)(uv_clut & 0xFF), .v=(int16_t)((uv_clut>>8)&0xFF) };
    uint16_t clut  = (uint16_t)(uv_clut >> 16);
    uint16_t tpage = make_tpage(gpu);
    uint16_t w = (uint16_t)(dim & 0xFFFF); uint16_t h = (uint16_t)(dim >> 16);
    if (w == 0) w = 1;
    if (h == 0) h = 1;

    // Validate texture coordinates
    uint8_t page_x, page_y, depth;
    bool raw_tex_unused;
    trace_textured_primitive(gpu, "rect", clut, tpage);
    gpu_unpack_tpage(tpage, &page_x, &page_y, &depth, &raw_tex_unused);

    if (!gpu_validate_texture_coords(page_x, page_y, depth, tex.u, tex.v)) {
        LOG_GPU_WARN("[GPU] Variable textured rect UV (%d,%d) out of bounds", tex.u, tex.v);
    }

    // Validate CLUT if using paletted texture
    if (depth != 2) {  // not 15-bit direct color
        if (!gpu_validate_clut_coords(clut, depth)) {
            LOG_GPU_WARN("[GPU] Variable textured rect CLUT out of bounds");
        }
    }

    draw_rectangle(gpu, x, y, w, h, col, true, raw_texture, &tex, clut, tpage, semi_trans);
}

static void gp0_rect_tex_variable_opaque(Gpu* gpu)     { gp0_rect_var_tex_impl(gpu, false, false); }
static void gp0_rect_tex_variable_semi(Gpu* gpu)       { gp0_rect_var_tex_impl(gpu, true,  false); }
static void gp0_rect_tex_variable_raw_opaque(Gpu* gpu) { gp0_rect_var_tex_impl(gpu, false, true);  }
static void gp0_rect_tex_variable_raw_semi(Gpu* gpu)   { gp0_rect_var_tex_impl(gpu, true,  true);  }

// Fixed-size monochrome rects: cmd+color, vtx
static void gp0_rect_fixed_mono_impl(Gpu* gpu, bool semi_trans, uint16_t size) {
    if (gpu->gp0_command_buffer.count < 2) return;
    uint32_t cmd = gpu->gp0_command_buffer.buffer[0];
    uint32_t vtx = gpu->gp0_command_buffer.buffer[1];
    RendererColor col = { .r=(uint8_t)(cmd&0xFF), .g=(uint8_t)((cmd>>8)&0xFF), .b=(uint8_t)((cmd>>16)&0xFF) };
    int16_t x = (int16_t)(vtx & 0xFFFF); int16_t y = (int16_t)(vtx >> 16);
    draw_rectangle(gpu, x, y, size, size, col, false, false, NULL, 0, 0, semi_trans);
}

static void gp0_rect_1x1_opaque(Gpu* gpu)  { gp0_rect_fixed_mono_impl(gpu, false, 1);  }
static void gp0_rect_1x1_semi(Gpu* gpu)    { gp0_rect_fixed_mono_impl(gpu, true,  1);  }
static void gp0_rect_8x8_opaque(Gpu* gpu)  { gp0_rect_fixed_mono_impl(gpu, false, 8);  }
static void gp0_rect_8x8_semi(Gpu* gpu)    { gp0_rect_fixed_mono_impl(gpu, true,  8);  }
static void gp0_rect_16x16_opaque(Gpu* gpu){ gp0_rect_fixed_mono_impl(gpu, false, 16); }
static void gp0_rect_16x16_semi(Gpu* gpu)  { gp0_rect_fixed_mono_impl(gpu, true,  16); }

// Fixed-size textured rects: cmd+color, vtx, uv+clut
static void gp0_rect_fixed_tex_impl(Gpu* gpu, bool semi_trans, bool raw_texture, uint16_t size) {
    if (gpu->gp0_command_buffer.count < 3) return;
    uint32_t cmd     = gpu->gp0_command_buffer.buffer[0];
    uint32_t vtx     = gpu->gp0_command_buffer.buffer[1];
    uint32_t uv_clut = gpu->gp0_command_buffer.buffer[2];
    RendererColor col = { .r=(uint8_t)(cmd&0xFF), .g=(uint8_t)((cmd>>8)&0xFF), .b=(uint8_t)((cmd>>16)&0xFF) };
    int16_t x = (int16_t)(vtx & 0xFFFF); int16_t y = (int16_t)(vtx >> 16);
    RendererTexCoord tex = { .u=(int16_t)(uv_clut & 0xFF), .v=(int16_t)((uv_clut>>8)&0xFF) };
    uint16_t clut  = (uint16_t)(uv_clut >> 16);
    uint16_t tpage = make_tpage(gpu);

    // Validate texture coordinates
    uint8_t page_x, page_y, depth;
    bool raw_tex_unused;
    trace_textured_primitive(gpu, "rect", clut, tpage);
    gpu_unpack_tpage(tpage, &page_x, &page_y, &depth, &raw_tex_unused);

    if (!gpu_validate_texture_coords(page_x, page_y, depth, tex.u, tex.v)) {
        LOG_GPU_WARN("[GPU] Fixed %ux%u textured rect UV (%d,%d) out of bounds", size, size, tex.u, tex.v);
    }

    // Validate CLUT if using paletted texture
    if (depth != 2) {  // not 15-bit direct color
        if (!gpu_validate_clut_coords(clut, depth)) {
            LOG_GPU_WARN("[GPU] Fixed %ux%u textured rect CLUT out of bounds", size, size);
        }
    }

    draw_rectangle(gpu, x, y, size, size, col, true, raw_texture, &tex, clut, tpage, semi_trans);
}

static void gp0_rect_tex_1x1_opaque(Gpu* gpu)  { gp0_rect_fixed_tex_impl(gpu, false, false, 1);  }
static void gp0_rect_tex_1x1_semi(Gpu* gpu)    { gp0_rect_fixed_tex_impl(gpu, true,  false, 1);  }
static void gp0_rect_tex_1x1_raw(Gpu* gpu)     { gp0_rect_fixed_tex_impl(gpu, false, true,  1);  }
static void gp0_rect_tex_1x1_raw_semi(Gpu* gpu){ gp0_rect_fixed_tex_impl(gpu, true,  true,  1);  }
static void gp0_rect_tex_8x8_opaque(Gpu* gpu)  { gp0_rect_fixed_tex_impl(gpu, false, false, 8);  }
static void gp0_rect_tex_8x8_semi(Gpu* gpu)    { gp0_rect_fixed_tex_impl(gpu, true,  false, 8);  }
static void gp0_rect_tex_8x8_raw(Gpu* gpu)     { gp0_rect_fixed_tex_impl(gpu, false, true,  8);  }
static void gp0_rect_tex_8x8_raw_semi(Gpu* gpu){ gp0_rect_fixed_tex_impl(gpu, true,  true,  8);  }
static void gp0_rect_tex_16x16_opaque(Gpu* gpu)  { gp0_rect_fixed_tex_impl(gpu, false, false, 16); }
static void gp0_rect_tex_16x16_semi(Gpu* gpu)    { gp0_rect_fixed_tex_impl(gpu, true,  false, 16); }
static void gp0_rect_tex_16x16_raw(Gpu* gpu)     { gp0_rect_fixed_tex_impl(gpu, false, true,  16); }
static void gp0_rect_tex_16x16_raw_semi(Gpu* gpu){ gp0_rect_fixed_tex_impl(gpu, true,  true,  16); }

// ---------------------------------------------------------------------------
// VRAM ops (0x80, 0xA0, 0xC0)
// ---------------------------------------------------------------------------

/** GP0(0x80): Copy Rectangle (VRAM to VRAM) */
static void gp0_copy_rectangle(Gpu* gpu) {
    if (gpu->gp0_command_buffer.count < 4) {
        LOG_GPU_ERROR("[GPU] GP0(0x80) Error: Expected 4 words, got %u", gpu->gp0_command_buffer.count);
        return;
    }
    uint32_t src_val = gpu->gp0_command_buffer.buffer[1];
    uint32_t dst_val = gpu->gp0_command_buffer.buffer[2];
    uint32_t dim_val = gpu->gp0_command_buffer.buffer[3];

    uint16_t src_x = (uint16_t)(src_val & 0x3FF);
    uint16_t src_y = (uint16_t)((src_val >> 16) & 0x1FF);
    uint16_t dst_x = (uint16_t)(dst_val & 0x3FF);
    uint16_t dst_y = (uint16_t)((dst_val >> 16) & 0x1FF);
    uint16_t w     = (uint16_t)(dim_val & 0x3FF);
    uint16_t h     = (uint16_t)((dim_val >> 16) & 0x1FF);
    if (w == 0) w = 1024;
    if (h == 0) h = 512;

    LOG_GPU_DEBUG("[GPU] GP0(0x80): VRAM Copy (%u,%u)->(%u,%u) %ux%u", src_x, src_y, dst_x, dst_y, w, h);
    LOG_VRAM_DEBUG("Copy rectangle from VRAM to VRAM src=(%u,%u), dst=(%u,%u), size=(%u,%u) [%u pixels]",
                   src_x, src_y, dst_x, dst_y, w, h, (uint32_t)w * h);

    /* Same reason as GP0(0xC0): the source rect may be something the
     * rasterizer drew, which the CPU-side VRAM has never seen. */
    gpu_vram_pull(gpu, src_x, src_y, w, h);
    /* "The mask setting affects all rendering commands, as well as CPU-to-VRAM
     * and VRAM-to-VRAM transfer commands" (psx-spx gpu/rendering-attributes.md:
     * 165-169). With GP0(E6h).1 set the test is against the destination's
     * bit 15, which has to be the real one, not the CPU copy's. */
    if (gpu->preserve_masked_pixels) gpu_vram_pull(gpu, dst_x, dst_y, w, h);

    // Overlap-safe directional copy, wrapping at the VRAM edges
    // (psx-spx gpu/memory-transfer-commands.md:95-98)
    uint16_t* const v = vram16(gpu);
    const uint16_t set_mask = gpu->force_set_mask_bit ? 0x8000u : 0u;
    const bool     test_mask = gpu->preserve_masked_pixels;
    int16_t step_x = 1, step_y = 1;
    int16_t start_x = 0, start_y = 0, end_x = w, end_y = h;
    if (dst_y > src_y) { step_y = -1; start_y = h - 1; end_y = -1; }
    if (dst_x > src_x) { step_x = -1; start_x = w - 1; end_x = -1; }

    for (int16_t y = start_y; y != end_y; y += step_y) {
        const uint16_t* srow = v + (uint32_t)((src_y + y) & 0x1FF) * VRAM_WIDTH;
        uint16_t*       drow = v + (uint32_t)((dst_y + y) & 0x1FF) * VRAM_WIDTH;
        for (int16_t x = start_x; x != end_x; x += step_x) {
            const uint16_t pixel = srow[(src_x + x) & 0x3FF];
            uint16_t* d = &drow[(dst_x + x) & 0x3FF];
            if (test_mask && (*d & 0x8000u)) continue;
            *d = (uint16_t)(pixel | set_mask);
        }
    }
    gpu_vram_push(gpu, dst_x, dst_y, w, h);
    frame_events_record(FEV_VRAM_COPY, (uint32_t)w * h);
    lua_debug_notify("gp0_vram_copy");
}

/** GP0(0xA0): Copy Rectangle CPU/DMA → VRAM (setup) */
static void gp0_image_load(Gpu* gpu) {
    if (gpu->gp0_command_buffer.count < 3) {
        LOG_GPU_ERROR("[GPU] GP0(0xA0) Error: Expected 3 words, got %u", gpu->gp0_command_buffer.count);
        return;
    }
    uint32_t pos = gpu->gp0_command_buffer.buffer[1];
    uint32_t dim = gpu->gp0_command_buffer.buffer[2];
    gpu->vram_load_x = (uint16_t)(pos & 0x3FF);
    gpu->vram_load_y = (uint16_t)((pos >> 16) & 0x1FF);
    gpu->vram_load_w = (uint16_t)(dim & 0x3FF);
    gpu->vram_load_h = (uint16_t)((dim >> 16) & 0x1FF);
    if (gpu->vram_load_w == 0) gpu->vram_load_w = 1024;
    if (gpu->vram_load_h == 0) gpu->vram_load_h = 512;
    if (gpu->vram_load_w > VRAM_WIDTH)  gpu->vram_load_w = VRAM_WIDTH;
    if (gpu->vram_load_h > VRAM_HEIGHT) gpu->vram_load_h = VRAM_HEIGHT;
    {
        static int s_trace = -1;
        if (s_trace < 0) s_trace = getenv("ZS1_GPU_TRACE_BATCH") ? 1 : 0;
        if (s_trace) {
            static int n = 0;
            if (n < 400 && gpu->vram_load_w > 8) {
                n++;
                if (gpu->gp0_mode == GP0_MODE_IMAGE_LOAD)
                    LOG_GPU_WARN("[GPU] A0: previous upload ABANDONED at %u of %u pixels",
                                 gpu->vram_load_count,
                                 (unsigned)((uint32_t)gpu->vram_load_w * gpu->vram_load_h));
                LOG_GPU_INFO("[GPU] A0 upload -> (%u,%u) %ux%u  [page %u..%u]",
                             gpu->vram_load_x, gpu->vram_load_y,
                             gpu->vram_load_w, gpu->vram_load_h,
                             gpu->vram_load_x / 64u,
                             (gpu->vram_load_x + gpu->vram_load_w - 1) / 64u);
            }
        }
    }

    uint32_t pixels = (uint32_t)gpu->vram_load_w * gpu->vram_load_h;
    uint32_t words  = (pixels + 1) & ~1; words >>= 1;

    LOG_GPU_DEBUG("[GPU] GP0(0xA0): VRAM UPLOAD START (%u,%u) %ux%u = %u words",
                 gpu->vram_load_x, gpu->vram_load_y,
                 gpu->vram_load_w, gpu->vram_load_h, words);

    /* Where the frames actually land, to compare against the display window
     * logged by gpu_update_display_mapping(). Change-only and full-frame-sized
     * only: an FMV re-uses one rectangle for every frame, and a scene is built
     * from hundreds of small texture uploads that say nothing about the window.
     * Both lines at INFO so the comparison needs no DEBUG run — a DEBUG run
     * writes ~1.4M lines and cannot be read. */
    {
        /* A seen-set, not a single previous rectangle. One slot is enough only
         * while a game re-uses one destination; a double-buffered one alternates
         * between two, and the alternation re-arms the gate every field. Dino
         * Crisis flips between (0,0) and (0,256) at 320x240 and produced this
         * line ~25 times a second, burying everything else in the session log
         * and rotating it out entirely. Same fix as gpu.c's display-map gate. */
        enum { UPLOAD_SEEN_MAX = 16 };
        static uint32_t seen_rect[UPLOAD_SEEN_MAX];
        static int      seen_rect_n = 0;
        if (gpu->vram_load_w >= 256 && gpu->vram_load_h >= 128) {
            uint32_t rect = ((uint32_t)gpu->vram_load_x << 22) | ((uint32_t)gpu->vram_load_y << 13) |
                            ((uint32_t)gpu->vram_load_w << 3)  | (gpu->vram_load_h >> 6);
            bool rect_known = false;
            for (int i = 0; i < seen_rect_n; i++)
                if (seen_rect[i] == rect) { rect_known = true; break; }
            if (!rect_known && seen_rect_n < UPLOAD_SEEN_MAX) {
                seen_rect[seen_rect_n++] = rect;
                LOG_GPU_INFO("[GPU] frame upload -> (%u,%u) %ux%u",
                             gpu->vram_load_x, gpu->vram_load_y,
                             gpu->vram_load_w, gpu->vram_load_h);
            }
        }
    }
    LOG_VRAM_DEBUG("Copy rectangle from CPU to VRAM offset=(%u,%u), size=(%u,%u) [%u pixels, %u words] page=%u",
                   gpu->vram_load_x, gpu->vram_load_y,
                   gpu->vram_load_w, gpu->vram_load_h,
                   (uint32_t)gpu->vram_load_w * gpu->vram_load_h, words,
                   gpu->vram_load_x / 64);

    if (words == 0) {
        gpu->gp0_words_remaining = 0;
        gpu->gp0_mode = GP0_MODE_COMMAND;
        return;
    }
    /* With GP0(E6h).1 set an upload must not overwrite pixels whose bit 15 is
     * set (psx-spx gpu/rendering-attributes.md:165-169), and the test is made
     * per pixel against the CPU copy below. A pixel the rasteriser drew is not
     * in the CPU copy, so the destination is brought back first. Only with the
     * test on: without it the old contents do not matter. */
    if (gpu->preserve_masked_pixels)
        gpu_vram_pull(gpu, gpu->vram_load_x, gpu->vram_load_y, gpu->vram_load_w, gpu->vram_load_h);

    gpu->gp0_words_remaining = words;
    gpu->gp0_mode = GP0_MODE_IMAGE_LOAD;
    gpu->vram_load_count = 0;
    lua_debug_notify("gp0_image_start");
}

/** GP0(0xC0): Copy Rectangle VRAM → CPU (setup) */
static void gp0_image_store(Gpu* gpu) {
    if (gpu->gp0_command_buffer.count < 3) {
        LOG_GPU_ERROR("[GPU] GP0(0xC0) Error: Expected 3 words, got %u", gpu->gp0_command_buffer.count);
        return;
    }
    uint32_t val1 = gpu->gp0_command_buffer.buffer[1];
    uint32_t val2 = gpu->gp0_command_buffer.buffer[2];
    /* "Masking for COPY Commands parameters": Xpos AND 3FFh, Ypos AND 1FFh,
     * Xsiz=((Xsiz-1) AND 3FFh)+1, Ysiz=((Ysiz-1) AND 1FFh)+1 (psx-spx
     * gpu/memory-transfer-commands.md:62-67). Sizes used to be clamped instead,
     * so a raw Ysiz of 513 read 512 rows where hardware reads one. */
    uint16_t x = (uint16_t)(val1 & 0x3FF);
    uint16_t y = (uint16_t)((val1 >> 16) & 0x1FF);
    uint16_t w = (uint16_t)((((val2 & 0xFFFFu) - 1u) & 0x3FFu) + 1u);
    uint16_t h = (uint16_t)((((val2 >> 16) - 1u) & 0x1FFu) + 1u);
    gpu->vram_load_x = x; gpu->vram_load_y = y;
    gpu->vram_load_w = w; gpu->vram_load_h = h;
    uint32_t words = ((uint32_t)w * h + 1) / 2;
    gpu->gp0_words_remaining = words;
    gpu->vram_load_count = 0;
    gpu->gp0_mode = GP0_MODE_IMAGE_STORE;

    /* Pull the rendered pixels back before GPUREAD starts serving them. The
     * CPU-side VRAM only ever holds uploads; everything the rasterizer drew
     * lives in vram_tex. The BIOS menu draws its 3D objects, reads them back
     * here, and re-uploads them as textures; without this it reads zeros and
     * the objects vanish. A rectangle that crosses an edge is read as its
     * in-bounds pieces, and GPUREAD wraps the same way (gpu.c). */
    gpu_vram_pull(gpu, x, y, w, h);
    LOG_GPU_DEBUG("[GPU] GP0(0xC0): VRAM->CPU START (%u,%u) %ux%u = %u words", x, y, w, h, words);
    LOG_VRAM_DEBUG("Copy rectangle from VRAM to CPU offset=(%u,%u), size=(%u,%u) [%u pixels, %u words] page=%u",
                   x, y, w, h, (uint32_t)w * h, words, x / 64);
    if (trace_batch_enabled() && w > 8)
        LOG_GPU_INFO("[GPU] C0 readback <- (%u,%u) %ux%u  [page %u..%u]",
                     x, y, w, h, x / 64, (x + w - 1) / 64);
}

// ---------------------------------------------------------------------------
// GP0 Command Dispatch Table
// ---------------------------------------------------------------------------

typedef struct {
    uint8_t  words;         // Number of command words (including the first)
    void (*handler)(Gpu*);  // Handler function
} Gp0TableEntry;

// Polyline mode: 0xFF means "enter polyline mode" — handled specially
#define POLY_WORDS 0xFF

static const Gp0TableEntry gp0_table[256] = {
    // NOP/Cache
    [0x00] = {1, gp0_nop},
    [0x01] = {1, gp0_clear_cache},
    [0x02] = {3, gp0_fill_rectangle},
    [0x1F] = {1, gp0_interrupt_request},

    // Monochrome poly (flat: 4 words for tri, 5 for quad)
    [0x20] = {4, gp0_tri_mono_opaque},
    [0x21] = {4, gp0_tri_mono_opaque},
    [0x22] = {4, gp0_tri_mono_semi},
    [0x23] = {4, gp0_tri_mono_semi},
    [0x28] = {5, gp0_quad_mono_opaque},
    [0x29] = {5, gp0_quad_mono_opaque},
    [0x2A] = {5, gp0_quad_mono_semi},
    [0x2B] = {5, gp0_quad_mono_semi},

    // Textured tri (7 words)
    [0x24] = {7, gp0_tri_tex_blend_opaque},
    [0x25] = {7, gp0_tri_tex_raw_opaque},
    [0x26] = {7, gp0_tri_tex_blend_semi},
    [0x27] = {7, gp0_tri_tex_raw_semi},

    // Textured quad (9 words)
    [0x2C] = {9, gp0_quad_tex_blend_opaque},
    [0x2D] = {9, gp0_quad_tex_raw_opaque},
    [0x2E] = {9, gp0_quad_tex_blend_semi},
    [0x2F] = {9, gp0_quad_tex_raw_semi},

    // Shaded tri (6 words: c0/cmd, v0, c1, v1, c2, v2)
    [0x30] = {6, gp0_tri_shaded_opaque},
    [0x31] = {6, gp0_tri_shaded_opaque},
    [0x32] = {6, gp0_tri_shaded_semi},
    [0x33] = {6, gp0_tri_shaded_semi},

    // Shaded+textured tri (9 words)
    [0x34] = {9, gp0_tri_shaded_tex_blend_opaque},
    [0x35] = {9, gp0_tri_shaded_tex_raw_opaque},
    [0x36] = {9, gp0_tri_shaded_tex_blend_semi},
    [0x37] = {9, gp0_tri_shaded_tex_raw_semi},

    // Shaded quad (8 words)
    [0x38] = {8, gp0_quad_shaded_opaque},
    [0x39] = {8, gp0_quad_shaded_opaque},
    [0x3A] = {8, gp0_quad_shaded_semi},
    [0x3B] = {8, gp0_quad_shaded_semi},

    // Shaded+textured quad (12 words)
    [0x3C] = {12, gp0_quad_shaded_tex_blend_opaque},
    [0x3D] = {12, gp0_quad_shaded_tex_raw_opaque},
    [0x3E] = {12, gp0_quad_shaded_tex_blend_semi},
    [0x3F] = {12, gp0_quad_shaded_tex_raw_semi},

    // Lines (monochrome: 3 words, shaded: 4 words)
    [0x40] = {3, gp0_line_mono_opaque},
    [0x41] = {3, gp0_line_mono_opaque},
    [0x42] = {3, gp0_line_mono_semi},
    [0x43] = {3, gp0_line_mono_semi},
    [0x50] = {4, gp0_line_shaded_opaque},
    [0x51] = {4, gp0_line_shaded_opaque},
    [0x52] = {4, gp0_line_shaded_semi},
    [0x53] = {4, gp0_line_shaded_semi},

    // Polylines (variable length: 1 word for initial entry then handled by state machine)
    [0x48] = {1, gp0_polyline_mono_opaque},
    [0x49] = {1, gp0_polyline_mono_opaque},
    [0x4A] = {1, gp0_polyline_mono_semi},
    [0x4B] = {1, gp0_polyline_mono_semi},
    [0x58] = {1, gp0_polyline_shaded_opaque},
    [0x59] = {1, gp0_polyline_shaded_opaque},
    [0x5A] = {1, gp0_polyline_shaded_semi},
    [0x5B] = {1, gp0_polyline_shaded_semi},

    // Variable rect (mono: 3 words, textured: 4 words)
    [0x60] = {3, gp0_rect_variable_opaque},
    [0x61] = {3, gp0_rect_variable_opaque},
    [0x62] = {3, gp0_rect_variable_semi},
    [0x63] = {3, gp0_rect_variable_semi},
    [0x64] = {4, gp0_rect_tex_variable_opaque},
    [0x65] = {4, gp0_rect_tex_variable_raw_opaque},
    [0x66] = {4, gp0_rect_tex_variable_semi},
    [0x67] = {4, gp0_rect_tex_variable_raw_semi},

    // 1x1 rect
    [0x68] = {2, gp0_rect_1x1_opaque},
    [0x69] = {2, gp0_rect_1x1_opaque},
    [0x6A] = {2, gp0_rect_1x1_semi},
    [0x6B] = {2, gp0_rect_1x1_semi},
    [0x6C] = {3, gp0_rect_tex_1x1_opaque},
    [0x6D] = {3, gp0_rect_tex_1x1_raw},
    [0x6E] = {3, gp0_rect_tex_1x1_semi},
    [0x6F] = {3, gp0_rect_tex_1x1_raw_semi},

    // 8x8 rect
    [0x70] = {2, gp0_rect_8x8_opaque},
    [0x71] = {2, gp0_rect_8x8_opaque},
    [0x72] = {2, gp0_rect_8x8_semi},
    [0x73] = {2, gp0_rect_8x8_semi},
    [0x74] = {3, gp0_rect_tex_8x8_opaque},
    [0x75] = {3, gp0_rect_tex_8x8_raw},
    [0x76] = {3, gp0_rect_tex_8x8_semi},
    [0x77] = {3, gp0_rect_tex_8x8_raw_semi},

    // 16x16 rect
    [0x78] = {2, gp0_rect_16x16_opaque},
    [0x79] = {2, gp0_rect_16x16_opaque},
    [0x7A] = {2, gp0_rect_16x16_semi},
    [0x7B] = {2, gp0_rect_16x16_semi},
    [0x7C] = {3, gp0_rect_tex_16x16_opaque},
    [0x7D] = {3, gp0_rect_tex_16x16_raw},
    [0x7E] = {3, gp0_rect_tex_16x16_semi},
    [0x7F] = {3, gp0_rect_tex_16x16_raw_semi},

    // VRAM ops
    [0x80] = {4, gp0_copy_rectangle},
    [0x81] = {4, gp0_copy_rectangle},
    [0x82] = {4, gp0_copy_rectangle},
    [0x83] = {4, gp0_copy_rectangle},
    [0x84] = {4, gp0_copy_rectangle},
    [0x85] = {4, gp0_copy_rectangle},
    [0x86] = {4, gp0_copy_rectangle},
    [0x87] = {4, gp0_copy_rectangle},
    [0x88] = {4, gp0_copy_rectangle},
    [0x89] = {4, gp0_copy_rectangle},
    [0x8A] = {4, gp0_copy_rectangle},
    [0x8B] = {4, gp0_copy_rectangle},
    [0x8C] = {4, gp0_copy_rectangle},
    [0x8D] = {4, gp0_copy_rectangle},
    [0x8E] = {4, gp0_copy_rectangle},
    [0x8F] = {4, gp0_copy_rectangle},
    [0x90] = {4, gp0_copy_rectangle},
    [0x91] = {4, gp0_copy_rectangle},
    [0x92] = {4, gp0_copy_rectangle},
    [0x93] = {4, gp0_copy_rectangle},
    [0x94] = {4, gp0_copy_rectangle},
    [0x95] = {4, gp0_copy_rectangle},
    [0x96] = {4, gp0_copy_rectangle},
    [0x97] = {4, gp0_copy_rectangle},
    [0x98] = {4, gp0_copy_rectangle},
    [0x99] = {4, gp0_copy_rectangle},
    [0x9A] = {4, gp0_copy_rectangle},
    [0x9B] = {4, gp0_copy_rectangle},
    [0x9C] = {4, gp0_copy_rectangle},
    [0x9D] = {4, gp0_copy_rectangle},
    [0x9E] = {4, gp0_copy_rectangle},
    [0x9F] = {4, gp0_copy_rectangle},

    [0xA0] = {3, gp0_image_load},
    [0xC0] = {3, gp0_image_store},

    // State commands
    [0xE1] = {1, gp0_draw_mode},
    [0xE2] = {1, gp0_texture_window},
    [0xE3] = {1, gp0_drawing_area_top_left},
    [0xE4] = {1, gp0_drawing_area_bottom_right},
    [0xE5] = {1, gp0_drawing_offset},
    [0xE6] = {1, gp0_mask_bit_setting},
    // 0x03-0x1E and unhandled: NULL handler = NOP
};

// ---------------------------------------------------------------------------
// GP0 word processor (the dispatch engine)
// ---------------------------------------------------------------------------

static void gpu_gp0_handle_word(Gpu* gpu, uint32_t word) {
    static uint32_t gp0_cmd_count = 0;
    gp0_cmd_count++;

    // --- Polyline accumulation mode ---
    if (gpu->gp0_mode == GP0_MODE_POLYLINE) {
        // Terminator: 0x50005000 (or 0x55555555 for some variants)
        if ((word & 0xF000F000) == 0x50005000) {
            flush_polyline(gpu);
            return;
        }
        if (gpu->polyline_count < 256) {
            LOG_GPU_TRACE("[GPU] Polyline: add word 0x%08x (entry %u)", word, gpu->polyline_count);
            gpu->polyline_buffer[gpu->polyline_count++] = word;
        }
        return;
    }

    // --- Image load mode ---
    if (gpu->gp0_mode == GP0_MODE_IMAGE_LOAD) {
        /* Two pixels per word, row by row from (vram_load_x, vram_load_y),
         * wrapping at the right and bottom edges of VRAM with no carry from X
         * into Y (psx-spx gpu/memory-transfer-commands.md:95-98). Writes past
         * column 1023 or line 511 used to be dropped.
         *
         * One divide per word: the column and row of the first pixel come from
         * the same division (the compiler takes quotient and remainder from one
         * instruction), the second pixel steps from there. */
        const uint32_t w     = gpu->vram_load_w;
        const uint32_t total = w * gpu->vram_load_h;
        uint32_t idx = gpu->vram_load_count;
        if (idx < total) {
            uint32_t col = idx % w, row = idx / w;
            const bool plain = !gpu->force_set_mask_bit && !gpu->preserve_masked_pixels;
            uint16_t* const v = vram16(gpu);
            for (int k = 0; k < 2 && idx < total; k++, idx++) {
                const uint16_t px  = k ? (uint16_t)(word >> 16) : (uint16_t)(word & 0xFFFF);
                const uint32_t off = (((uint32_t)gpu->vram_load_y + row) & 0x1FFu) * VRAM_WIDTH
                                   + (((uint32_t)gpu->vram_load_x + col) & 0x3FFu);
                if (plain) v[off] = px;
                else       vram_write_masked(gpu, off * VRAM_BPP, px);
                if (++col == w) { col = 0; row++; }
            }
        }

        gpu->vram_load_count += 2;
        gpu->gp0_words_remaining--;
        if (gpu->gp0_words_remaining == 0) {
            gpu->gp0_mode = GP0_MODE_COMMAND;
            if (trace_batch_enabled() && gpu->vram_load_w > 8) {
                LOG_GPU_INFO("[GPU] A0 mask state at completion: force_set=%d preserve=%d",
                             gpu->force_set_mask_bit, gpu->preserve_masked_pixels);
                LOG_GPU_INFO("[GPU] A0 COMPLETE (%u,%u) %ux%u — %u of %u pixels written",
                             gpu->vram_load_x, gpu->vram_load_y,
                             gpu->vram_load_w, gpu->vram_load_h, gpu->vram_load_count,
                             (unsigned)((uint32_t)gpu->vram_load_w * gpu->vram_load_h));
            }
            gpu_vram_push(gpu, gpu->vram_load_x, gpu->vram_load_y,
                          gpu->vram_load_w, gpu->vram_load_h);
            frame_events_record(FEV_VRAM_UPLOAD,
                                (uint32_t)gpu->vram_load_w * gpu->vram_load_h);
            lua_debug_notify("gp0_vram_upload");
            LOG_GPU_DEBUG("[GPU] GP0(0xA0): VRAM UPLOAD COMPLETE (%u,%u) %ux%u",
                         gpu->vram_load_x, gpu->vram_load_y,
                         gpu->vram_load_w, gpu->vram_load_h);
            LOG_VRAM_DEBUG("CPU to VRAM transfer complete: offset=(%u,%u), size=(%u,%u) [%u pixels] page=%u",
                           gpu->vram_load_x, gpu->vram_load_y,
                           gpu->vram_load_w, gpu->vram_load_h,
                           (uint32_t)gpu->vram_load_w * gpu->vram_load_h,
                           gpu->vram_load_x / 64);
        }
        return;
    }

    // --- Command mode ---
    if (gpu->gp0_words_remaining == 0) {
        // Start of a new command
        uint8_t opcode = (uint8_t)(word >> 24);

        /* Line commands decode only bit 28 (gouraud), bit 27 (polyline) and
         * bit 25 (semi-transparent) — bits 26 and 24 are not decoded at all
         * (DOCS/graphicsprocessingunitgpu.md:219-229). The dispatch table is
         * indexed by the exact byte, so 0x4C, 0x4E and 0x55 — the same
         * commands with the undecoded bits set — were dropped as unhandled and
         * the lines never drawn. Ace Combat 2 sends all three. */
        if ((opcode >> 5) == 0x2)
            opcode &= (uint8_t)~0x05u;
        /* The three VRAM transfer commands are decoded from the top 3 bits
         * alone: "For them, the remaining 29 bits are ignored, and can be set
         * to any arbitrary value" (psx-spx gpu/memory-transfer-commands.md:3-5).
         * The table only had A0h and C0h, so A1h..BFh and C1h..DFh were taken as
         * unhandled one-word commands and the image data after them was
         * interpreted as commands. */
        else if (opcode >= 0x80 && opcode < 0xE0)
            opcode &= 0xE0u;

        gpu->gp0_current_opcode = opcode;
        gpu_clear_cmd_buf(gpu);

        Gp0TableEntry entry = gp0_table[opcode];
        if (entry.handler == NULL) {
            // Unhandled — NOP and consume 1 word
            if (gp0_cmd_count <= 20 || gp0_cmd_count % 5000 == 0)
                LOG_GPU_WARN("[GPU] Unhandled GP0 opcode 0x%02x (cmd 0x%08x) #%u @ 0x%08x", opcode, word, gp0_cmd_count);
            gpu_push_cmd_word(gpu, word);
            return;
        }

        gpu->gp0_words_remaining = entry.words;
        gpu->gp0_command_method  = entry.handler;

        if (opcode >= 0x20 && opcode <= 0x3F) lua_debug_notify("gp0_poly");
        else if (opcode >= 0x60 && opcode <= 0x7F) lua_debug_notify("gp0_rect");
    }

    gpu_push_cmd_word(gpu, word);
    gpu->gp0_words_remaining--;

    if (gpu->gp0_words_remaining == 0) {
        if (gpu->gp0_current_opcode >= 0x20 && gpu->gp0_current_opcode <= 0x3F)
            lua_debug_notify("gp0_poly_complete");
        /* Polygons, lines and rectangles (20h..7Fh) are the only commands that
         * rasterise; polylines are drawn later by flush_polyline, which marks
         * again. */
        if (gpu->gp0_current_opcode >= 0x20 && gpu->gp0_current_opcode <= 0x7F)
            gpu_note_raster(gpu);
        if (gpu->gp0_command_method)
            gpu->gp0_command_method(gpu);
        if (gpu->gp0_mode == GP0_MODE_COMMAND)
            gpu_clear_cmd_buf(gpu);
    }
}

// ---------------------------------------------------------------------------
// Public GP0 entry point
// ---------------------------------------------------------------------------
void gpu_gp0(Gpu* gpu, uint32_t command) {
    // Drain FIFO when full before enqueueing
    if (gpu->gp0_fifo_count >= 16) {
        while (gpu->gp0_fifo_count > 0) {
            uint32_t w = gpu->gp0_fifo[gpu->gp0_fifo_head];
            gpu->gp0_fifo_head = (uint8_t)((gpu->gp0_fifo_head + 1) & 0x0F);
            gpu->gp0_fifo_count--;
            gpu_gp0_handle_word(gpu, w);
        }
    }
    gpu->gp0_fifo[gpu->gp0_fifo_tail] = command;
    gpu->gp0_fifo_tail = (uint8_t)((gpu->gp0_fifo_tail + 1) & 0x0F);
    gpu->gp0_fifo_count++;

    while (gpu->gp0_fifo_count > 0) {
        uint32_t w = gpu->gp0_fifo[gpu->gp0_fifo_head];
        gpu->gp0_fifo_head = (uint8_t)((gpu->gp0_fifo_head + 1) & 0x0F);
        gpu->gp0_fifo_count--;
        gpu_gp0_handle_word(gpu, w);
    }
}
