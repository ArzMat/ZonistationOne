/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 ZioZoni95
 *
 * Part of ZoniStation One, a PlayStation 1 emulator.
 * See LICENSE for the full licence text and THIRD-PARTY.md for the
 * components of this project that have other authors.
 */
#include "renderer_gl.h"
#include "log.h"
#include "lua_debug.h"
#include "frame_events.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * GPU Thread — Double-Buffered Batch Recording (Phase 2)
 *
 * CPU thread calls glr_draw() → records a GpuBatch into s_frame[write_idx].
 * At frame end, glr_submit_frame() swaps write_idx and wakes GPU thread.
 * GPU thread drains s_frame[read_idx], executing all GL calls.
 * CPU runs next frame immediately while GPU renders previous frame.
 * ========================================================================= */

/* Ace Combat 2 draws well past a thousand primitives in a frame once the line
 * commands it sends are actually accepted. At 1024 the tail of every busy
 * frame was dropped with "batch overflow". */
#define GPU_MAX_BATCHES        8192
/* An FMV frame arrives as dozens of narrow VRAM upload strips per frame, on
 * top of the 2 MB VRAM-viewer snapshot and any full-VRAM upload, so both the
 * update count and the staging pool have to carry a whole frame's worth or
 * the tail of every frame is dropped ("VRAM pool full — skipping rect"). */
#define GPU_MAX_VRAM_UPDATES   1024
#define GPU_VRAM_POOL_SIZE     (16 * 1024 * 1024)  /* 16 MB per slot */

typedef struct {
    uint32_t vertex_start;      /* index into s_pos/col/tex/tpg pools */
    uint32_t vertex_count;
    bool is_lines;              /* true → GL_LINES, false → GL_TRIANGLES */
    /* GlRenderer state snapshot */
    bool texture_enabled;
    bool raw_texture_enabled;
    bool semi_trans_enabled;
    bool dither_enabled;
    bool set_mask_enabled;      /* GP0(E6).0 — force bit 15 on drawn pixels */
    bool mask_test_enabled;     /* GP0(E6).1 — skip pixels whose destination bit 15 is set */
    uint8_t semi_trans_mode;
    float screen_w, screen_h;
    int16_t offset_x, offset_y;
    int32_t tex_window[4];      /* and_x, and_y, or_x, or_y */
    int32_t scissor[4];         /* gl_x, gl_y, clip_w, clip_h */
} GpuBatch;

typedef struct {
    uint16_t x, y, w, h;
    uint32_t data_offset;       /* byte offset into s_vram_pool[slot] */
    bool     update_display;    /* true → a real VRAM write: also store into vram_tex */
    bool     full_upload;       /* unused, kept for alignment */
    bool     is_viewer;         /* true → upload to vram_viewer_texture (RGBA8) */
} GpuVramUpdate;

/* Ops record VRAM updates and draw batches in the exact order they were
 * submitted by the CPU thread. A VRAM texture page can be uploaded to,
 * drawn from, then re-uploaded within the same frame (e.g. text glyphs
 * reusing a page previously holding a sprite) — executing all VRAM updates
 * before any draw would apply the *later* upload before the *earlier* draw
 * runs, corrupting whatever that draw was supposed to sample. */
typedef enum { GPU_OP_VRAM_UPDATE, GPU_OP_BATCH } GpuOpType;
typedef struct { GpuOpType type; uint32_t index; } GpuOp;
#define GPU_MAX_OPS (GPU_MAX_BATCHES + GPU_MAX_VRAM_UPDATES)

typedef struct {
    GpuBatch       batches[GPU_MAX_BATCHES];
    uint32_t       batch_count;
    GpuVramUpdate  vram_updates[GPU_MAX_VRAM_UPDATES];
    uint32_t       vram_update_count;
    GpuOp          ops[GPU_MAX_OPS];
    uint32_t       op_count;
    void*          imgui_draw_data;  /* ImDrawData* — valid until next NewFrame */
    uint16_t       disp_x, disp_y, disp_w, disp_h;  /* snapshot of CRTC display region */
    bool           disp_depth24;     /* snapshot of GPUSTAT.21 for the scanout pass */
    bool           disp_blank;       /* snapshot of GPUSTAT.23 - display off */
    /* The viewer decode is driven from the UI thread; snapshot it with the
     * frame rather than letting the GPU thread read renderer->vram_view live. */
    VramViewParams view;
} GpuFrame;

/* Record submission order for the GPU thread — see GpuOp comment above. */
static inline void gpu_frame_record_op(GpuFrame* frame, GpuOpType type, uint32_t index) {
    if (frame->op_count < GPU_MAX_OPS)
        frame->ops[frame->op_count++] = (GpuOp){ type, index };
}

/* Double-buffered vertex pools — in BSS (static), not on stack */
static RendererPosition s_pos[2][VERTEX_BUFFER_LEN];
static RendererColor    s_col[2][VERTEX_BUFFER_LEN];
static RendererTexCoord s_tex[2][VERTEX_BUFFER_LEN];
static RendererTPage    s_tpg[2][VERTEX_BUFFER_LEN];
static uint32_t         s_vtx[2];   /* vertex pool write position per slot */

/* Double-buffered VRAM copy pools */
static uint8_t   s_vram_pool[2][GPU_VRAM_POOL_SIZE];
static uint32_t  s_vram_pool_used[2];
static uint32_t  s_vram_pool_skips;      /* rects dropped because the pool was full */
static uint32_t  s_vram_pool_peak;       /* high-water mark of a single frame's usage */

/* Frame command lists */
static GpuFrame  s_frame[2];

/* =========================================================================
 * Phase 5 — cross-thread VRAM readback
 *
 * The GPU thread owns the GL context, so nothing on the CPU side can read
 * vram_tex directly. This is the request/response channel: the CPU raises a
 * request, the GPU thread services it at a point in the frame where vram_tex
 * holds every rasterized pixel, and publishes the result plus a sequence
 * number the CPU polls.
 *
 * Deliberately asynchronous. A synchronous mid-frame readback — what
 * GP0(0xC0) and GP0(0x80) will eventually need — requires a partial frame
 * flush, because the ops the caller wants to read back are still sitting
 * unexecuted in the CPU's write slot. That is a change to the frame protocol
 * and is not this. What is here serves the Inspector's CPU-vs-GPU comparison,
 * which only ever wants "the VRAM as of some recent frame".
 * ========================================================================= */
/* Whether this driver supports reading the render target with a barrier. Decided
 * once at init; picks the texture the shader samples and whether barriers are
 * emitted between batches. */
static bool s_texture_barrier = false;

static SDL_AtomicInt s_readback_request;   /* CPU sets 1; GPU clears when done */
static SDL_AtomicInt s_readback_seq;       /* incremented after each completed readback */
/* Two buffers, published by index. The GPU thread fills the one readers are
 * not holding and then flips s_readback_pub. One buffer was enough while the
 * emulation thread only ran with the GPU thread idle; since main.c emulates the
 * next field while the GPU thread finishes the previous one, a Lua probe that
 * reads the result from inside the emulation (emu.vram_map, emu.vram_compare)
 * could otherwise be reading the buffer while the GPU thread rewrites it. The
 * GPU thread services at most one request per frame, and the emulation thread
 * never runs more than one frame ahead, so a reader's buffer is never the one
 * being written. */
static uint16_t     s_readback_vram[2][1024 * 512];   /* packed 1555, mask bit in 15 */
static SDL_AtomicInt s_readback_pub;       /* index of the buffer readers get */
static uint8_t      s_readback_rgba[1024 * 512 * 4];

/* GPU thread. Unpacks vram_tex (RGBA8, 5:5:5:1 expanded as (v<<3)|(v>>2),
 * alpha carrying the PSX mask bit) back into PSX halfwords. */
static void glr_service_vram_readback(GlRenderer* renderer) {
    if (!SDL_GetAtomicInt(&s_readback_request)) return;

    glBindTexture(GL_TEXTURE_2D, renderer->vram_tex);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, s_readback_rgba);
    glBindTexture(GL_TEXTURE_2D, 0);

    const int back = 1 - SDL_GetAtomicInt(&s_readback_pub);
    uint16_t* out = s_readback_vram[back];
    for (uint32_t i = 0; i < 1024u * 512u; i++) {
        const uint8_t* p = &s_readback_rgba[i * 4];
        out[i] = (uint16_t)(((uint16_t)(p[0] >> 3))
                                      | ((uint16_t)(p[1] >> 3) << 5)
                                      | ((uint16_t)(p[2] >> 3) << 10)
                                      | (p[3] >= 128 ? 0x8000u : 0u));
    }

    SDL_SetAtomicInt(&s_readback_pub, back);
    SDL_SetAtomicInt(&s_readback_request, 0);
    SDL_AddAtomicInt(&s_readback_seq, 1);
}

void glr_request_vram_readback(GlRenderer* renderer) {
    if (!renderer || !renderer->gpu_thread) return;
    SDL_SetAtomicInt(&s_readback_request, 1);
}

const uint16_t* glr_get_vram_readback(uint32_t* seq_out) {
    if (seq_out) *seq_out = (uint32_t)SDL_GetAtomicInt(&s_readback_seq);
    return s_readback_vram[SDL_GetAtomicInt(&s_readback_pub)];
}

/* --- Synchronous mid-frame readback (GP0(0xC0), GP0(0x80)) ----------------
 *
 * What the paragraph above called "not this". The BIOS menu needs it: it
 * draws an object with polygons, reads the result back with GP0(0xC0), and
 * re-uploads it as a texture with GP0(0xA0). Reading the CPU-side VRAM there
 * returns zeros, because rasterized pixels only ever existed in vram_tex —
 * which is why the menu's 3D objects were missing entirely.
 *
 * The caller's ops are still queued in the CPU's write slot, so the GPU
 * thread first executes that slot's outstanding ops (partial flush) and then
 * reads the rect. s_exec_from[] remembers how far each slot has been run so
 * the ops are not replayed when the full frame is submitted — replaying a
 * semi-transparent draw would blend it twice. */
static uint32_t     s_exec_from[2];       /* first unexecuted op, per slot */
/* How many of each slot's vertices are already in the four VBOs. A slot's
 * vertex arrays are uploaded once per replay, from here to s_vtx[slot], instead
 * of once per batch; incremental because a synchronous readback replays the
 * write slot part-way through the frame and the rest follows at submit. Reset
 * with the slot. GPU thread only. */
static uint32_t     s_vtx_uploaded[2];
static SDL_Condition*    s_sync_rb_done;
static SDL_AtomicInt s_sync_rb_pending;    /* CPU sets 1; GPU clears when done */
static int          s_sync_rb_slot;
static uint16_t     s_sync_rb_x, s_sync_rb_y, s_sync_rb_w, s_sync_rb_h;
static uint16_t*    s_sync_rb_dst;        /* full 1024x512 CPU VRAM, written in place */

// --- Helper: Check for OpenGL Errors ---
void check_gl_error(const char* location) {
    GLenum error;
    while ((error = glGetError()) != GL_NO_ERROR) {
        const char* error_str;
        switch (error) {
            case GL_INVALID_ENUM: error_str = "INVALID_ENUM"; break;
            case GL_INVALID_VALUE: error_str = "INVALID_VALUE"; break;
            case GL_INVALID_OPERATION: error_str = "INVALID_OPERATION"; break;
            case GL_STACK_OVERFLOW: error_str = "STACK_OVERFLOW"; break;
            case GL_STACK_UNDERFLOW: error_str = "STACK_UNDERFLOW"; break;
            case GL_OUT_OF_MEMORY: error_str = "OUT_OF_MEMORY"; break;
            case GL_INVALID_FRAMEBUFFER_OPERATION: error_str = "INVALID_FRAMEBUFFER_OPERATION"; break;
            default: error_str = "UNKNOWN_ERROR"; break;
        }
        LOG_RENDERER_ERROR("[RENDERER] OpenGL Error at %s: %s (0x%04x)", location, error_str, error);
    }
}


// --- GLSL Shader Source ---
// Based on Guide Section 5.3 and 5.4

// Vertex Shader: Transforms PSX VRAM coordinates and colors to OpenGL format.
const char* vertex_shader_source =
    "#version 330 core\n"
    // Input attributes from VBOs (locations match glVertexAttribIPointer setup)
    "layout (location = 0) in ivec2 vertex_position; // PSX VRAM coords (int16)\n"
    "layout (location = 1) in uvec3 vertex_color;    // PSX BGR color (uint8)\n"
    "layout (location = 2) in ivec2 vertex_texcoord; // PSX VRAM TexCoords (int16)\n"
    "layout (location = 3) in uvec2 vertex_tpage;    // CLUT (x) and TPage (y) (uint16)\n"
    "\n"
    // Uniform: A single value passed to the shader for a batch of vertices
    "uniform ivec2 offset; // Drawing offset (applied to vertex_position)\n"
    "uniform vec2 screen_scale; // Half-width/height used for coordinate conversion\n"
    "\n"
    // Output: Color passed to the fragment shader (interpolated)
    "out vec3 color;\n"
    "out vec2 tex_coord;\n"
    "flat out uvec2 tpage_info; // Pass TPage info to fragment shader (no interpolation)\n"
    "\n"
    "void main() {\n"
    // Apply the drawing offset
    "    ivec2 p = vertex_position + offset;\n"
    "\n"
    // Convert X coordinate from PSX VRAM (0..1023) to OpenGL NDC (-1.0..+1.0)
    // screen_scale is (width/2, height/2)
    // xpos = (p.x / (width/2)) - 1.0 = (2*p.x / width) - 1.0
    // If p.x = 0, xpos = -1.0. If p.x = width, xpos = 1.0.
    "    float xpos = (float(p.x) / screen_scale.x) - 1.0;\n"
    "\n"
    // Y is NOT flipped: the render target is the unified VRAM texture, whose
    // texel row N must be PSX VRAM line N — the same row a CPU/MDEC upload
    // writes with glTexSubImage2D, and the same row the scanout pass reads.
    // (Rendering flipped while uploading unflipped is what made FMV frames and
    // rasterized output disagree about where a scanline lives.)
    // If p.y = 0, ypos = -1.0 (texel row 0). If p.y = height, ypos = +1.0.
    "    float ypos = (float(p.y) / screen_scale.y) - 1.0;\n"
    "\n"
    // Set the final position for this vertex. Z=0 (2D), W=1 (position).
    "    gl_Position = vec4(xpos, ypos, 0.0, 1.0);\n"
    "\n"
    // Convert color from 8-bit BGR to 32-bit float RGB [0.0..1.0]
    "    color = vec3(float(vertex_color.r) / 255.0,\n"
    "                   float(vertex_color.g) / 255.0,\n"
    "                   float(vertex_color.b) / 255.0);\n"
    "\n"
    // Pass texture coordinates directly (0..255)
    "    tex_coord = vec2(float(vertex_texcoord.x), float(vertex_texcoord.y));\n"
    "    tpage_info = vertex_tpage;\n"
    "}\n";

// Fragment Shader: Determines the final color of each pixel fragment.
// Uses usampler2D for R16UI integer texture - preserves exact 16-bit PSX pixel values
const char* fragment_shader_source =
    "#version 330 core\n"
    // Input: Color interpolated from the vertex shader outputs
    "in vec3 color;\n"
    "in vec2 tex_coord;\n"
    "flat in uvec2 tpage_info; // x=CLUT, y=TPage\n"
    "\n"
    /* VRAM read. Two sources, chosen at init and selected by a #define prepended
     * to this source (see build_fragment_shader):
     *
     *   UNIFIED_VRAM   sample vram_tex, the RGBA8 texture the rasterizer draws
     *                  into. It holds everything — CPU uploads *and* pixels drawn
     *                  by earlier primitives — so a game that renders into VRAM and
     *                  then textures from it reads what it actually wrote. Reading
     *                  the bound render target is a feedback loop, which is legal
     *                  only with glTextureBarrier between the draws that write and
     *                  the draws that read.
     *
     *   otherwise      sample the separate R16UI mirror, synced from gpu.vram.data.
     *                  That mirror never receives rasterized pixels, so render-to-
     *                  texture samples garbage — the BIOS memory-card menu draws its
     *                  labels and samples them straight back, and they came out as
     *                  speckle. Kept only for a driver without the barrier.
     */
    "#ifdef UNIFIED_VRAM\n"
    "uniform sampler2D vram_texture;\n"
    /* Recover the PS1 halfword from the 5:5:5:1-expanded RGBA8 texel. The
     * expansion ((v<<3)|(v>>2)) is exactly invertible, so this is lossless. */
    "uint to16(vec4 t){\n"
    "  uint r = uint(t.r*255.0+0.5)>>3;\n"
    "  uint g = uint(t.g*255.0+0.5)>>3;\n"
    "  uint b = uint(t.b*255.0+0.5)>>3;\n"
    "  uint a = (t.a > 0.5) ? 1u : 0u;\n"
    "  return r | (g<<5) | (b<<10) | (a<<15);\n"
    "}\n"
    "uint vram_read(ivec2 p){ return to16(texelFetch(vram_texture, p, 0)); }\n"
    "#else\n"
    "uniform usampler2D vram_texture;\n"
    "uint vram_read(ivec2 p){ return texelFetch(vram_texture, p, 0).r; }\n"
    "#endif\n"
    "uniform int u_mask_test;    // GP0(E6).1 — skip pixels whose destination bit 15 is set\n"
    "uniform int use_texture;\n"
    "uniform ivec4 u_texWindow; // (and_x, and_y, or_x, or_y) pre-computed masks\n"
    "uniform int raw_texture; // 1 = use texture color directly (no modulation)\n"
    "uniform int u_dither_enable; // 1 = apply PSX 4x4 dither before 15-bit quantization\n"
    "uniform int u_stp_mode;     // -1=off, 0=opaque pass (discard STP=1), 1=blend pass (discard STP=0)\n"
    "uniform int u_set_mask;     // GP0(E6).0 — force bit 15 set on every pixel drawn\n"
    "\n"
    // Output: Final color of the fragment (RGBA)
    "out vec4 frag_color;\n"
    "\n"
    // Helper: Convert PSX 1555 color to vec3
    "vec3 psx_to_rgb(uint raw) {\n"
    "    float r = float(raw & 0x1Fu) / 31.0;\n"
    "    float g = float((raw >> 5) & 0x1Fu) / 31.0;\n"
    "    float b = float((raw >> 10) & 0x1Fu) / 31.0;\n"
    "    return vec3(r, g, b);\n"
    "}\n"
    "\n"
    "void main() {\n"
    /* Alpha is not opacity here: the unified VRAM texture stores the PSX mask
     * bit (bit 15) in it, so a drawn pixel must carry the same bit a CPU write
     * would. Untextured primitives write 0 unless GP0(E6).0 forces it; textured
     * ones copy the source texel's bit 15 (the STP bit), OR'd with the force
     * flag — PSX-SPX "Mask bit". Writing a constant 1.0 here made every
     * rasterized pixel come back as 0x8000, which 24bpp scanout reads as
     * picture data (black areas turned green). */
    /* Mask-bit *test* — GP0(E6).1. Hardware skips a pixel whose destination
     * halfword already has bit 15 set, which is how games protect sprites they
     * have drawn from being overpainted. Reading the destination is the same
     * feedback loop the texture path needs, so it is only correct where the
     * barrier is: without UNIFIED_VRAM the mirror has no rasterized pixels to
     * test against and the check would pass everything, which is what the
     * renderer did before. */
    "#ifdef UNIFIED_VRAM\n"
    "    if (u_mask_test == 1) {\n"
    "        uint dst = vram_read(ivec2(gl_FragCoord.xy));\n"
    "        if ((dst & 0x8000u) != 0u) discard;\n"
    "    }\n"
    "#endif\n"
    "    float mask_a = (u_set_mask != 0) ? 1.0 : 0.0;\n"
    "    vec4 final_color = vec4(color, mask_a);\n"
    "    if (use_texture == 1) {\n"
    // "        frag_color = vec4(1.0, 0.0, 0.0, 1.0); return;\n" // DEBUG: Uncomment to verify geometry
    "        uint clut = tpage_info.x;\n"
    "        uint tpage = tpage_info.y;\n"
    "        uint depth = (tpage >> 7) & 3u;\n"
    "        uint page_x = (tpage & 0xFu) * 64u;\n"
    "        uint page_y = ((tpage >> 4) & 1u) * 256u;\n"
    "        uint clut_x = (clut & 0x3Fu) * 16u;\n"
    "        uint clut_y = (clut >> 6) & 0x1FFu;\n"
    "\n"
    "        // Apply Texture Window to UV coordinates (0-255).\n"
    "        //\n"
    "        // Converting through int, not uint, then clamping. GLSL leaves\n"
    "        // float->uint undefined for negative inputs (GLSL 3.30 section 5.4.1),\n"
    "        // and tex_coord is interpolated: a vertex UV of 0 can arrive at a\n"
    "        // fragment as a value a fraction below zero purely from interpolation\n"
    "        // rounding. NVIDIA saturates that to 0; Mesa wraps it to 0xFFFFFFFF,\n"
    "        // which the 0xFF mask turns into column 255 — the wrong end of the\n"
    "        // texture page. On a primitive whose UVs are all 0 that mis-samples\n"
    "        // the entire surface, which is why textures came out as flat blocks\n"
    "        // of one colour on the Intel iGPU and looked correct on the dGPU.\n"
    "        // float->int is defined for negatives, so clamp there instead.\n"
    "        uint u_raw = uint(clamp(int(tex_coord.x), 0, 255));\n"
    "        uint v_raw = uint(clamp(int(tex_coord.y), 0, 255));\n"
    "        uint u = (u_raw & uint(u_texWindow.x)) | uint(u_texWindow.z);\n"
    "        uint v = (v_raw & uint(u_texWindow.y)) | uint(u_texWindow.w);\n"
    "\n"
    "        vec3 tex_rgb = vec3(0.0);\n"
    "        uint raw_color = 0u;\n"
    "\n"
    "        if (depth == 0u) { // 4-bit paletted\n"
    "            uint tex_x = page_x + (u / 4u);\n"
    "            uint tex_y = page_y + v;\n"
    "            uint raw_word = vram_read(ivec2(tex_x, tex_y));\n"
    "            uint shift = (u & 3u) * 4u;\n"
    "            uint index = (raw_word >> shift) & 0xFu;\n"
    "            uint clut_pos_x = clut_x + index;\n"
    "            raw_color = vram_read(ivec2(clut_pos_x, clut_y));\n"
    "            if (raw_color == 0u) discard;\n"
    "            if (u_stp_mode == 0 && (raw_color & 0x8000u) != 0u) discard;\n"  /* pass1: discard STP=1 */
    "            if (u_stp_mode == 1 && (raw_color & 0x8000u) == 0u) discard;\n"  /* pass2: discard STP=0 */
    "            tex_rgb = psx_to_rgb(raw_color);\n"
    "\n"
    "        } else if (depth == 1u) { // 8-bit paletted\n"
    "            uint tex_x = page_x + (u / 2u);\n"
    "            uint tex_y = page_y + v;\n"
    "            uint raw_word = vram_read(ivec2(tex_x, tex_y));\n"
    "            uint shift = (u & 1u) * 8u;\n"
    "            uint index = (raw_word >> shift) & 0xFFu;\n"
    "            uint clut_pos_x = clut_x + index;\n"
    "            raw_color = vram_read(ivec2(clut_pos_x, clut_y));\n"
    "            if (raw_color == 0u) discard;\n"
    "            if (u_stp_mode == 0 && (raw_color & 0x8000u) != 0u) discard;\n"
    "            if (u_stp_mode == 1 && (raw_color & 0x8000u) == 0u) discard;\n"
    "            tex_rgb = psx_to_rgb(raw_color);\n"
    "\n"
    "        } else { // 15-bit direct color (depth == 2 or 3)\n"
    "            uint tex_x = page_x + u;\n"
    "            uint tex_y = page_y + v;\n"
    "            raw_color = vram_read(ivec2(tex_x, tex_y));\n"
    "            if (raw_color == 0u) discard;\n"
    "            if (u_stp_mode == 0 && (raw_color & 0x8000u) != 0u) discard;\n"
    "            if (u_stp_mode == 1 && (raw_color & 0x8000u) == 0u) discard;\n"
    "            tex_rgb = psx_to_rgb(raw_color);\n"
    "        }\n"
    "\n"
    "        if ((raw_color & 0x8000u) != 0u) mask_a = 1.0;\n"
    "        if (raw_texture == 1) {\n"
    "            final_color = vec4(tex_rgb, mask_a);\n"
    "        } else {\n"
    "            final_color = vec4(tex_rgb * color * 2.0, mask_a);\n"
    "        }\n"
    "    }\n"
    // PSX 4x4 dithering matrix (applied before 24-to-15bit quantization).
    // Per PSX-SPX: offsets added to 8-bit channel, result clamped [0,255], then >>3 to 5-bit.
    // Applied to: gouraud-shaded polygons, textured-blend polygons, all lines.
    // NOT applied to: mono polygons, raw-texture polygons, rectangles.
    "    if (u_dither_enable == 1) {\n"
    "        const int dither_table[16] = int[16](\n"
    "            -4,  0, -3,  1,\n"
    "             2, -2,  3, -1,\n"
    "            -3,  1, -4,  0,\n"
    "             3, -1,  2, -2\n"
    "        );\n"
    "        int dx = int(mod(gl_FragCoord.x, 4.0));\n"
    "        int dy = int(mod(gl_FragCoord.y, 4.0));\n"
    "        float doff = float(dither_table[dy * 4 + dx]) / 255.0;\n"
    "        // Clamp after adding dither offset, then quantize to 5-bit and normalize back\n"
    "        vec3 c_d = clamp(final_color.rgb + vec3(doff), 0.0, 1.0);\n"
    "        final_color.rgb = floor(c_d * 255.0 / 8.0) / 31.0;\n"
    "    }\n"
    "    frag_color = final_color;\n"
    "}\n";


// --- OpenGL Helper Functions ---

// Compiles a shader from source code.
// Based on Guide Section 5.5
static GLuint compile_shader(const char* source, GLenum shader_type) {
    GLuint shader = glCreateShader(shader_type);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);

    GLint status = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (status == GL_FALSE) {
        GLint log_len = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_len);
        char* log_buffer = (char*)malloc(log_len + 1);
        if (log_buffer) {
            glGetShaderInfoLog(shader, log_len, NULL, log_buffer);
            log_buffer[log_len] = '\0';
            LOG_RENDERER_ERROR("[RENDERER] Shader Compilation Error (%s):%s",
                (shader_type == GL_VERTEX_SHADER) ? "Vertex" : "Fragment",
                log_buffer);
            free(log_buffer);
        } else {
            LOG_RENDERER_ERROR("[RENDERER] Shader Compilation Error (%s) - Failed to allocate log buffer",
                (shader_type == GL_VERTEX_SHADER) ? "Vertex" : "Fragment");
        }
        glDeleteShader(shader); // Delete the failed shader object
        check_gl_error("compile_shader (error path)");
        return 0; // Return 0 on failure
    }
    LOG_RENDERER_DEBUG("[RENDERER] Shader compiled successfully (Type: %s)", (shader_type == GL_VERTEX_SHADER) ? "Vertex" : "Fragment");
    check_gl_error("compile_shader (success path)");
    return shader;
}

// Links vertex and fragment shaders into a shader program.
// Based on Guide Section 5.5
static GLuint link_program(GLuint vertex_shader, GLuint fragment_shader) {
    GLuint program = glCreateProgram();
    glAttachShader(program, vertex_shader);
    glAttachShader(program, fragment_shader);
    glLinkProgram(program);

    GLint status = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &status);
    if (status == GL_FALSE) {
        GLint log_len = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &log_len);
        char* log_buffer = (char*)malloc(log_len + 1);
        if (log_buffer) {
            glGetProgramInfoLog(program, log_len, NULL, log_buffer);
            log_buffer[log_len] = '\0';
            LOG_RENDERER_ERROR("[RENDERER] Shader Program Linking Error:%s", log_buffer);
            free(log_buffer);
        } else {
            LOG_RENDERER_ERROR("[RENDERER] Shader Program Linking Error - Failed to allocate log buffer");
        }
        glDeleteProgram(program); // Delete the failed program object
        // Shaders are still attached if linking failed, detach and delete them
        glDetachShader(program, vertex_shader);
        glDetachShader(program, fragment_shader);
        // Don't delete shaders here if they were passed in, caller might reuse
        check_gl_error("link_program (error path)");
        return 0; // Return 0 on failure
    }

    // Shaders can be detached and deleted after successful linking
    glDetachShader(program, vertex_shader);
    glDetachShader(program, fragment_shader);
    // Caller should delete the individual shaders if they are no longer needed
    // glDeleteShader(vertex_shader); // Optional: Delete here if not needed elsewhere
    // glDeleteShader(fragment_shader);

    LOG_RENDERER_DEBUG("[RENDERER] Shader program linked successfully (ID: %u)", program);
    check_gl_error("link_program (success path)");
    return program;
}


// --- GlRenderer Implementation ---

bool glr_init(GlRenderer* renderer) {
    if (log_get_level() >= LOG_LEVEL_INFO) {
        LOG_RENDERER_DEBUG("[RENDERER] Initializing renderer");
    }
    LOG_RENDERER_DEBUG("[RENDERER] Initializing GlRenderer...");
    renderer->initialized = false;
    renderer->vertex_count = 0;
    // Clear CPU-side buffers initially (optional but good practice)
    memset(renderer->positions_data, 0, sizeof(renderer->positions_data));
    memset(renderer->colors_data, 0, sizeof(renderer->colors_data));
    if (renderer->screen_width <= 0.0f) {
        renderer->screen_width = 1024.0f;
    }
    if (renderer->screen_height <= 0.0f) {
        renderer->screen_height = 512.0f;
    }


    /* Can this driver let a shader read the texture it is drawing into?
     *
     * Sampling the bound render target is a feedback loop, undefined in GL unless
     * glTextureBarrier() separates the write from the read. With it, texturing and
     * the mask test can use the one texture that holds everything — CPU uploads and
     * rasterized pixels alike — instead of a mirror that only ever sees the former.
     * Without it, the old mirror path stays, wrong in the same way it always was
     * but no worse.
     *
     * Core in GL 4.5 and present as ARB or NV on every driver this runs on, but
     * the emulator asks for a 3.3 core context, so it has to be asked for. */
    s_texture_barrier = (GLEW_ARB_texture_barrier || GLEW_NV_texture_barrier) &&
                        glTextureBarrier != NULL;
    LOG_RENDERER_INFO("[RENDERER] Unified VRAM sampling: %s",
                      s_texture_barrier
                        ? "on (texture barrier available — render-to-texture and the "
                          "mask-bit test are correct)"
                        : "OFF (no texture barrier — texturing falls back to the "
                          "upload-only mirror; pixels drawn by primitives will not "
                          "be visible to later texture reads)");

    /* One program either way: the source carries both paths and the #define picks
     * one, so there is no second pipeline to keep in step with the first. */
    const char* frag_sources[2] = {
        s_texture_barrier ? "#define UNIFIED_VRAM 1\n" : "",
        fragment_shader_source
    };
    char* frag_combined = NULL;
    {
        size_t n0 = strlen(frag_sources[0]), n1 = strlen(frag_sources[1]);
        frag_combined = (char*)malloc(n0 + n1 + 1);
        if (!frag_combined) {
            LOG_RENDERER_ERROR("[RENDERER] Out of memory assembling the fragment shader");
            return false;
        }
        /* The #version line must come first, so the define is spliced in after it
         * rather than pasted in front of the whole source. */
        const char* nl = strchr(frag_sources[1], '\n');
        size_t head = nl ? (size_t)(nl - frag_sources[1] + 1) : 0;
        memcpy(frag_combined, frag_sources[1], head);
        memcpy(frag_combined + head, frag_sources[0], n0);
        memcpy(frag_combined + head + n0, frag_sources[1] + head, n1 - head);
        frag_combined[n0 + n1] = '\0';
    }

    // Compile Shaders
    LOG_RENDERER_DEBUG("[RENDERER] Compiling vertex shader...");
    GLuint vs = compile_shader(vertex_shader_source, GL_VERTEX_SHADER);
    LOG_RENDERER_DEBUG("[RENDERER] Compiling fragment shader...");
    GLuint fs = compile_shader(frag_combined, GL_FRAGMENT_SHADER);
    free(frag_combined);
    if (vs == 0 || fs == 0) {
        LOG_RENDERER_ERROR("[RENDERER] GlRenderer Init Failed: Shader compilation error.");
        if (vs != 0) glDeleteShader(vs); // Clean up if one succeeded
        if (fs != 0) glDeleteShader(fs);
        return false;
    }

    // Link Program
    LOG_RENDERER_DEBUG("[RENDERER] Linking shader program...");
    renderer->shader_program = link_program(vs, fs);
    // Delete individual shaders now that they are linked into the program
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (renderer->shader_program == 0) {
        LOG_RENDERER_ERROR("[RENDERER] GlRenderer Init Failed: Shader linking error.");
        return false;
    }
    check_gl_error("After linking program");


    // Get Uniform Location for the drawing offset
    renderer->uniform_offset_loc = glGetUniformLocation(renderer->shader_program, "offset");
    if (renderer->uniform_offset_loc < 0) {
        // This isn't fatal, but offset won't work. Check for GL errors too.
        LOG_RENDERER_WARN("[RENDERER] Could not find uniform 'offset'. Draw offset will not work.");
        check_gl_error("glGetUniformLocation offset"); // Check if there was an error other than not found
    } else {
        LOG_RENDERER_DEBUG("[RENDERER] Found uniform 'offset' at location: %d", renderer->uniform_offset_loc);
        // Set initial offset to 0,0
        glUseProgram(renderer->shader_program); // Need to bind program to set uniform
        glUniform2i(renderer->uniform_offset_loc, 0, 0);
        glUseProgram(0); // Unbind program
    }
    check_gl_error("After getting/setting offset uniform");

    renderer->uniform_screen_scale_loc = glGetUniformLocation(renderer->shader_program, "screen_scale");
    if (renderer->uniform_screen_scale_loc < 0) {
        LOG_RENDERER_WARN("[RENDERER] Could not find uniform 'screen_scale'. Display scaling will be incorrect.");
    } else {
        LOG_RENDERER_DEBUG("[RENDERER] Found uniform 'screen_scale' at location: %d", renderer->uniform_screen_scale_loc);
        glUseProgram(renderer->shader_program);
        glUniform2f(renderer->uniform_screen_scale_loc,
                    renderer->screen_width * 0.5f,
                    renderer->screen_height * 0.5f);
        glUseProgram(0);
    }


    // --- Create Vertex Array Object (VAO) ---
    // VAO stores the links between VBOs and shader attributes.
    // Based on Guide Section 5.6
    LOG_RENDERER_DEBUG("[RENDERER] Creating VAO...");
    glGenVertexArrays(1, &renderer->vao);
    glBindVertexArray(renderer->vao); // Bind the VAO to make it active
    LOG_RENDERER_DEBUG("[RENDERER] VAO created (ID: %u) and bound.", renderer->vao);
    check_gl_error("After creating/binding VAO");


    // --- Create and Configure Position Vertex Buffer Object (VBO) ---
    LOG_RENDERER_DEBUG("[RENDERER] Creating Position VBO...");
    glGenBuffers(1, &renderer->position_buffer);
    glBindBuffer(GL_ARRAY_BUFFER, renderer->position_buffer); // Bind the new buffer to the GL_ARRAY_BUFFER target
    LOG_RENDERER_DEBUG("[RENDERER] Position VBO created (ID: %u) and bound.", renderer->position_buffer);

    // Allocate buffer storage on the GPU. We'll upload data later using glBufferSubData.
    // GL_DYNAMIC_DRAW is a hint that the data will be modified frequently.
    glBufferData(GL_ARRAY_BUFFER,               // Target buffer type
                 VERTEX_BUFFER_LEN * sizeof(RendererPosition), // Total buffer size in bytes
                 NULL,                         // Initial data (none)
                 GL_DYNAMIC_DRAW);             // Usage hint
    LOG_RENDERER_DEBUG("[RENDERER] Position VBO allocated %lu bytes.", VERTEX_BUFFER_LEN * sizeof(RendererPosition));
    check_gl_error("After position VBO glBufferData");

    // --- Link Position VBO to Shader Attribute ---
    // Get the location of the 'vertex_position' attribute in the shader (should be 0 as per layout qualifier)
    GLint pos_attrib_loc = glGetAttribLocation(renderer->shader_program, "vertex_position");
     if (pos_attrib_loc < 0) { LOG_RENDERER_WARN("[RENDERER] Could not find attribute 'vertex_position'."); }
     else { LOG_RENDERER_DEBUG("[RENDERER] Attribute 'vertex_position' found at location %d.", pos_attrib_loc); }

    // Enable this vertex attribute array
    glEnableVertexAttribArray(pos_attrib_loc); // Use the obtained location

    // Specify how OpenGL should interpret the data in the VBO for this attribute
    glVertexAttribIPointer(pos_attrib_loc,       // Attribute location in the shader
                           2,                  // Number of components per vertex (x, y)
                           GL_SHORT,           // Data type of each component (signed 16-bit int)
                           0, // Stride (0 = tightly packed) --> Or sizeof(RendererPosition)? Set 0 for now.
                           (void*)0);          // Offset of the first component in the buffer
    LOG_RENDERER_DEBUG("[RENDERER] Position VBO linked to vertex shader attribute location %d.", pos_attrib_loc);
    check_gl_error("After setting position attribute pointer");


    // --- Create and Configure Color Vertex Buffer Object (VBO) ---
    LOG_RENDERER_DEBUG("[RENDERER] Creating Color VBO...");
    glGenBuffers(1, &renderer->color_buffer);
    glBindBuffer(GL_ARRAY_BUFFER, renderer->color_buffer);
    LOG_RENDERER_DEBUG("[RENDERER] Color VBO created (ID: %u) and bound.", renderer->color_buffer);

    // Allocate storage
    glBufferData(GL_ARRAY_BUFFER, VERTEX_BUFFER_LEN * sizeof(RendererColor), NULL, GL_DYNAMIC_DRAW);
    LOG_RENDERER_DEBUG("[RENDERER] Color VBO allocated %lu bytes.", VERTEX_BUFFER_LEN * sizeof(RendererColor));
    check_gl_error("After color VBO glBufferData");

    // --- Link Color VBO to Shader Attribute ---
    GLint col_attrib_loc = glGetAttribLocation(renderer->shader_program, "vertex_color");
     if (col_attrib_loc < 0) { LOG_RENDERER_WARN("[RENDERER] Could not find attribute 'vertex_color'."); }
     else { LOG_RENDERER_DEBUG("[RENDERER] Attribute 'vertex_color' found at location %d.", col_attrib_loc); }

    glEnableVertexAttribArray(col_attrib_loc);

    // Specify data format for the color attribute
    glVertexAttribIPointer(col_attrib_loc,       // Attribute location
                           3,                  // Number of components (r, g, b)
                           GL_UNSIGNED_BYTE,   // Data type (unsigned 8-bit int)
                           0, // Stride (0 = tightly packed) --> Or sizeof(RendererColor)? Set 0 for now.
                           (void*)0);          // Offset
    LOG_RENDERER_DEBUG("[RENDERER] Color VBO linked to vertex shader attribute location %d.", col_attrib_loc);
    check_gl_error("After setting color attribute pointer");

    // --- Create and Configure Texture Coordinate VBO ---
    LOG_RENDERER_DEBUG("[RENDERER] Creating TexCoord VBO...");
    glGenBuffers(1, &renderer->texcoord_buffer);
    glBindBuffer(GL_ARRAY_BUFFER, renderer->texcoord_buffer);
    glBufferData(GL_ARRAY_BUFFER, VERTEX_BUFFER_LEN * sizeof(RendererTexCoord), NULL, GL_DYNAMIC_DRAW);
    LOG_RENDERER_DEBUG("[RENDERER] TexCoord VBO created (ID: %u) and bound.", renderer->texcoord_buffer);

    GLint tex_attrib_loc = glGetAttribLocation(renderer->shader_program, "vertex_texcoord");
    if (tex_attrib_loc >= 0) {
        glEnableVertexAttribArray(tex_attrib_loc);
        glVertexAttribIPointer(tex_attrib_loc, 2, GL_SHORT, 0, (void*)0);
        LOG_RENDERER_DEBUG("[RENDERER] Attribute 'vertex_texcoord' found at location %d.", tex_attrib_loc);
    } else {
        LOG_RENDERER_WARN("[RENDERER] Could not find attribute 'vertex_texcoord'.");
    }

    // --- Create and Configure TPage/CLUT VBO ---
    LOG_RENDERER_DEBUG("[RENDERER] Creating TPage VBO...");
    glGenBuffers(1, &renderer->tpage_buffer);
    glBindBuffer(GL_ARRAY_BUFFER, renderer->tpage_buffer);
    glBufferData(GL_ARRAY_BUFFER, VERTEX_BUFFER_LEN * sizeof(RendererTPage), NULL, GL_DYNAMIC_DRAW);
    LOG_RENDERER_DEBUG("[RENDERER] TPage VBO created (ID: %u) and bound.", renderer->tpage_buffer);

    GLint tpage_attrib_loc = glGetAttribLocation(renderer->shader_program, "vertex_tpage");
    if (tpage_attrib_loc >= 0) {
        glEnableVertexAttribArray(tpage_attrib_loc);
        glVertexAttribIPointer(tpage_attrib_loc, 2, GL_UNSIGNED_SHORT, 0, (void*)0);
        LOG_RENDERER_DEBUG("[RENDERER] Attribute 'vertex_tpage' found at location %d.", tpage_attrib_loc);
    } else {
        LOG_RENDERER_WARN("[RENDERER] Could not find attribute 'vertex_tpage'.");
    }

    // --- Create VRAM Texture ---
    // Use R16UI (16-bit unsigned integer) to preserve raw PSX pixel values exactly
    glGenTextures(1, &renderer->vram_texture);
    glBindTexture(GL_TEXTURE_2D, renderer->vram_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    // Allocate texture storage (1024x512, 16-bit unsigned integer)
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R16UI, 1024, 512, 0, GL_RED_INTEGER, GL_UNSIGNED_SHORT, NULL);
    glBindTexture(GL_TEXTURE_2D, 0);
    LOG_RENDERER_DEBUG("[RENDERER] VRAM Texture created (ID: %u) as R16UI.", renderer->vram_texture);

    // --- Unified VRAM texture: the FBO colour attachment (hardware-accelerated path) ---
    // Rasterization renders into this; CPU/MDEC uploads write into this; the
    // scanout pass reads this. One object, so uploaded content is displayable
    // by construction.
    glGenFramebuffers(1, &renderer->display_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, renderer->display_fbo);

    glGenTextures(1, &renderer->vram_tex);
    glBindTexture(GL_TEXTURE_2D, renderer->vram_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1024, 512, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, renderer->vram_tex, 0);
    renderer->display_texture = renderer->vram_tex;  /* legacy accessor alias */

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        LOG_RENDERER_ERROR("[RENDERER] GlRenderer Init Failed: VRAM FBO is not complete.");
        return false;
    }
    LOG_RENDERER_DEBUG("[RENDERER] Unified VRAM FBO created (FBO: %u, VRAM tex: %u).",
                       renderer->display_fbo, renderer->vram_tex);
    // Note: We leave display_fbo bound so all PSX rendering goes here!

    /* GL_DITHER is enabled by default, and it is not ours to want: the PSX's own
     * 4x4 dither is applied in the fragment shader before the 15-bit quantize,
     * so a second dither on the way to the render target is noise on top of a
     * signal that is already correct. It also diverges by driver — NVIDIA
     * generally ignores the state on 8-bit targets, Mesa honours it — which
     * makes the same frame come out differently on the iGPU and the dGPU. */
    glDisable(GL_DITHER);

    // --- VRAM viewer texture (RGBA8, produced by the viewer pass below) ---
    glGenTextures(1, &renderer->vram_viewer_texture);
    glBindTexture(GL_TEXTURE_2D, renderer->vram_viewer_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1024, 512, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);
    LOG_RENDERER_DEBUG("[RENDERER] VRAM viewer texture created (ID: %u).", renderer->vram_viewer_texture);

    // --- VRAM viewer pass ---
    // Decodes the whole unified VRAM into vram_viewer_texture, one RGBA8 texel
    // per VRAM halfword, so the viewer image is always 1024x512 and VRAM
    // coordinates map 1:1 regardless of the decode mode.
    //
    // This used to be a CPU loop over gpu.vram.data uploaded every frame. That
    // buffer is only the CPU-side model — it receives uploads, fills and DMA,
    // never the rasteriser — so every pixel a game draws with GP0 primitives was
    // absent and the display area came out black. Reading vram_tex here is the
    // whole point; it also drops a 2 MB upload per frame.
    {
        static const char* viewer_vs =
            "#version 330 core\n"
            "out vec2 v_uv;\n"
            "void main(){\n"
            "  vec2 p = vec2(float((gl_VertexID<<1)&2), float(gl_VertexID&2));\n"
            "  v_uv = p;\n"
            "  gl_Position = vec4(p*2.0-1.0, 0.0, 1.0);\n"
            "}\n";
        static const char* viewer_fs =
            "#version 330 core\n"
            "in vec2 v_uv;\n"
            "out vec4 o_col;\n"
            "uniform sampler2D u_vram;\n"
            "uniform int   u_mode;\n"     /* 0=4bpp 1=8bpp 2=16bpp 3=24bpp */
            "uniform ivec2 u_clut;\n"
            "uniform ivec3 u_flags;\n"    /* x=greyscale y=show_alpha z=shift24 */
            /* Same 5:5:5:1 recovery the scanout pass uses — bit 15 included,
             * because in 24bpp it is a data bit and in mask view it is the value
             * being looked at. */
            "uint to16(ivec2 p){\n"
            "  vec4 t = texelFetch(u_vram, ivec2(clamp(p.x,0,1023), clamp(p.y,0,511)), 0);\n"
            "  uint r = uint(t.r*255.0+0.5)>>3;\n"
            "  uint g = uint(t.g*255.0+0.5)>>3;\n"
            "  uint b = uint(t.b*255.0+0.5)>>3;\n"
            "  uint a = (t.a > 0.5) ? 1u : 0u;\n"
            "  return r | (g<<5) | (b<<10) | (a<<15);\n"
            "}\n"
            "vec3 expand(uint v){\n"
            "  return vec3(float((v      )&0x1Fu),\n"
            "              float((v >>  5)&0x1Fu),\n"
            "              float((v >> 10)&0x1Fu)) * (8.0/255.0);\n"
            "}\n"
            /* One VRAM halfword lives at linear index y*1024+x; the indexed modes
             * need arbitrary CLUT entries, so index arithmetic is done flat and
             * folded back to 2D. */
            "uint at(int idx){ return to16(ivec2(idx & 1023, (idx >> 10) & 511)); }\n"
            "void main(){\n"
            "  int x = int(v_uv.x * 1024.0);\n"
            "  int y = int(v_uv.y * 512.0);\n"
            "  int idx = y*1024 + x;\n"
            "  uint raw = at(idx);\n"
            "  vec3 c;\n"
            "  if (u_mode == 0) {\n"
            /* 4bpp: four indices per halfword, averaged into this slot so the
             * whole page stays visible at 1:1. */
            "    int base = u_clut.y*1024 + u_clut.x;\n"
            "    c = vec3(0.0);\n"
            "    for (int n = 0; n < 4; n++)\n"
            "      c += expand(at(base + int((raw >> uint(n*4)) & 0xFu)));\n"
            "    c *= 0.25;\n"
            "  } else if (u_mode == 1) {\n"
            "    int base = u_clut.y*1024 + u_clut.x;\n"
            "    vec3 e0 = expand(at(base + int(raw & 0xFFu)));\n"
            "    vec3 e1 = expand(at(base + int((raw >> 8) & 0xFFu)));\n"
            "    c = (e0 + e1) * 0.5;\n"
            "  } else if (u_mode == 3) {\n"
            /* 24bpp: three bytes per pixel straddling halfwords — read the byte
             * stream at this slot, offset by the phase. */
            "    int off = idx*2 + u_flags.z;\n"
            "    uint b0 = (at(off>>1) >> uint((off&1)*8)) & 0xFFu;\n"
            "    uint b1 = (at((off+1)>>1) >> uint(((off+1)&1)*8)) & 0xFFu;\n"
            "    uint b2 = (at((off+2)>>1) >> uint(((off+2)&1)*8)) & 0xFFu;\n"
            "    c = vec3(float(b0), float(b1), float(b2)) / 255.0;\n"
            "  } else {\n"
            "    c = expand(raw);\n"
            "  }\n"
            "  if (u_flags.y != 0) {\n"
            /* Mask bit only — makes write-protected pixels obvious. */
            "    float a = ((raw & 0x8000u) != 0u) ? 1.0 : 0.0;\n"
            "    c = vec3(a);\n"
            "  } else if (u_flags.x != 0) {\n"
            "    float l = dot(c, vec3(77.0, 150.0, 29.0) / 256.0);\n"
            "    c = vec3(l);\n"
            "  }\n"
            "  o_col = vec4(c, 1.0);\n"
            "}\n";
        GLuint vvs = compile_shader(viewer_vs, GL_VERTEX_SHADER);
        GLuint vfs = compile_shader(viewer_fs, GL_FRAGMENT_SHADER);
        renderer->viewer_program = link_program(vvs, vfs);
        glDeleteShader(vvs); glDeleteShader(vfs);
        renderer->viewer_vram_loc  = glGetUniformLocation(renderer->viewer_program, "u_vram");
        renderer->viewer_mode_loc  = glGetUniformLocation(renderer->viewer_program, "u_mode");
        renderer->viewer_clut_loc  = glGetUniformLocation(renderer->viewer_program, "u_clut");
        renderer->viewer_flags_loc = glGetUniformLocation(renderer->viewer_program, "u_flags");

        glGenFramebuffers(1, &renderer->viewer_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, renderer->viewer_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               renderer->vram_viewer_texture, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            LOG_RENDERER_ERROR("[RENDERER] VRAM viewer FBO incomplete");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    // --- Scanout-extract pass ---
    // Fullscreen triangle that reads the CRTC display window out of the unified
    // VRAM texture and unpacks it for the active depth (15/24bpp repacking).
    {
        static const char* scanout_vs =
            "#version 330 core\n"
            "out vec2 v_uv;\n"
            "void main(){\n"
            "  vec2 p = vec2(float((gl_VertexID<<1)&2), float(gl_VertexID&2));\n"
            "  v_uv = p;\n"
            "  gl_Position = vec4(p*2.0-1.0, 0.0, 1.0);\n"
            "}\n";
        static const char* scanout_fs =
            "#version 330 core\n"
            "in vec2 v_uv;\n"
            "out vec4 o_col;\n"
            "uniform sampler2D u_vram;\n"
            "uniform ivec2 u_disp_off;\n"    /* display start (VRAM halfword x, line y) */
            "uniform ivec2 u_disp_size;\n"   /* display size in output pixels */
            "uniform int   u_depth24;\n"
            /* Recover the raw PS1 halfword from the 5:5:5:1-expanded RGBA8 texel.
             * Bit 15 must come back too: in 24bpp it is a data bit of the packed
             * byte stream, not a mask flag, so dropping it corrupts every pixel
             * whose high byte is >= 0x80. */
            "uint to16(vec4 t){\n"
            "  uint r = uint(t.r*255.0+0.5)>>3;\n"
            "  uint g = uint(t.g*255.0+0.5)>>3;\n"
            "  uint b = uint(t.b*255.0+0.5)>>3;\n"
            "  uint a = (t.a > 0.5) ? 1u : 0u;\n"
            "  return r | (g<<5) | (b<<10) | (a<<15);\n"
            "}\n"
            "void main(){\n"
            "  int px = int(v_uv.x * float(u_disp_size.x));\n"
            "  int py = int(v_uv.y * float(u_disp_size.y));\n"
            "  int y  = u_disp_off.y + py;\n"
            "  if (u_depth24 == 1) {\n"
             /* 24bpp: 3 bytes per pixel spanning 1.5 halfwords — recombine two
              * texels and byte-shift on odd pixels. */
            "    int hx = u_disp_off.x + (px*3)/2;\n"
            "    uint s0 = to16(texelFetch(u_vram, ivec2(hx,   y), 0));\n"
            "    uint s1 = to16(texelFetch(u_vram, ivec2(hx+1, y), 0));\n"
            "    uint c  = ((s1<<16)|s0) >> (uint(px&1)*8u);\n"
            "    o_col = vec4(float(c&0xFFu), float((c>>8)&0xFFu), float((c>>16)&0xFFu), 255.0)/255.0;\n"
            "  } else {\n"
            /* 15bpp: the texel already holds the expanded colour. */
            "    o_col = vec4(texelFetch(u_vram, ivec2(u_disp_off.x+px, y), 0).rgb, 1.0);\n"
            "  }\n"
            "}\n";
        GLuint svs = compile_shader(scanout_vs, GL_VERTEX_SHADER);
        GLuint sfs = compile_shader(scanout_fs, GL_FRAGMENT_SHADER);
        renderer->scanout_program = link_program(svs, sfs);
        glDeleteShader(svs); glDeleteShader(sfs);
        renderer->scanout_vram_loc = glGetUniformLocation(renderer->scanout_program, "u_vram");
        renderer->scanout_off_loc  = glGetUniformLocation(renderer->scanout_program, "u_disp_off");
        renderer->scanout_size_loc = glGetUniformLocation(renderer->scanout_program, "u_disp_size");
        renderer->scanout_d24_loc  = glGetUniformLocation(renderer->scanout_program, "u_depth24");

        glGenTextures(1, &renderer->scanout_texture);
        glBindTexture(GL_TEXTURE_2D, renderer->scanout_texture);
        /* GL_RGBA8, not the unsized GL_RGB this used to ask for.
         *
         * An unsized internal format lets the driver pick whatever sized format
         * it likes, and GL 3.3 core only guarantees the *sized* formats are
         * colour-renderable — this texture is a framebuffer attachment. NVIDIA
         * resolves GL_RGB to RGBA8 and the picture came out right; Mesa is free
         * to choose something narrower, and a low-precision choice crushes every
         * gradient to the nearest primary. That is what the flat blocks of pure
         * blue, magenta, green and yellow on the Intel iGPU were: not mis-sampled
         * textures, but the finished picture quantized on its way into this
         * texture. Menus hid it because they are already saturated colour on
         * black; the 3D scene showed it immediately. */
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1024, 512, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glBindTexture(GL_TEXTURE_2D, 0);

        glGenFramebuffers(1, &renderer->scanout_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, renderer->scanout_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               renderer->scanout_texture, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            LOG_RENDERER_ERROR("[RENDERER] Scanout FBO incomplete");
        glBindFramebuffer(GL_FRAMEBUFFER, renderer->display_fbo);  /* restore */

        glGenVertexArrays(1, &renderer->dummy_vao);
        LOG_RENDERER_DEBUG("[RENDERER] Scanout pass ready (prog %u, tex %u).",
                           renderer->scanout_program, renderer->scanout_texture);
    }

    renderer->uniform_use_texture_loc = glGetUniformLocation(renderer->shader_program, "use_texture");
    renderer->uniform_raw_texture_loc = glGetUniformLocation(renderer->shader_program, "raw_texture");
    renderer->uniform_vram_texture_loc = glGetUniformLocation(renderer->shader_program, "vram_texture");
    renderer->uniform_tex_window_loc = glGetUniformLocation(renderer->shader_program, "u_texWindow");
    renderer->uniform_dither_loc = glGetUniformLocation(renderer->shader_program, "u_dither_enable");

    LOG_RENDERER_DEBUG("[RENDERER] Found uniform 'use_texture' at location: %d", renderer->uniform_use_texture_loc);
    LOG_RENDERER_DEBUG("[RENDERER] Found uniform 'raw_texture' at location: %d", renderer->uniform_raw_texture_loc);
    LOG_RENDERER_DEBUG("[RENDERER] Found uniform 'vram_texture' at location: %d", renderer->uniform_vram_texture_loc);
    LOG_RENDERER_DEBUG("[RENDERER] Found uniform 'u_texWindow' at location: %d", renderer->uniform_tex_window_loc);
    LOG_RENDERER_DEBUG("[RENDERER] Found uniform 'u_dither_enable' at location: %d", renderer->uniform_dither_loc);

    // Default texture window: no masking (and_x=0xFF, and_y=0xFF, or_x=0, or_y=0)
    glUseProgram(renderer->shader_program);
    if (renderer->uniform_tex_window_loc >= 0) glUniform4i(renderer->uniform_tex_window_loc, 0xFF, 0xFF, 0, 0);
    renderer->uniform_stp_mode_loc = glGetUniformLocation(renderer->shader_program, "u_stp_mode");
    if (renderer->uniform_stp_mode_loc >= 0) glUniform1i(renderer->uniform_stp_mode_loc, -1);
    renderer->uniform_set_mask_loc = glGetUniformLocation(renderer->shader_program, "u_set_mask");
    renderer->uniform_mask_test_loc = glGetUniformLocation(renderer->shader_program, "u_mask_test");
    if (renderer->uniform_mask_test_loc >= 0) glUniform1i(renderer->uniform_mask_test_loc, 0);
    if (renderer->uniform_set_mask_loc >= 0) glUniform1i(renderer->uniform_set_mask_loc, 0);
    if (renderer->uniform_raw_texture_loc >= 0) glUniform1i(renderer->uniform_raw_texture_loc, 0);
    if (renderer->uniform_dither_loc >= 0) glUniform1i(renderer->uniform_dither_loc, 0);
    glUseProgram(0);

    // --- Unbind ---
    glBindVertexArray(0); // Unbind the VAO
    glBindBuffer(GL_ARRAY_BUFFER, 0); // Unbind the VBO from the target
    LOG_RENDERER_DEBUG("[RENDERER] VAO and VBO unbound.");


    // --- Initial GL State ---
    // Set the default clear color to black
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    check_gl_error("After glClearColor");

    // Enable scissor test with default bounds covering entire VRAM (1024×512)
    // This ensures clipping is enabled even if GPU drawing area setup is called before renderer init
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, 1024, 512);
    LOG_RENDERER_DEBUG("[RENDERER] GL_SCISSOR_TEST enabled with default bounds (0,0,1024,512)");
    check_gl_error("After scissor initialization");

    // Potentially enable depth testing if needed later
    // glEnable(GL_DEPTH_TEST);

    renderer->initialized = true;
    renderer->texture_enabled = false;
    renderer->raw_texture_enabled = false;
    LOG_RENDERER_DEBUG("[RENDERER] GlRenderer Initialized Successfully.");
    return true;
}

// Buffers a triangle's vertex data
void glr_push_triangle(GlRenderer* renderer, RendererPosition pos[3], RendererColor col[3], RendererTexCoord tex[3], uint16_t clut, uint16_t tpage) {
    if (!renderer->initialized) {
        LOG_RENDERER_ERROR("[RENDERER] GlRenderer Error: push_triangle called before initialization.");
        return;
    }

    if (renderer->vertex_count + 3 > VERTEX_BUFFER_LEN) {
        LOG_RENDERER_DEBUG("[RENDERER] GlRenderer: Vertex buffer full (%u verts), forcing draw before push_triangle.", renderer->vertex_count);
        glr_draw(renderer);
        if (renderer->vertex_count + 3 > VERTEX_BUFFER_LEN) {
             LOG_RENDERER_ERROR("[RENDERER] GlRenderer Error: Cannot push triangle, buffer still full after draw.");
             return;
        }
    }

    // Copy data to CPU-side buffers
memcpy(&renderer->positions_data[renderer->vertex_count], pos, 3 * sizeof(RendererPosition));
    memcpy(&renderer->colors_data[renderer->vertex_count], col, 3 * sizeof(RendererColor));
    if (tex) {
        memcpy(&renderer->texcoords_data[renderer->vertex_count], tex, 3 * sizeof(RendererTexCoord));
        for(int i=0; i<3; ++i) {
            renderer->tpage_data[renderer->vertex_count + i].clut = clut;
            renderer->tpage_data[renderer->vertex_count + i].tpage = tpage;
        }
    } else {
        memset(&renderer->texcoords_data[renderer->vertex_count], 0, 3 * sizeof(RendererTexCoord));
        memset(&renderer->tpage_data[renderer->vertex_count], 0, 3 * sizeof(RendererTPage));
    }

    renderer->vertex_count += 3;
}

// Buffers a quad's vertex data (as two triangles)
void glr_push_quad(GlRenderer* renderer, RendererPosition pos[4], RendererColor col[4], RendererTexCoord tex[4], uint16_t clut, uint16_t tpage) {
     if (!renderer->initialized) {
        LOG_RENDERER_ERROR("[RENDERER] GlRenderer Error: push_quad called before initialization.");
        return;
     }

     if (renderer->vertex_count + 6 > VERTEX_BUFFER_LEN) {
        LOG_RENDERER_DEBUG("[RENDERER] GlRenderer Info: Vertex buffer full (%u verts), forcing draw before push_quad.", renderer->vertex_count);
        glr_draw(renderer);
        if (renderer->vertex_count + 6 > VERTEX_BUFFER_LEN) {
            LOG_RENDERER_ERROR("[RENDERER] GlRenderer Error: Cannot push quad, buffer still full after draw.");
            return;
        }
     }

    LOG_RENDERER_DEBUG("[RENDERER] GlRenderer: Buffering Quad (Start Index: %u)", renderer->vertex_count);
    // Decompose quad into two triangles
    // PSX Quad vertex order: 0--1
    //                        |  |
    //                        2--3
    // Triangle 1: V0, V1, V2
    renderer->positions_data[renderer->vertex_count + 0] = pos[0];
    renderer->colors_data[renderer->vertex_count + 0]    = col[0];
    renderer->positions_data[renderer->vertex_count + 1] = pos[1];
    renderer->colors_data[renderer->vertex_count + 1]    = col[1];
    renderer->positions_data[renderer->vertex_count + 2] = pos[2];
    renderer->colors_data[renderer->vertex_count + 2]    = col[2];

    if (tex) {
        renderer->texcoords_data[renderer->vertex_count + 0] = tex[0];
        renderer->texcoords_data[renderer->vertex_count + 1] = tex[1];
        renderer->texcoords_data[renderer->vertex_count + 2] = tex[2];
        for(int i=0; i<3; ++i) {
            renderer->tpage_data[renderer->vertex_count + i].clut = clut;
            renderer->tpage_data[renderer->vertex_count + i].tpage = tpage;
        }
    } else {
        memset(&renderer->texcoords_data[renderer->vertex_count], 0, 3 * sizeof(RendererTexCoord));
        memset(&renderer->tpage_data[renderer->vertex_count], 0, 3 * sizeof(RendererTPage));
    }

    // Triangle 2: V1, V2, V3
    renderer->positions_data[renderer->vertex_count + 3] = pos[1]; // V1
    renderer->colors_data[renderer->vertex_count + 3]    = col[1]; // C1
    renderer->positions_data[renderer->vertex_count + 4] = pos[2]; // V2
    renderer->colors_data[renderer->vertex_count + 4]    = col[2]; // C2
    renderer->positions_data[renderer->vertex_count + 5] = pos[3]; // V3
    renderer->colors_data[renderer->vertex_count + 5]    = col[3]; // C3

    if (tex) {
        renderer->texcoords_data[renderer->vertex_count + 3] = tex[1];
        renderer->texcoords_data[renderer->vertex_count + 4] = tex[2];
        renderer->texcoords_data[renderer->vertex_count + 5] = tex[3];
        for(int i=3; i<6; ++i) {
            renderer->tpage_data[renderer->vertex_count + i].clut = clut;
            renderer->tpage_data[renderer->vertex_count + i].tpage = tpage;
        }
    } else {
        memset(&renderer->texcoords_data[renderer->vertex_count + 3], 0, 3 * sizeof(RendererTexCoord));
        memset(&renderer->tpage_data[renderer->vertex_count + 3], 0, 3 * sizeof(RendererTPage));
    }

    renderer->vertex_count += 6;
}

void glr_set_texture_mode(GlRenderer* renderer, bool enabled) {
    if (renderer->texture_enabled != enabled) {
        glr_draw(renderer); // Flush current batch
        renderer->texture_enabled = enabled;
    }
}

void glr_set_raw_texture_mode(GlRenderer* renderer, bool enabled) {
    if (!renderer->initialized) return;
    if (renderer->raw_texture_enabled == enabled) return;
    glr_draw(renderer);
    renderer->raw_texture_enabled = enabled;
    /* GL uniform applied per-batch in glr_draw_gl() on GPU thread */
}

void glr_set_screen_scale(GlRenderer* renderer, uint16_t width, uint16_t height) {
    if (!renderer->initialized) {
        return;
    }
    if (renderer->uniform_screen_scale_loc < 0) {
        return;
    }

    if (width == 0) {
        width = 1024;
    }
    if (height == 0) {
        height = 512;
    }

    if (renderer->screen_width == (float)width && renderer->screen_height == (float)height) {
        return; // Nothing to update
    }

    glr_draw(renderer);
    renderer->screen_width  = (float)width;
    renderer->screen_height = (float)height;
    /* GL uniform applied per-batch in glr_draw_gl() on GPU thread */
}

void glr_set_texture_window(GlRenderer* renderer, uint8_t mask_x, uint8_t mask_y, uint8_t offset_x, uint8_t offset_y) {
    if (!renderer->initialized) return;

    // Calculate AND/OR masks from the GP0(0xE2) mask/offset registers:
    // Mask: 0=Don't mask, 1-31=Mask (size = 8, 16, 32... 256 pixels)
    // Offset: Base address of the window (in 8 pixel steps)
    
    // Formula:
    // AND = ~(mask * 8)
    // OR = (offset & mask) * 8
    
    uint32_t and_x = ~(mask_x * 8u) & 0xFF;
    uint32_t and_y = ~(mask_y * 8u) & 0xFF;
    uint32_t or_x = (offset_x & mask_x) * 8u;
    uint32_t or_y = (offset_y & mask_y) * 8u;

    /* Compare before flushing, like the mode setters: gpu_commands.c re-sends
     * E2 whenever the game does, and an unchanged value used to split the
     * batch anyway. */
    if (renderer->cached_tex_window[0] == (int32_t)and_x &&
        renderer->cached_tex_window[1] == (int32_t)and_y &&
        renderer->cached_tex_window[2] == (int32_t)or_x &&
        renderer->cached_tex_window[3] == (int32_t)or_y)
        return;

    glr_draw(renderer);
    /* Cache tex window — applied per-batch in glr_draw_gl() on GPU thread */
    renderer->cached_tex_window[0] = (int32_t)and_x;
    renderer->cached_tex_window[1] = (int32_t)and_y;
    renderer->cached_tex_window[2] = (int32_t)or_x;
    renderer->cached_tex_window[3] = (int32_t)or_y;
}

/* -------------------------------------------------------------------------
 * glr_record_vram_update — CPU thread: copy VRAM rect into pool and
 * record a GpuVramUpdate command for the GPU thread to execute.
 * ------------------------------------------------------------------------- */
static void glr_record_vram_update(GlRenderer* renderer, const uint16_t* vram_data,
                                         uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                         bool update_display) {
    if (!renderer->initialized || w == 0 || h == 0) return;
    int wi = renderer->write_idx;
    GpuFrame* frame = &s_frame[wi];

    if (frame->vram_update_count >= GPU_MAX_VRAM_UPDATES) {
        LOG_RENDERER_WARN("[RENDERER] VRAM update overflow — skipping %u×%u rect", w, h);
        return;
    }

    /* Copy only the rect rows (R16UI, row-stride = 1024 halfwords) */
    uint32_t bytes_needed = (uint32_t)w * h * sizeof(uint16_t);
    uint32_t aligned = (bytes_needed + 3u) & ~3u;
    if (s_vram_pool_used[wi] + aligned > GPU_VRAM_POOL_SIZE) {
        s_vram_pool_skips++;
        LOG_RENDERER_WARN("[RENDERER] VRAM pool full — skipping %ux%u rect (%u KB needed, %u KB used)",
                          w, h, aligned >> 10, s_vram_pool_used[wi] >> 10);
        return;
    }

    uint8_t* dst = s_vram_pool[wi] + s_vram_pool_used[wi];
    for (uint16_t row = 0; row < h; row++) {
        const uint16_t* src_row = &vram_data[((uint32_t)(y + row)) * 1024u + x];
        memcpy(dst + (uint32_t)row * w * 2u, src_row, w * sizeof(uint16_t));
    }

    uint32_t idx = frame->vram_update_count++;
    GpuVramUpdate* u = &frame->vram_updates[idx];
    u->x              = x;
    u->y              = y;
    u->w              = w;
    u->h              = h;
    u->data_offset    = s_vram_pool_used[wi];
    u->update_display = update_display;
    u->full_upload    = false;
    /* Every field has to be written here: the GpuVramUpdate array is reused
     * frame after frame, so an entry whose index once held the VRAM-viewer
     * snapshot would still say is_viewer, and the GPU thread would push this
     * rect into the debug viewer texture instead of VRAM — the column never
     * reaches vram_tex and the display keeps showing the previous frame there
     * (FMV playback showed a fixed set of stale vertical bars). */
    u->is_viewer      = false;
    s_vram_pool_used[wi] += aligned;
    if (s_vram_pool_used[wi] > s_vram_pool_peak) s_vram_pool_peak = s_vram_pool_used[wi];
    gpu_frame_record_op(frame, GPU_OP_VRAM_UPDATE, idx);
}

void glr_get_pool_stats(GlRenderer* renderer, uint32_t* used, uint32_t* peak,
                             uint32_t* updates, uint32_t* skips) {
    int wi = renderer->write_idx;
    if (used)    *used    = s_vram_pool_used[wi];
    if (peak)    *peak    = s_vram_pool_peak;
    if (updates) *updates = s_frame[wi].vram_update_count;
    if (skips)   *skips   = s_vram_pool_skips;
}

void glr_upload_vram(GlRenderer* renderer, const uint16_t* vram_data) {
    if (!renderer->initialized) return;
    /* Notified first, unconditionally: scripts that count these keep counting
     * the same events whether or not anything is copied below. */
    lua_debug_notify("vram_full_upload");
    /* With the texture barrier nothing reads what this would write.
     *
     * A full upload is recorded with update_display=false, so all it ever
     * reaches is the R16UI mirror (vram_texture), never vram_tex. The mirror
     * has exactly one reader: glr_draw_gl() binds it for textured batches only
     * when s_texture_barrier is false. Scanout, the VRAM viewer pass, both
     * readback paths, the frame dump and the savestate upload all go through
     * vram_tex. So where the barrier exists (every driver this has run on)
     * this was a 2 MB copy into the pool on the emulation thread and a 2 MB
     * glTexSubImage2D on the GPU thread, once per field from main.c and again
     * after every fill followed by a textured primitive (gpu_commands.c
     * upload_vram_if_dirty), for no visible effect; and each one took 2 MB of
     * the 16 MB per-slot pool, so a fill-heavy field could run the pool out and
     * drop a real upload with "VRAM pool full". The Vulkan backend reached the
     * same conclusion and makes this call a no-op (vkr_upload_vram). Without
     * the barrier the path below is unchanged. */
    if (s_texture_barrier) return;
    glr_record_vram_update(renderer, vram_data, 0, 0, 1024, 512, false);
}

void glr_upload_vram_rect(GlRenderer* renderer, const uint16_t* vram_data,
                                uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    if (!renderer->initialized || w == 0 || h == 0) return;
    /* Anything already submitted has to take its place in the order before this
     * write does. Primitives sit in renderer->positions_data until something
     * calls glr_draw(), and only that call records their op — so a draw
     * issued BEFORE this upload would be flushed afterwards and replayed on top
     * of it. Dino Crisis (E) clears its back buffer with GP0(02) and then
     * uploads the picture into it in the same field: the clear landed after the
     * picture and the screen stayed black with the image sitting in VRAM.
     * The reference emulator flushes on the same four boundaries (FillVRAM,
     * UpdateVRAM, CopyVRAM, DownloadVRAMFromGPU). */
    glr_draw(renderer);
    /* Mirror the rect into display_texture too. On real hardware a CPU/DMA
     * write straight into the displayed VRAM area is immediately visible;
     * display_texture only ever carried GL-rasterized pixels, so anything a
     * game paints by uploading (FMV frames decoded by the MDEC, 2D backdrops)
     * never showed up at all. Only the uploaded rect is stamped, so this does
     * not clobber rasterized pixels outside it. */
    glr_record_vram_update(renderer, vram_data, x, y, w, h, true);
}

/* Semi-transparency blend setup for one of the four PSX modes. The alpha
 * channel is NOT blended: it carries the PSX mask bit, and hardware writes the
 * source pixel's mask bit as-is, whatever the colour blend does. */
static void apply_semi_trans_blend(uint8_t mode) {
    GLenum src = GL_ONE, dst = GL_ONE, eq = GL_FUNC_ADD;
    switch (mode) {
        case 0:  /* B/2 + F/2 */
            src = GL_CONSTANT_ALPHA; dst = GL_CONSTANT_ALPHA;
            glBlendColor(0.0f, 0.0f, 0.0f, 0.5f);
            break;
        case 1:  /* B + F */
            break;
        case 2:  /* B - F */
            eq = GL_FUNC_REVERSE_SUBTRACT;
            break;
        case 3:  /* B + F/4 */
            src = GL_CONSTANT_ALPHA; dst = GL_ONE;
            glBlendColor(0.0f, 0.0f, 0.0f, 0.25f);
            break;
    }
    glBlendEquationSeparate(eq, GL_FUNC_ADD);
    glBlendFuncSeparate(src, dst, GL_ONE, GL_ZERO);
}

/* -------------------------------------------------------------------------
 * Replay state, GPU thread only.
 *
 * One of these lives for the length of one replay (a frame, or the part of
 * the write slot a synchronous readback runs). It exists so a batch issues
 * only the GL calls that differ from the batch before it. The old per-batch
 * cost was about 25 calls: ten uniforms, scissor, program, VAO and texture
 * binds and unbinds, a glTextureBarrier, and four glBufferSubData at offset 0
 * that rewrote the start of the same four buffers the previous draw was still
 * reading, which a driver can only honour by stalling or by copying.
 *
 * Everything starts "unknown" at the top of a replay, because other code on
 * this context (ImGui, scanout, the viewer pass, VRAM updates) changes the
 * same bindings and blend state between replays.
 * ------------------------------------------------------------------------- */
typedef struct {
    bool    bound;          /* program, VAO and the VRAM texture are bound */
    bool    written;        /* vram_tex may hold rendered pixels that no barrier
                             * has made visible to texel fetches yet */
    int     blend_on;       /* -1 unknown, 0 off, 1 on */
    int     blend_mode;     /* semi-transparency mode last applied, -1 unknown */
    bool    uni_valid;      /* the cached uniforms below are what the program holds */
    int     stp_mode;
    int16_t offset_x, offset_y;
    float   screen_w, screen_h;
    int32_t tex_window[4];
    int     dither, set_mask, mask_test, use_texture, raw_texture;
    bool    scissor_valid;
    int32_t scissor[4];
} GlrReplay;

static void glr_replay_begin(GlrReplay* ctx) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->bound      = false;
    /* Conservative: whatever the previous replay drew has not been behind a
     * barrier as far as this one can tell, so the first batch that samples
     * VRAM gets one. */
    ctx->written    = true;
    ctx->blend_on   = -1;
    ctx->blend_mode = -1;
    ctx->uni_valid  = false;
    ctx->scissor_valid = false;
}

static void glr_replay_set_blend(GlrReplay* ctx, bool on, int mode) {
    if (ctx->blend_on != (on ? 1 : 0)) {
        if (on) glEnable(GL_BLEND); else glDisable(GL_BLEND);
        ctx->blend_on = on ? 1 : 0;
    }
    /* apply_semi_trans_blend() sets the equation, the function and (for modes
     * 0 and 3) the constant every time, so one mode value fully describes it. */
    if (on && ctx->blend_mode != mode) {
        apply_semi_trans_blend((uint8_t)mode);
        ctx->blend_mode = mode;
    }
}

static void glr_replay_set_stp(GlRenderer* renderer, GlrReplay* ctx, int stp) {
    if (renderer->uniform_stp_mode_loc < 0) return;
    if (ctx->uni_valid && ctx->stp_mode == stp) return;
    glUniform1i(renderer->uniform_stp_mode_loc, stp);
    ctx->stp_mode = stp;
}

/* -------------------------------------------------------------------------
 * glr_draw_gl — INTERNAL: called by GPU thread to execute one batch.
 * All GL calls are here; CPU thread never calls this directly. The slot's
 * vertices are already in the VBOs at their own indices (glr_replay_slot), so
 * the draw starts at b->vertex_start.
 * ------------------------------------------------------------------------- */
static void glr_draw_gl(GlRenderer* renderer, const GpuBatch* b, GlrReplay* ctx) {
    if (b->vertex_count == 0) return;

    if (!ctx->bound) {
        glUseProgram(renderer->shader_program);
        glBindVertexArray(renderer->vao);
        /* Bind whichever VRAM the shader was built to read. With the barrier the
         * sampled texture *is* the render target; bound for every batch, since
         * an untextured batch still samples through this unit when the mask
         * test is on. Without it, the R16UI mirror; an untextured batch does
         * not sample (use_texture is 0 and the mask test is compiled out), so
         * leaving it bound for those is harmless. */
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, s_texture_barrier ? renderer->vram_tex : renderer->vram_texture);
        glEnable(GL_SCISSOR_TEST);
        ctx->bound = true;
    }

    /* Make pixels drawn by earlier batches visible to this one, when this one
     * reads VRAM and something has been drawn since the last barrier.
     *
     * With the barrier the sampled texture is the render target, a feedback
     * loop that is defined only with glTextureBarrier() between the draw that
     * writes and the draw that reads. The batches that read are the textured
     * ones and the ones with the mask test on (it reads the destination); a
     * batch that only writes needs nothing, since writes and blending are
     * ordered by the pipeline. One barrier before the first reading batch after
     * any write is exactly the guarantee the old one-per-batch barrier gave,
     * without paying for it on every untextured flat polygon. VRAM update ops
     * count as writes (glr_replay_slot), conservatively. */
    if (s_texture_barrier && (b->texture_enabled || b->mask_test_enabled) && ctx->written) {
        glTextureBarrier();
        ctx->written = false;
    }

    /* Uniforms: only the ones that changed since the previous batch. */
    if (!ctx->uni_valid || ctx->offset_x != b->offset_x || ctx->offset_y != b->offset_y) {
        glUniform2i(renderer->uniform_offset_loc, b->offset_x, b->offset_y);
        ctx->offset_x = b->offset_x; ctx->offset_y = b->offset_y;
    }
    if (renderer->uniform_screen_scale_loc >= 0 &&
        (!ctx->uni_valid || ctx->screen_w != b->screen_w || ctx->screen_h != b->screen_h)) {
        glUniform2f(renderer->uniform_screen_scale_loc, b->screen_w * 0.5f, b->screen_h * 0.5f);
        ctx->screen_w = b->screen_w; ctx->screen_h = b->screen_h;
    }
    if (renderer->uniform_tex_window_loc >= 0 &&
        (!ctx->uni_valid || memcmp(ctx->tex_window, b->tex_window, sizeof(ctx->tex_window)) != 0)) {
        glUniform4i(renderer->uniform_tex_window_loc,
                    b->tex_window[0], b->tex_window[1], b->tex_window[2], b->tex_window[3]);
        memcpy(ctx->tex_window, b->tex_window, sizeof(ctx->tex_window));
    }
    const int dither = b->dither_enabled ? 1 : 0, set_mask = b->set_mask_enabled ? 1 : 0;
    const int mask_test = b->mask_test_enabled ? 1 : 0, use_tex = b->texture_enabled ? 1 : 0;
    const int raw = b->raw_texture_enabled ? 1 : 0;
    if (renderer->uniform_dither_loc >= 0 && (!ctx->uni_valid || ctx->dither != dither))
        glUniform1i(renderer->uniform_dither_loc, dither);
    if (renderer->uniform_set_mask_loc >= 0 && (!ctx->uni_valid || ctx->set_mask != set_mask))
        glUniform1i(renderer->uniform_set_mask_loc, set_mask);
    if (renderer->uniform_mask_test_loc >= 0 && (!ctx->uni_valid || ctx->mask_test != mask_test))
        glUniform1i(renderer->uniform_mask_test_loc, mask_test);
    if (!ctx->uni_valid || ctx->use_texture != use_tex)
        glUniform1i(renderer->uniform_use_texture_loc, use_tex);
    if (renderer->uniform_raw_texture_loc >= 0 && (!ctx->uni_valid || ctx->raw_texture != raw))
        glUniform1i(renderer->uniform_raw_texture_loc, raw);
    if (!ctx->uni_valid)
        glUniform1i(renderer->uniform_vram_texture_loc, 0);
    ctx->dither = dither; ctx->set_mask = set_mask; ctx->mask_test = mask_test;
    ctx->use_texture = use_tex; ctx->raw_texture = raw;
    if (!ctx->uni_valid) {
        /* u_stp_mode is not set yet in this replay: force the first set below. */
        ctx->stp_mode = 0x7FFFFFFF;
        ctx->uni_valid = true;
    }

    if (!ctx->scissor_valid || memcmp(ctx->scissor, b->scissor, sizeof(ctx->scissor)) != 0) {
        glScissor(b->scissor[0], b->scissor[1], b->scissor[2], b->scissor[3]);
        memcpy(ctx->scissor, b->scissor, sizeof(ctx->scissor));
        ctx->scissor_valid = true;
    }

    const GLenum  prim  = b->is_lines ? GL_LINES : GL_TRIANGLES;
    const GLint   first = (GLint)b->vertex_start;
    const GLsizei count = (GLsizei)b->vertex_count;

    if (!b->is_lines && b->semi_trans_enabled && b->texture_enabled) {
        /* Two passes: blending is per draw but the STP bit is per texel, so the
         * opaque texels go down with blending off and the rest with it on. */
        glr_replay_set_blend(ctx, false, -1);
        glr_replay_set_stp(renderer, ctx, 0);
        glDrawArrays(prim, first, count);

        glr_replay_set_blend(ctx, true, b->semi_trans_mode);
        glr_replay_set_stp(renderer, ctx, 1);
        glDrawArrays(prim, first, count);
    } else if (!b->is_lines && b->semi_trans_enabled) {
        /* Flat/gouraud-shaded semi-transparent primitive: no per-texel STP bit
           to discard on (that only exists for textured sources): the whole
           primitive is uniformly semi-transparent, so a single blended pass
           is correct, unlike the textured two-pass case above. */
        glr_replay_set_stp(renderer, ctx, -1);
        glr_replay_set_blend(ctx, true, b->semi_trans_mode);
        glDrawArrays(prim, first, count);
    } else {
        glr_replay_set_stp(renderer, ctx, -1);
        glr_replay_set_blend(ctx, false, -1);
        glDrawArrays(prim, first, count);
    }
    ctx->written = true;
}

/* -------------------------------------------------------------------------
 * glr_draw — CPU thread: records a batch. No GL calls.
 * ------------------------------------------------------------------------- */
void glr_draw(GlRenderer* renderer) {
    if (!renderer->initialized) return;
    if (renderer->vertex_count == 0) return;

    int wi = renderer->write_idx;
    GpuFrame* frame = &s_frame[wi];

    if (frame->batch_count >= GPU_MAX_BATCHES) {
        LOG_RENDERER_WARN("[RENDERER] batch overflow — dropping %u vertices", renderer->vertex_count);
        renderer->vertex_count = 0;
        return;
    }

    uint32_t vtx_start = s_vtx[wi];
    uint32_t vtx_count = renderer->vertex_count;

    if (vtx_start + vtx_count > VERTEX_BUFFER_LEN) {
        LOG_RENDERER_WARN("[RENDERER] vertex pool overflow — dropping batch");
        renderer->vertex_count = 0;
        return;
    }

    frame_events_record(FEV_DRAW_BATCH, vtx_count);

    /* ZS1_GPU_TRACE_BATCH=1: one line per *distinct* combination of the state a
     * textured draw samples with. Frame captures are the wrong instrument for
     * "which page and depth is this primitive reading" — they show the result,
     * and only if the run happens to reach the frame. This shows the inputs, and
     * a seen-set keeps it to a handful of lines however long the run is. */
    {
        static int s_trace = -1;
        if (s_trace < 0) s_trace = getenv("ZS1_GPU_TRACE_BATCH") ? 1 : 0;
        if (s_trace && vtx_count > 0) {
            const RendererTPage* tp = &renderer->tpage_data[0];
            uint32_t key = ((uint32_t)tp->tpage << 16) | tp->clut;
            key = (key * 2u) | (renderer->texture_enabled ? 1u : 0u);
            static uint32_t seen[64];
            static int seen_n = 0;
            bool known = false;
            for (int i = 0; i < seen_n; i++) if (seen[i] == key) { known = true; break; }
            if (!known && seen_n < 64) {
                seen[seen_n++] = key;
                uint16_t t = tp->tpage;
                LOG_RENDERER_INFO("[GPU] batch: tex=%d tpage=0x%04x (pageX=%u pageY=%u "
                                  "semi=%u depth=%u) clut=0x%04x -> clutX=%u clutY=%u "
                                  "| raw=%d blend=%d semimode=%u",
                                  renderer->texture_enabled ? 1 : 0, t,
                                  t & 0xF, (t >> 4) & 1, (t >> 5) & 3, (t >> 7) & 3,
                                  tp->clut, (tp->clut & 0x3F) * 16, (tp->clut >> 6) & 0x1FF,
                                  renderer->raw_texture_enabled ? 1 : 0,
                                  renderer->semi_trans_enabled ? 1 : 0, renderer->semi_trans_mode);
            }
        }
    }

    /* Copy vertex data to pool */
    memcpy(&s_pos[wi][vtx_start], renderer->positions_data, vtx_count * sizeof(RendererPosition));
    memcpy(&s_col[wi][vtx_start], renderer->colors_data,    vtx_count * sizeof(RendererColor));
    memcpy(&s_tex[wi][vtx_start], renderer->texcoords_data, vtx_count * sizeof(RendererTexCoord));
    memcpy(&s_tpg[wi][vtx_start], renderer->tpage_data,     vtx_count * sizeof(RendererTPage));
    s_vtx[wi] += vtx_count;

    /* Record batch with full state snapshot */
    uint32_t batch_idx = frame->batch_count++;
    GpuBatch* b = &frame->batches[batch_idx];
    b->vertex_start       = vtx_start;
    b->vertex_count       = vtx_count;
    b->is_lines           = false;
    b->texture_enabled    = renderer->texture_enabled;
    b->raw_texture_enabled = renderer->raw_texture_enabled;
    b->semi_trans_enabled = renderer->semi_trans_enabled;
    b->semi_trans_mode    = renderer->semi_trans_mode;
    b->dither_enabled     = renderer->dither_enabled;
    b->set_mask_enabled   = renderer->set_mask_enabled;
    b->mask_test_enabled  = renderer->mask_test_enabled;
    b->screen_w           = renderer->screen_width  ? renderer->screen_width  : 1024.0f;
    b->screen_h           = renderer->screen_height ? renderer->screen_height : 512.0f;
    b->offset_x           = renderer->cached_offset_x;
    b->offset_y           = renderer->cached_offset_y;
    b->tex_window[0]      = renderer->cached_tex_window[0];
    b->tex_window[1]      = renderer->cached_tex_window[1];
    b->tex_window[2]      = renderer->cached_tex_window[2];
    b->tex_window[3]      = renderer->cached_tex_window[3];
    b->scissor[0]         = renderer->cached_scissor[0];
    b->scissor[1]         = renderer->cached_scissor[1];
    b->scissor[2]         = renderer->cached_scissor[2];
    b->scissor[3]         = renderer->cached_scissor[3];

    gpu_frame_record_op(frame, GPU_OP_BATCH, batch_idx);
    renderer->vertex_count = 0;
    LOG_RENDERER_DEBUG("[RENDERER] batch recorded: %u verts (slot %d, batch %u)",
                       vtx_count, wi, frame->batch_count - 1);
}

// Blits a portion of the VRAM texture to the screen as a full-screen quad
// Uses the existing shader infrastructure - draws VRAM content directly
void glr_blit_vram(GlRenderer* renderer, uint16_t vram_x, uint16_t vram_y, uint16_t width, uint16_t height) {
    if (!renderer->initialized) return;
    
    // First, flush any pending primitives
    glr_draw(renderer);
    // Diagnostic: log the region being blitted so we can confirm renderer sampling
    LOG_RENDERER_WARN("[RENDERER] VRAM blit region: x=%u y=%u w=%u h=%u", vram_x, vram_y, width, height);
    
    // Create positions for a screen-filling quad using VRAM coordinates
    // The vertex shader will convert these to NDC
    RendererPosition positions[4];
    RendererColor colors[4];
    RendererTexCoord texcoords[4];
    
    // Full VRAM in screen coordinates (0-1023, 0-511 maps to -1..1, 1..-1)
    // The texture coordinates select the actual display region within VRAM.
    // Top-left
    positions[0].x = 0;
    positions[0].y = 0;
    // Top-right
    positions[1].x = 1024;
    positions[1].y = 0;
    // Bottom-left
    positions[2].x = 0;
    positions[2].y = 512;
    // Bottom-right
    positions[3].x = 1024;
    positions[3].y = 512;
    
    // Neutral color (the shader multiplies by 2, so 128 = 1.0)
    for (int i = 0; i < 4; i++) {
        colors[i].r = 128;
        colors[i].g = 128;
        colors[i].b = 128;
    }
    
    // Texture coordinates map to the display region in VRAM
    texcoords[0].u = vram_x;
    texcoords[0].v = vram_y;
    texcoords[1].u = vram_x + width;
    texcoords[1].v = vram_y;
    texcoords[2].u = vram_x;
    texcoords[2].v = vram_y + height;
    texcoords[3].u = vram_x + width;
    texcoords[3].v = vram_y + height;
    
    // Build TPage for 15-bit direct texture mode (depth = 2)
    // Page base X = vram_x / 64, Page base Y = vram_y / 256, Depth = 2 (15-bit)
    uint16_t tpage = (vram_x / 64) | ((vram_y / 256) << 4) | (2 << 7);
    uint16_t clut = 0; // Not used for 15-bit mode
    
    /* Save offset, zero it for the blit quad, then restore */
    int16_t saved_ox = renderer->cached_offset_x;
    int16_t saved_oy = renderer->cached_offset_y;
    renderer->cached_offset_x = 0;
    renderer->cached_offset_y = 0;

    glr_set_texture_mode(renderer, true);
    glr_push_quad(renderer, positions, colors, texcoords, clut, tpage);
    glr_draw(renderer);

    renderer->cached_offset_x = saved_ox;
    renderer->cached_offset_y = saved_oy;
}

// Draws buffered primitives and requests buffer swap (swap happens in main loop)
void glr_display(GlRenderer* renderer) {
    if (!renderer->initialized) return;
    LOG_RENDERER_DEBUG("[RENDERER] GlRenderer: Display requested.");
    // Draw any remaining buffered vertices
    glr_draw(renderer);
    // Actual swap (SDL_GL_SwapWindow) happens in main.c/main loop
}

// Sets the drawing offset uniform. Forces a draw first.
// Based on Guide Section 5.10
void glr_set_draw_offset(GlRenderer* renderer, int16_t x, int16_t y) {
    if (!renderer->initialized) return;
    /* Unchanged (games re-send E5 with every primitive list): no flush. */
    if (renderer->cached_offset_x == x && renderer->cached_offset_y == y) return;
    LOG_RENDERER_DEBUG("[RENDERER] draw offset (%d, %d), flushing batch", x, y);
    glr_draw(renderer);
    renderer->cached_offset_x = x;
    renderer->cached_offset_y = y;
    /* GL uniform applied per-batch in glr_draw_gl() on GPU thread */
}

// ---------------------------------------------------------------------------
// glr_set_drawing_area — enable scissor clipping to PSX drawing area
// ---------------------------------------------------------------------------
void glr_set_drawing_area(GlRenderer* renderer, uint16_t left, uint16_t top,
                                uint16_t right, uint16_t bottom)
{
    if (!renderer->initialized) return;

    float sw = renderer->screen_width  ? renderer->screen_width  : 1024.0f;
    float sh = renderer->screen_height ? renderer->screen_height : 512.0f;
    float sx = 1024.0f / sw;
    float sy = 512.0f  / sh;

    int gl_left  = (int)((float)left  * sx);
    int gl_right = (int)((float)(right  + 1) * sx);
    int gl_top   = (int)((float)top   * sy);
    int gl_bot   = (int)((float)(bottom + 1) * sy);
    int clip_w   = gl_right - gl_left;
    int clip_h   = gl_bot   - gl_top;
    if (clip_w <= 0) clip_w = 1;
    if (clip_h <= 0) clip_h = 1;

    /* Scissor Y is a texel row, and the vertex shader no longer flips Y, so the
     * PSX top edge is the low row — no 512-gl_bot inversion. */
    int gl_y = gl_top;
    if (gl_y < 0) gl_y = 0;

    /* The scissor is computed first and compared, so an E3/E4 pair that names
     * the area already in force does not split the batch. */
    if (renderer->cached_scissor[0] == gl_left && renderer->cached_scissor[1] == gl_y &&
        renderer->cached_scissor[2] == clip_w  && renderer->cached_scissor[3] == clip_h)
        return;
    glr_draw(renderer);

    /* Cache scissor — applied per-batch on GPU thread */
    renderer->cached_scissor[0] = gl_left;
    renderer->cached_scissor[1] = gl_y;
    renderer->cached_scissor[2] = clip_w;
    renderer->cached_scissor[3] = clip_h;
}

// ---------------------------------------------------------------------------
// glr_set_dither_mode — enable/disable PSX 4x4 dithering in fragment shader
// ---------------------------------------------------------------------------
void glr_set_dither_mode(GlRenderer* renderer, bool enabled)
{
    if (!renderer->initialized) return;
    if (renderer->dither_enabled == enabled) return;

    glr_draw(renderer); // flush before changing dither state
    renderer->dither_enabled = enabled;
    // Uniform is set per-draw in glr_draw(); no immediate GL call needed.
}

// ---------------------------------------------------------------------------
// glr_set_mask_mode — GP0(0xE6).0, force the mask bit on drawn pixels.
// The unified texture keeps bit 15 in alpha, so this has to be batch state.
// ---------------------------------------------------------------------------
void glr_set_mask_mode(GlRenderer* renderer, bool enabled)
{
    if (!renderer->initialized) return;
    if (renderer->set_mask_enabled == enabled) return;

    glr_draw(renderer); // flush before changing mask state
    renderer->set_mask_enabled = enabled;
}

// ---------------------------------------------------------------------------
// glr_set_mask_test — GP0(0xE6).1, skip pixels whose destination has bit 15.
// The test reads the render target, so it is only honoured where the texture
// barrier is available; see the shader's UNIFIED_VRAM branch.
// ---------------------------------------------------------------------------
void glr_set_mask_test(GlRenderer* renderer, bool enabled)
{
    if (!renderer->initialized) return;
    if (renderer->mask_test_enabled == enabled) return;

    glr_draw(renderer); // flush before changing mask state
    renderer->mask_test_enabled = enabled;
}

// glr_set_semi_trans_mode — enable/disable GL blending for semi-trans
// ---------------------------------------------------------------------------
void glr_set_semi_trans_mode(GlRenderer* renderer, bool enabled, uint8_t mode)
{
    if (!renderer->initialized) return;
    if (renderer->semi_trans_enabled == enabled && renderer->semi_trans_mode == mode)
        return;

    glr_draw(renderer);
    renderer->semi_trans_enabled = enabled;
    renderer->semi_trans_mode    = mode;
    /* GL blend state applied per-batch in glr_draw_gl() on GPU thread */
}

// ---------------------------------------------------------------------------
// glr_push_line — CPU thread: record a 2-vertex line batch.
// ---------------------------------------------------------------------------
void glr_push_line(GlRenderer* renderer, RendererPosition pos[2], RendererColor col[2])
{
    if (!renderer->initialized) return;

    /* Flush any pending triangle batch first (different primitive type) */
    glr_draw(renderer);

    int wi = renderer->write_idx;
    GpuFrame* frame = &s_frame[wi];

    if (s_vtx[wi] + 2 > VERTEX_BUFFER_LEN) {
        LOG_RENDERER_WARN("[RENDERER] vertex pool overflow in push_line — skipping");
        return;
    }

    /* Consecutive lines under identical state extend the batch instead of
     * starting a new one. One batch per line is what made a wireframe frame
     * run into GPU_MAX_BATCHES and lose everything after it. */
    GpuBatch* prev = NULL;
    if (frame->op_count > 0 && frame->batch_count > 0) {
        const GpuOp* last = &frame->ops[frame->op_count - 1];
        if (last->type == GPU_OP_BATCH && last->index == frame->batch_count - 1) {
            GpuBatch* c = &frame->batches[last->index];
            /* Not a batch a synchronous readback has already run (s_exec_from
             * is past it): its vertex count was fixed when it was drawn, and
             * growing it now would add lines that are never drawn. The GPU
             * thread only moves s_exec_from while this thread is blocked in
             * glr_read_vram_rect(), so reading it here is safe. */
            if (frame->op_count > s_exec_from[wi]
                && c->is_lines && !c->texture_enabled && !c->semi_trans_enabled
                && c->vertex_start + c->vertex_count == s_vtx[wi]
                && c->dither_enabled    == renderer->dither_enabled
                && c->set_mask_enabled  == renderer->set_mask_enabled
                && c->mask_test_enabled == renderer->mask_test_enabled
                && c->offset_x == renderer->cached_offset_x
                && c->offset_y == renderer->cached_offset_y
                && memcmp(c->scissor, renderer->cached_scissor, sizeof(c->scissor)) == 0)
                prev = c;
        }
    }

    if (!prev && frame->batch_count >= GPU_MAX_BATCHES) {
        LOG_RENDERER_WARN("[RENDERER] batch overflow in push_line — skipping");
        return;
    }

    uint32_t vs = s_vtx[wi];
    RendererTexCoord zero_tc = {0, 0};
    RendererTPage    zero_tp = {0, 0};
    s_pos[wi][vs]     = pos[0]; s_pos[wi][vs+1] = pos[1];
    s_col[wi][vs]     = col[0]; s_col[wi][vs+1] = col[1];
    s_tex[wi][vs]     = zero_tc; s_tex[wi][vs+1] = zero_tc;
    s_tpg[wi][vs]     = zero_tp; s_tpg[wi][vs+1] = zero_tp;
    s_vtx[wi] += 2;

    if (prev) {
        prev->vertex_count += 2;
        return;
    }

    uint32_t line_batch_idx = frame->batch_count++;
    GpuBatch* b = &frame->batches[line_batch_idx];
    b->vertex_start       = vs;
    b->vertex_count       = 2;
    b->is_lines           = true;
    b->texture_enabled    = false;
    b->raw_texture_enabled = false;
    b->semi_trans_enabled = false;
    b->dither_enabled     = renderer->dither_enabled;
    b->set_mask_enabled   = renderer->set_mask_enabled;
    b->mask_test_enabled  = renderer->mask_test_enabled;
    b->semi_trans_mode    = 0;
    b->screen_w           = renderer->screen_width  ? renderer->screen_width  : 1024.0f;
    b->screen_h           = renderer->screen_height ? renderer->screen_height : 512.0f;
    b->offset_x           = renderer->cached_offset_x;
    b->offset_y           = renderer->cached_offset_y;
    b->tex_window[0]      = renderer->cached_tex_window[0];
    b->tex_window[1]      = renderer->cached_tex_window[1];
    b->tex_window[2]      = renderer->cached_tex_window[2];
    b->tex_window[3]      = renderer->cached_tex_window[3];
    b->scissor[0]         = renderer->cached_scissor[0];
    b->scissor[1]         = renderer->cached_scissor[1];
    b->scissor[2]         = renderer->cached_scissor[2];
    b->scissor[3]         = renderer->cached_scissor[3];

    gpu_frame_record_op(frame, GPU_OP_BATCH, line_batch_idx);
}

GLuint glr_get_display_texture(GlRenderer* renderer) {
    if (!renderer || !renderer->initialized) return 0;
    return renderer->display_texture;
}

GLuint glr_get_scanout_texture(GlRenderer* renderer) {
    if (!renderer || !renderer->initialized) return 0;
    return renderer->scanout_texture;
}

void glr_update_vram_viewer(GlRenderer* renderer, const uint8_t* vram_bytes) {
    if (!renderer->initialized) return;

    int wi = renderer->write_idx;
    GpuFrame* frame = &s_frame[wi];
    if (frame->vram_update_count >= GPU_MAX_VRAM_UPDATES) return;

    /* RGBA8: 4 bytes/pixel × 1024×512 = 2 MB — fits in our 2 MB pool */
    uint32_t bytes_needed = 1024u * 512u * 4u;
    if (s_vram_pool_used[wi] + bytes_needed > GPU_VRAM_POOL_SIZE) return;

    uint8_t* dst = s_vram_pool[wi] + s_vram_pool_used[wi];
    const uint16_t* src = (const uint16_t*)vram_bytes;
    const VramViewParams* vp = &renderer->vram_view;

    /* Every mode writes one RGBA8 texel per VRAM *halfword* slot, so the
     * viewer image always stays 1024x512 and VRAM coordinates map 1:1 to
     * texels regardless of the decode mode — the sub-modes just reinterpret
     * what each slot's bytes mean (PCSX-Redux does the same, in a shader). */
    for (uint32_t i = 0; i < 1024u * 512u; i++) {
        uint16_t raw = src[i];
        uint8_t r, g, b;

        switch (vp->mode) {
            case VRAM_VIEW_4BPP: {
                /* Four 4-bit indices per halfword; look each up in the CLUT
                 * row and average them into this slot's texel so the whole
                 * page stays visible at 1:1 scale. */
                uint32_t clut_base = (uint32_t)vp->clut_y * 1024u + vp->clut_x;
                uint32_t sr = 0, sg = 0, sb = 0;
                for (int n = 0; n < 4; n++) {
                    uint16_t entry = src[(clut_base + ((raw >> (n * 4)) & 0xFu)) & 0x7FFFFu];
                    sr += (uint32_t)((entry      ) & 0x1Fu) << 3;
                    sg += (uint32_t)((entry >>  5) & 0x1Fu) << 3;
                    sb += (uint32_t)((entry >> 10) & 0x1Fu) << 3;
                }
                r = (uint8_t)(sr / 4); g = (uint8_t)(sg / 4); b = (uint8_t)(sb / 4);
                break;
            }
            case VRAM_VIEW_8BPP: {
                uint32_t clut_base = (uint32_t)vp->clut_y * 1024u + vp->clut_x;
                uint16_t e0 = src[(clut_base + (raw & 0xFFu)) & 0x7FFFFu];
                uint16_t e1 = src[(clut_base + ((raw >> 8) & 0xFFu)) & 0x7FFFFu];
                r = (uint8_t)(((((e0      ) & 0x1Fu) + ((e1      ) & 0x1Fu)) << 3) / 2);
                g = (uint8_t)(((((e0 >>  5) & 0x1Fu) + ((e1 >>  5) & 0x1Fu)) << 3) / 2);
                b = (uint8_t)(((((e0 >> 10) & 0x1Fu) + ((e1 >> 10) & 0x1Fu)) << 3) / 2);
                break;
            }
            case VRAM_VIEW_24BPP: {
                /* 3 bytes per pixel straddling halfword boundaries: read the
                 * byte stream directly at this slot, offset by the phase. */
                const uint8_t* bytes = (const uint8_t*)src;
                uint32_t off = i * 2u + (uint32_t)vp->shift24;
                r = bytes[(off    ) & 0xFFFFFu];
                g = bytes[(off + 1) & 0xFFFFFu];
                b = bytes[(off + 2) & 0xFFFFFu];
                break;
            }
            case VRAM_VIEW_16BPP:
            default:
                r = (uint8_t)((raw & 0x1Fu) << 3);
                g = (uint8_t)(((raw >> 5) & 0x1Fu) << 3);
                b = (uint8_t)(((raw >> 10) & 0x1Fu) << 3);
                break;
        }

        if (vp->show_alpha) {
            /* Mask bit only — makes it obvious which pixels are write-protected. */
            uint8_t a = (raw & 0x8000u) ? 255 : 0;
            r = g = b = a;
        } else if (vp->greyscale) {
            uint8_t y = (uint8_t)(((uint32_t)r * 77u + (uint32_t)g * 150u + (uint32_t)b * 29u) >> 8);
            r = g = b = y;
        }

        *dst++ = r; *dst++ = g; *dst++ = b; *dst++ = 255;
    }

    uint32_t viewer_idx = frame->vram_update_count++;
    GpuVramUpdate* u = &frame->vram_updates[viewer_idx];
    u->x              = 0;
    u->y              = 0;
    u->w              = 1024;
    u->h              = 512;
    u->data_offset    = s_vram_pool_used[wi];
    u->update_display = false;
    u->full_upload    = false;
    u->is_viewer      = true;
    s_vram_pool_used[wi] += bytes_needed;
    gpu_frame_record_op(frame, GPU_OP_VRAM_UPDATE, viewer_idx);
}

GLuint glr_get_vram_viewer_texture(GlRenderer* renderer) {
    if (!renderer || !renderer->initialized) return 0;
    return renderer->vram_viewer_texture;
}

// Cleans up OpenGL resources
void glr_destroy(GlRenderer* renderer) {
    if (!renderer || !renderer->initialized) return;
    LOG_RENDERER_DEBUG("[RENDERER] Destroying GlRenderer...");

    /* Everything glr_init() created, in the order it created it.
     *
     * This used to free the main program, two of the four VBOs, the viewer
     * texture and the VAO, and leak the rest: the texcoord and tpage VBOs, all
     * three FBOs, vram_tex, vram_texture, scanout_texture, and the scanout and
     * viewer programs. At process exit that costs nothing, which is why it went
     * unnoticed. It stops being free the moment a backend can be torn down and
     * rebuilt inside a live process — a hot switch would leak the whole set on
     * every commutation. Names are zeroed after deletion so a re-init starts
     * from a struct that makes no claims about objects that no longer exist. */

    glDeleteProgram(renderer->shader_program);   check_gl_error("destroy - shader_program");
    glDeleteProgram(renderer->scanout_program);  check_gl_error("destroy - scanout_program");
    glDeleteProgram(renderer->viewer_program);   check_gl_error("destroy - viewer_program");
    renderer->shader_program = renderer->scanout_program = renderer->viewer_program = 0;

    glDeleteBuffers(1, &renderer->position_buffer);
    glDeleteBuffers(1, &renderer->color_buffer);
    glDeleteBuffers(1, &renderer->texcoord_buffer);
    glDeleteBuffers(1, &renderer->tpage_buffer);
    check_gl_error("destroy - vertex buffers");
    renderer->position_buffer = renderer->color_buffer = 0;
    renderer->texcoord_buffer = renderer->tpage_buffer = 0;

    /* An FBO must go before the texture it attaches, or the driver is asked to
     * keep a dangling attachment alive until the container is dropped. */
    glDeleteFramebuffers(1, &renderer->display_fbo);
    glDeleteFramebuffers(1, &renderer->scanout_fbo);
    glDeleteFramebuffers(1, &renderer->viewer_fbo);
    check_gl_error("destroy - framebuffers");
    renderer->display_fbo = renderer->scanout_fbo = renderer->viewer_fbo = 0;

    /* display_texture is an alias of vram_tex (glr_init sets them from the same
     * name), so it is cleared, never deleted — deleting it would be a double
     * free of the one VRAM texture. */
    glDeleteTextures(1, &renderer->vram_tex);
    glDeleteTextures(1, &renderer->vram_texture);
    glDeleteTextures(1, &renderer->scanout_texture);
    glDeleteTextures(1, &renderer->vram_viewer_texture);
    check_gl_error("destroy - textures");
    renderer->vram_tex = renderer->vram_texture = 0;
    renderer->scanout_texture = renderer->vram_viewer_texture = 0;
    renderer->display_texture = 0;

    glDeleteVertexArrays(1, &renderer->vao);
    glDeleteVertexArrays(1, &renderer->dummy_vao);
    check_gl_error("destroy - vertex arrays");
    renderer->vao = renderer->dummy_vao = 0;

    renderer->initialized = false;
    LOG_RENDERER_DEBUG("[RENDERER] GlRenderer Destroyed.");
}

/* =========================================================================
 * GPU Thread — execute VRAM updates then draw batches from a read slot.
 * All functions below are GPU-thread-only (GL context owned by GPU thread).
 * ========================================================================= */

/* Execute a single VRAM update. Called in submission order, interleaved with
 * draw batches, so a texture page reused later in the same frame is only
 * visible to draws that were actually issued after it (see GpuOp comment). */
static void glr_execute_one_vram_update(GlRenderer* renderer, const GpuVramUpdate* u, int slot) {
    const uint8_t* data = s_vram_pool[slot] + u->data_offset;

    if (u->is_viewer) {
        /* RGBA8 viewer upload */
        glBindTexture(GL_TEXTURE_2D, renderer->vram_viewer_texture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1024, 512,
                        GL_RGBA, GL_UNSIGNED_BYTE, data);
        glBindTexture(GL_TEXTURE_2D, 0);
    } else {
        /* R16UI VRAM texture upload: the sampling mirror. Only a driver without
         * the texture barrier samples it (see glr_upload_vram), so with the
         * barrier this copy fed nothing and is skipped. */
        if (!s_texture_barrier) {
            glBindTexture(GL_TEXTURE_2D, renderer->vram_texture);
            glTexSubImage2D(GL_TEXTURE_2D, 0, u->x, u->y, u->w, u->h,
                            GL_RED_INTEGER, GL_UNSIGNED_SHORT, data);
            glBindTexture(GL_TEXTURE_2D, 0);
        }

        /* Write real VRAM writes (GP0 A0 upload, GP0 80 copy, fill) into the
         * unified VRAM texture as 5:5:5:1-expanded RGBA8, so CPU/MDEC content
         * lands in the object the scanout reads. The expansion ((v<<3)|(v>>2))
         * is exactly invertible, so to16() recovers the halfword bit-for-bit.
         *
         * Guarded on update_display: the every-frame full-VRAM sync
         * (glr_upload_vram) exists only to refresh the R16UI sampling
         * mirror from gpu.vram.data, which does NOT contain GL-rasterized
         * pixels — blitting it into the unified texture would erase everything
         * rasterized that frame (black/flickering screen). */
        if (u->update_display) {
            static uint8_t rgba_buf[1024 * 512 * 4];
            const uint16_t* src = (const uint16_t*)data;
            uint8_t* dst = rgba_buf;
            for (uint32_t i = 0, n = (uint32_t)u->w * u->h; i < n; i++) {
                uint16_t raw = src[i];
                uint8_t r5 = (uint8_t)(raw & 0x1Fu);
                uint8_t g5 = (uint8_t)((raw >> 5) & 0x1Fu);
                uint8_t b5 = (uint8_t)((raw >> 10) & 0x1Fu);
                *dst++ = (uint8_t)((r5 << 3) | (r5 >> 2));
                *dst++ = (uint8_t)((g5 << 3) | (g5 >> 2));
                *dst++ = (uint8_t)((b5 << 3) | (b5 >> 2));
                *dst++ = (raw & 0x8000u) ? 255u : 0u;   /* mask bit */
            }
            /* vram_tex is the bound FBO's colour attachment; writing it with
             * glTexSubImage2D while that FBO is bound is undefined in GL, so
             * detach first (drivers silently drop the write otherwise). */
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glBindTexture(GL_TEXTURE_2D, renderer->vram_tex);
            glTexSubImage2D(GL_TEXTURE_2D, 0, u->x, u->y, u->w, u->h,
                            GL_RGBA, GL_UNSIGNED_BYTE, rgba_buf);
            glBindTexture(GL_TEXTURE_2D, 0);
            glBindFramebuffer(GL_FRAMEBUFFER, renderer->display_fbo);
        }
    }
}

/* ZS1_VSYNC=0|1|-1: the GL swap interval, set on the thread that owns the
 * context (it is per-context state, and this is where the context is current).
 *
 * Unset, nothing is called and the driver's default stands, which is exactly
 * what every run did before this existed: the tree never called
 * SDL_GL_SetSwapInterval at all. Set, it makes the vsync A/B repeatable
 * instead of depending on __GL_SYNC_TO_VBLANK / vblank_mode, which differ per
 * driver. 0 is off (the audio ring is the clock anyway, main.c pacing), 1 is
 * vsync, -1 is adaptive vsync (late frames tear instead of waiting a whole
 * refresh), falling back to 1 where the driver has no adaptive mode. */
static void glr_apply_vsync_preference(void) {
    const char* v = getenv("ZS1_VSYNC");
    if (!v) return;
    int want = atoi(v);
    if (want < -1 || want > 1) {
        LOG_RENDERER_WARN("[GPU-THREAD] ZS1_VSYNC=\"%s\" not recognised (want 0, 1 or -1); "
                          "leaving the driver default", v);
        return;
    }
    bool ok = SDL_GL_SetSwapInterval(want);
    if (!ok && want == -1) {
        LOG_RENDERER_WARN("[GPU-THREAD] Adaptive vsync refused (%s); using 1", SDL_GetError());
        want = 1;
        ok = SDL_GL_SetSwapInterval(want);
    }
    int got = 0;
    SDL_GL_GetSwapInterval(&got);
    if (ok) LOG_RENDERER_INFO("[GPU-THREAD] ZS1_VSYNC: swap interval %d (driver reports %d)", want, got);
    else    LOG_RENDERER_WARN("[GPU-THREAD] ZS1_VSYNC: SDL_GL_SetSwapInterval(%d) failed: %s",
                              want, SDL_GetError());
}

/* Put the slot's vertices that are not in the VBOs yet into them, at their
 * own indices, so every batch can draw straight out of [vertex_start, +count).
 * Starting a slot from zero orphans each buffer first (glBufferData with no
 * data): the driver hands back fresh storage instead of making this upload
 * wait for the previous frame's draws, which are still reading the old one. */
static void glr_upload_slot_vertices(GlRenderer* renderer, int slot) {
    const uint32_t from = s_vtx_uploaded[slot], to = s_vtx[slot];
    if (to <= from) return;
    const uint32_t n = to - from;
    struct { GLuint buf; size_t elem; const void* base; } a[4] = {
        { renderer->position_buffer, sizeof(RendererPosition), s_pos[slot] },
        { renderer->color_buffer,    sizeof(RendererColor),    s_col[slot] },
        { renderer->texcoord_buffer, sizeof(RendererTexCoord), s_tex[slot] },
        { renderer->tpage_buffer,    sizeof(RendererTPage),    s_tpg[slot] },
    };
    for (int i = 0; i < 4; i++) {
        glBindBuffer(GL_ARRAY_BUFFER, a[i].buf);
        if (from == 0)
            glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(VERTEX_BUFFER_LEN * a[i].elem), NULL, GL_DYNAMIC_DRAW);
        glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)(from * a[i].elem), (GLsizeiptr)(n * a[i].elem),
                        (const uint8_t*)a[i].base + from * a[i].elem);
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    s_vtx_uploaded[slot] = to;
}

/* Run a slot's outstanding ops into vram_tex, in submission order (a texture
 * page re-uploaded mid-frame must only be seen by the draws issued after it;
 * see GpuOp). Shared by the frame replay and the synchronous readback, which
 * runs the write slot part of the way; s_exec_from keeps either from running
 * an op twice. */
static void glr_replay_slot(GlRenderer* renderer, int slot) {
    /* Bind FBO for all draw commands; the viewport MUST match the FBO size, not the window. */
    glBindFramebuffer(GL_FRAMEBUFFER, renderer->display_fbo);
    glViewport(0, 0, 1024, 512);
    glr_upload_slot_vertices(renderer, slot);

    GlrReplay ctx;
    glr_replay_begin(&ctx);
    for (uint32_t i = s_exec_from[slot]; i < s_frame[slot].op_count; i++) {
        const GpuOp* op = &s_frame[slot].ops[i];
        if (op->type == GPU_OP_VRAM_UPDATE) {
            glr_execute_one_vram_update(renderer, &s_frame[slot].vram_updates[op->index], slot);
            /* It rebinds the texture unit (and the framebuffer) for its own
             * upload, and it writes VRAM: rebind before the next batch, and
             * treat it as a write for the barrier. */
            ctx.bound   = false;
            ctx.written = true;
        } else {
            glr_draw_gl(renderer, &s_frame[slot].batches[op->index], &ctx);
        }
    }
    s_exec_from[slot] = s_frame[slot].op_count;

    /* Leave the context as the old per-batch code did between batches. */
    glBindVertexArray(0);
    glUseProgram(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_BLEND);
}

/* GPU thread argument */
typedef struct {
    GlRenderer*     renderer;
    SDL_Window*   window;
    SDL_GLContext gl_context;
} GpuThreadArg;

static GpuThreadArg s_gpu_thread_arg;

/* GPU thread. Runs the write slot's outstanding ops so vram_tex holds every
 * pixel the caller has drawn so far, then copies the requested rect back into
 * the CPU-side VRAM. */
static void glr_service_sync_readback(GlRenderer* renderer, int wi) {
    glr_replay_slot(renderer, wi);
    glBindFramebuffer(GL_FRAMEBUFFER, renderer->display_fbo);

    /* Read only the rect. display_fbo has vram_tex as its colour attachment and
     * glReadPixels returns rows in the same order glTexSubImage2D uploads them,
     * so framebuffer row y is VRAM row y. Pulling the whole 2 MB texture back
     * instead cost ~2.3ms per call, and a burst of 50 in one second stalled the
     * emulator thread for 115ms — long enough to drain the audio ring.
     *
     * A rect that wraps the VRAM edges is read whole; that case is rare and not
     * worth a second code path. */
    bool wraps = (uint32_t)s_sync_rb_x + s_sync_rb_w > 1024u
              || (uint32_t)s_sync_rb_y + s_sync_rb_h > 512u;
    if (wraps) {
        glBindTexture(GL_TEXTURE_2D, renderer->vram_tex);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, s_readback_rgba);
        glBindTexture(GL_TEXTURE_2D, 0);
    } else {
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(s_sync_rb_x, s_sync_rb_y, s_sync_rb_w, s_sync_rb_h,
                     GL_RGBA, GL_UNSIGNED_BYTE, s_readback_rgba);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    for (uint16_t row = 0; row < s_sync_rb_h; row++) {
        uint32_t y = (uint32_t)((s_sync_rb_y + row) & 0x1FF);
        for (uint16_t col = 0; col < s_sync_rb_w; col++) {
            uint32_t x = (uint32_t)((s_sync_rb_x + col) & 0x3FF);
            const uint8_t* p = wraps ? &s_readback_rgba[(y * 1024u + x) * 4u]
                                     : &s_readback_rgba[((uint32_t)row * s_sync_rb_w + col) * 4u];
            s_sync_rb_dst[y * 1024u + x] = (uint16_t)(((uint16_t)(p[0] >> 3))
                                                    | ((uint16_t)(p[1] >> 3) << 5)
                                                    | ((uint16_t)(p[2] >> 3) << 10)
                                                    | (p[3] >= 128 ? 0x8000u : 0u));
        }
    }
}

static int gpu_thread_main(void* userdata) {
    GpuThreadArg* arg = (GpuThreadArg*)userdata;
    GlRenderer*   renderer = arg->renderer;
    SDL_Window* window   = arg->window;

    if (!SDL_GL_MakeCurrent(window, arg->gl_context)) {
        LOG_RENDERER_ERROR("[GPU-THREAD] SDL_GL_MakeCurrent failed: %s", SDL_GetError());
        return -1;
    }
    LOG_RENDERER_INFO("[GPU-THREAD] GPU thread started — GL context acquired");
    glr_apply_vsync_preference();

    /* ImGui OpenGL backend needs to be initialized on this thread */
    extern void imgui_opengl_new_frame(void);

    while (!SDL_GetAtomicInt(&renderer->gpu_stop)) {
        SDL_LockMutex(renderer->gpu_mutex);
        while (renderer->frames_pending == 0 && !SDL_GetAtomicInt(&s_sync_rb_pending)
               && !SDL_GetAtomicInt(&renderer->gpu_stop))
            SDL_WaitCondition(renderer->frame_ready, renderer->gpu_mutex);
        if (SDL_GetAtomicInt(&renderer->gpu_stop)) {
            SDL_UnlockMutex(renderer->gpu_mutex);
            break;
        }
        if (SDL_GetAtomicInt(&s_sync_rb_pending)) {
            int wi = s_sync_rb_slot;
            SDL_UnlockMutex(renderer->gpu_mutex);
            glr_service_sync_readback(renderer, wi);
            SDL_LockMutex(renderer->gpu_mutex);
            SDL_SetAtomicInt(&s_sync_rb_pending, 0);
            SDL_SignalCondition(s_sync_rb_done);
            SDL_UnlockMutex(renderer->gpu_mutex);
            continue;
        }
        int ri = 1 - renderer->write_idx; /* read slot = opposite of current write slot */
        SDL_UnlockMutex(renderer->gpu_mutex); /* frames_pending stays 1 until render+reset done */

        /* Replay VRAM updates and draw batches in original submission order,
         * from wherever a synchronous readback left the slot. */
        glr_replay_slot(renderer, ri);

        /* Serviced here: every op for this frame has run, so vram_tex holds
         * both the uploads and the rasterized pixels, and the scanout pass
         * below does not touch it. */
        glr_service_vram_readback(renderer);

        /* Scanout: extract the CRTC display window from the unified VRAM into
         * scanout_texture, unpacking for the active depth. This is the single
         * path by which anything reaches the screen. */
        {
            uint16_t dw = s_frame[ri].disp_w ? s_frame[ri].disp_w : 320;
            uint16_t dh = s_frame[ri].disp_h ? s_frame[ri].disp_h : 240;
            glBindFramebuffer(GL_FRAMEBUFFER, renderer->scanout_fbo);
            glViewport(0, 0, dw, dh);
            glDisable(GL_SCISSOR_TEST);
            glDisable(GL_BLEND);
            /* GP1(03).0 set: the video output is off, and DOCS/
             * graphicsprocessingunitgpu.md:647 says that shows "a black
             * picture". Games set it across a scene change while they rebuild
             * the framebuffer and reload CLUTs; scanning VRAM out anyway put
             * whatever the display window happened to cover on screen. */
            if (s_frame[ri].disp_blank) {
                glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);
            } else {
                glUseProgram(renderer->scanout_program);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, renderer->vram_tex);
                glUniform1i(renderer->scanout_vram_loc, 0);
                glUniform2i(renderer->scanout_off_loc,  (GLint)s_frame[ri].disp_x, (GLint)s_frame[ri].disp_y);
                glUniform2i(renderer->scanout_size_loc, (GLint)dw, (GLint)dh);
                glUniform1i(renderer->scanout_d24_loc,  s_frame[ri].disp_depth24 ? 1 : 0);
                glBindVertexArray(renderer->dummy_vao);
                glDrawArrays(GL_TRIANGLES, 0, 3);
                glBindVertexArray(0);
                glUseProgram(0);
                glBindTexture(GL_TEXTURE_2D, 0);
            }
        }

        /* VRAM viewer: decode the whole unified VRAM for the debug window. Runs
         * after scanout so it shows the same frame the screen shows, and reads
         * vram_tex — the CPU-side mirror never sees rasterised pixels. */
        /* Only while the viewer is on screen (view.enabled, set by debug_ui.cpp
         * with the frame's snapshot): it is a 1024x512 fullscreen pass, half a
         * million fragments a field, and it used to run in the gameplay shell
         * too, where nothing ever displays it. */
        if (renderer->viewer_program && s_frame[ri].view.enabled) {
            const VramViewParams* vv = &s_frame[ri].view;
            glBindFramebuffer(GL_FRAMEBUFFER, renderer->viewer_fbo);
            glViewport(0, 0, 1024, 512);
            glDisable(GL_SCISSOR_TEST);
            glDisable(GL_BLEND);
            glUseProgram(renderer->viewer_program);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, renderer->vram_tex);
            glUniform1i(renderer->viewer_vram_loc, 0);
            glUniform1i(renderer->viewer_mode_loc, (GLint)vv->mode);
            glUniform2i(renderer->viewer_clut_loc, (GLint)vv->clut_x, (GLint)vv->clut_y);
            glUniform3i(renderer->viewer_flags_loc,
                        vv->greyscale ? 1 : 0, vv->show_alpha ? 1 : 0, (GLint)vv->shift24);
            glBindVertexArray(renderer->dummy_vao);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glBindVertexArray(0);
            glUseProgram(0);
            glBindTexture(GL_TEXTURE_2D, 0);
        }

        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        {
            static int s_dump_counter = 0;
            const char* dump_path = getenv("ZS1_DUMP_FRAME");
            if (dump_path) {
                int target = 300;
                const char* target_env = getenv("ZS1_DUMP_FRAME_N");
                if (target_env) target = atoi(target_env);
                /* ZS1_DUMP_EVERY=1 turns the single shot into a repeating one that
                 * overwrites the same file, so whenever the run ends the file holds
                 * the most recent frame. Waiting for an exact frame number means
                 * losing the capture entirely if the run is shorter than expected,
                 * which is how several attempts at catching the BIOS menu were
                 * lost — the emulator reached it and exited before the count. */
                static int s_every = -1;
                if (s_every < 0) s_every = getenv("ZS1_DUMP_EVERY") ? 1 : 0;
                bool fire = s_every
                          ? (target > 0 && s_dump_counter > 0 && (s_dump_counter % target) == 0)
                          : (s_dump_counter == target);
                if (fire) {
                    /* Dump what the screen shows (scanout) by default; set
                     * ZS1_DUMP_VRAM=1 to dump the whole unified VRAM instead. */
                    GLuint dtex = getenv("ZS1_DUMP_VRAM") ? renderer->vram_tex
                                                          : renderer->scanout_texture;
                    unsigned char* buf = (unsigned char*)malloc(1024 * 512 * 3);
                    glBindTexture(GL_TEXTURE_2D, dtex);
                    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGB, GL_UNSIGNED_BYTE, buf);
                    glBindTexture(GL_TEXTURE_2D, 0);
                    FILE* f = fopen(dump_path, "wb");
                    if (f) { fwrite(buf, 1, 1024 * 512 * 3, f); fclose(f); }
                    free(buf);
                    LOG_RENDERER_INFO("[GPU-THREAD] Dumped frame %d to %s", s_dump_counter, dump_path);
                }
                s_dump_counter++;
            }
        }

        /* 3) Prepare default framebuffer for ImGui — display via draw_ps1_display() ImGui::Image */
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_BLEND);
        {
            int win_w, win_h;
            SDL_GetWindowSize(window, &win_w, &win_h);
            glViewport(0, 0, win_w, win_h);
        }
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        /* 4) ImGui: prepare GL backend for next frame, then render current frame */
        imgui_opengl_new_frame();
        if (s_frame[ri].imgui_draw_data) {
            extern void imgui_render_draw_data(void* draw_data);
            imgui_render_draw_data(s_frame[ri].imgui_draw_data);
        }

        SDL_GL_SwapWindow(window);

        /* Reset read slot for reuse */
        s_frame[ri].batch_count        = 0;
        s_frame[ri].vram_update_count  = 0;
        s_frame[ri].op_count           = 0;
        s_frame[ri].imgui_draw_data    = NULL;
        s_vtx[ri]                      = 0;
        s_vtx_uploaded[ri]             = 0;
        s_vram_pool_used[ri]           = 0;
        s_exec_from[ri]                = 0;

        /* Signal CPU that frame is done — set pending=0 here (after render+reset) */
        SDL_LockMutex(renderer->gpu_mutex);
        renderer->frames_pending = 0;
        SDL_SignalCondition(renderer->frame_done);
        SDL_UnlockMutex(renderer->gpu_mutex);
    }

    SDL_GL_MakeCurrent(window, NULL);
    LOG_RENDERER_INFO("[GPU-THREAD] GPU thread exiting");
    return 0;
}

/* -------------------------------------------------------------------------
 * Public GPU thread control API
 * ------------------------------------------------------------------------- */

void glr_start_gpu_thread(GlRenderer* renderer, SDL_Window* window, SDL_GLContext ctx) {
    renderer->gpu_mutex   = SDL_CreateMutex();
    renderer->frame_ready = SDL_CreateCondition();
    renderer->frame_done  = SDL_CreateCondition();
    renderer->sdl_window  = window;
    renderer->gl_context  = ctx;
    renderer->frames_pending = 0;
    SDL_SetAtomicInt(&renderer->gpu_stop, 0);

    /* Reset both slots */
    for (int i = 0; i < 2; i++) {
        s_frame[i].batch_count       = 0;
        s_frame[i].vram_update_count = 0;
        s_frame[i].op_count          = 0;
        s_frame[i].imgui_draw_data   = NULL;
        s_vtx[i]           = 0;
        s_vtx_uploaded[i]  = 0;
        s_vram_pool_used[i] = 0;
        s_exec_from[i]      = 0;
    }
    renderer->write_idx = 0;
    s_sync_rb_done = SDL_CreateCondition();
    SDL_SetAtomicInt(&s_sync_rb_pending, 0);

    s_gpu_thread_arg.renderer   = renderer;
    s_gpu_thread_arg.window     = window;
    s_gpu_thread_arg.gl_context = ctx;

    renderer->gpu_thread = SDL_CreateThread(gpu_thread_main, "GPU", &s_gpu_thread_arg);
    if (!renderer->gpu_thread)
        LOG_RENDERER_ERROR("[RENDERER] Failed to create GPU thread: %s", SDL_GetError());
    else
        LOG_RENDERER_INFO("[RENDERER] GPU thread started");
}

void glr_stop_gpu_thread(GlRenderer* renderer) {
    if (!renderer->gpu_thread) return;
    SDL_SetAtomicInt(&renderer->gpu_stop, 1);
    SDL_LockMutex(renderer->gpu_mutex);
    SDL_SignalCondition(renderer->frame_ready);
    SDL_UnlockMutex(renderer->gpu_mutex);
    SDL_WaitThread(renderer->gpu_thread, NULL);
    renderer->gpu_thread = NULL;
    SDL_DestroyMutex(renderer->gpu_mutex);
    SDL_DestroyCondition(renderer->frame_ready);
    SDL_DestroyCondition(renderer->frame_done);
    if (s_sync_rb_done) { SDL_DestroyCondition(s_sync_rb_done); s_sync_rb_done = NULL; }
    renderer->gpu_mutex   = NULL;
    renderer->frame_ready = NULL;
    renderer->frame_done  = NULL;
    LOG_RENDERER_INFO("[RENDERER] GPU thread stopped");
}

void glr_set_display_region(GlRenderer* renderer, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    renderer->display_x = x;
    renderer->display_y = y;
    renderer->display_w = w;
    renderer->display_h = h;
}

void glr_set_vram_view_params(GlRenderer* renderer, const VramViewParams* p) {
    if (p) renderer->vram_view = *p;
}

void glr_set_display_depth24(GlRenderer* renderer, bool depth24) {
    renderer->display_depth24 = depth24;
}

void glr_set_display_blank(GlRenderer* renderer, bool blank) {
    renderer->display_blank = blank;
}

/* Display state carried across submits — off by default, see below.
 *
 * A per-field latch was added here on the reasoning that hardware latches the
 * display registers at the start of the field, while we were building the frame
 * from renderer->display_* at submit time. That reasoning omitted *when* submit
 * happens: system_run_frame() returns the moment VBlank sets frame_complete
 * (system.c:64-71) and main.c submits immediately after, so submit already sits
 * on the field boundary and the live state at that instant IS the field's start
 * state. Latching on top of it hands the frame a value that is one whole field
 * older.
 *
 * That is visible on any double-buffered title, which is every FMV: the game
 * writes GP1(05) to the finished buffer in its VBlank handler and draws into the
 * other one for the rest of the field. The live value names the finished buffer;
 * the latched value names the buffer being drawn into right now, so each field
 * presents a half-written image and the picture shakes.
 *
 * The real defect the latch was aimed at — a game changing depth or window
 * part-way through a field, seen as the stretched 15bpp-read-as-24bpp frame
 * after an FMV (docs/GPU_DISPLAY_STUDY_2026-08-10.md §2.2) — needs the scanout
 * to run inside the field, per line, not a coarser whole-field delay.
 *
 * ZS1_DISPLAY_LATCH=1 restores the latched behaviour for A/B runs.
 *
 * File-static rather than GlRenderer fields so the struct — and with it the
 * savestate layout — does not move for a display latch. */
static uint16_t s_latch_x = 0, s_latch_y = 0, s_latch_w = 0, s_latch_h = 0;
static bool     s_latch_depth24 = false, s_latch_blank = false;
static bool     s_latch_valid = false;

static bool glr_display_latch(void) {
    static int cached = -1;
    if (cached < 0) {
        const char* v = getenv("ZS1_DISPLAY_LATCH");
        cached = (v && v[0] == '1') ? 1 : 0;
    }
    return cached != 0;
}

void glr_submit_frame(GlRenderer* renderer, void* imgui_draw_data) {
    if (!renderer->gpu_thread) return;

    /* Flush any leftover batch from CPU */
    glr_draw(renderer);

    SDL_LockMutex(renderer->gpu_mutex);
    /* Block if GPU is still rendering the previous frame */
    while (renderer->frames_pending > 0)
        SDL_WaitCondition(renderer->frame_done, renderer->gpu_mutex);

    const bool latch = glr_display_latch();

    /* First frame after start-up or a savestate load: nothing has been latched
     * yet, so the live state is the field's start state. */
    if (latch && !s_latch_valid) {
        s_latch_x = renderer->display_x;
        s_latch_y = renderer->display_y;
        s_latch_w = renderer->display_w;
        s_latch_h = renderer->display_h;
        s_latch_depth24 = renderer->display_depth24;
        s_latch_blank   = renderer->display_blank;
        s_latch_valid   = true;
    }

    GpuFrame* f = &s_frame[renderer->write_idx];
    f->imgui_draw_data = imgui_draw_data;
    f->disp_x = latch ? s_latch_x : renderer->display_x;
    f->disp_y = latch ? s_latch_y : renderer->display_y;
    f->disp_w = latch ? s_latch_w : renderer->display_w;
    f->disp_h = latch ? s_latch_h : renderer->display_h;
    f->disp_depth24 = latch ? s_latch_depth24 : renderer->display_depth24;
    f->disp_blank   = latch ? s_latch_blank   : renderer->display_blank;
    f->view = renderer->vram_view;   /* debug VRAM view: live, not part of the field */

    /* Carry the live state to the next submit, for the A/B path only. */
    s_latch_x = renderer->display_x;
    s_latch_y = renderer->display_y;
    s_latch_w = renderer->display_w;
    s_latch_h = renderer->display_h;
    s_latch_depth24 = renderer->display_depth24;
    s_latch_blank   = renderer->display_blank;
    renderer->write_idx    = 1 - renderer->write_idx;  /* swap */
    renderer->frames_pending = 1;
    SDL_SignalCondition(renderer->frame_ready);
    SDL_UnlockMutex(renderer->gpu_mutex);
}

bool glr_read_vram_rect(GlRenderer* renderer, uint16_t* vram,
                             uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    if (!renderer || !renderer->gpu_thread || !vram || !w || !h || !s_sync_rb_done)
        return false;

    /* Push the batch still open on the CPU side into the write slot, or the
     * primitive the caller just drew would not be in the rect it reads back. */
    glr_draw(renderer);

    SDL_LockMutex(renderer->gpu_mutex);
    while (renderer->frames_pending > 0)
        SDL_WaitCondition(renderer->frame_done, renderer->gpu_mutex);

    s_sync_rb_slot = renderer->write_idx;
    s_sync_rb_x = x; s_sync_rb_y = y; s_sync_rb_w = w; s_sync_rb_h = h;
    s_sync_rb_dst = vram;
    SDL_SetAtomicInt(&s_sync_rb_pending, 1);
    SDL_SignalCondition(renderer->frame_ready);
    while (SDL_GetAtomicInt(&s_sync_rb_pending))
        SDL_WaitCondition(s_sync_rb_done, renderer->gpu_mutex);
    SDL_UnlockMutex(renderer->gpu_mutex);
    return true;
}

void glr_wait_frame_done(GlRenderer* renderer) {
    if (!renderer->gpu_thread) return;
    SDL_LockMutex(renderer->gpu_mutex);
    while (renderer->frames_pending > 0)
        SDL_WaitCondition(renderer->frame_done, renderer->gpu_mutex);
    SDL_UnlockMutex(renderer->gpu_mutex);
}
/* -------------------------------------------------------------------------
 * The GfxBackend vtable
 *
 * Thin adapters, not casts: a function pointer called through an incompatible
 * type is undefined behaviour in C even when the layouts happen to agree, and
 * "happens to agree" is exactly the kind of thing that survives on one driver
 * and not the other. Each adapter costs one jump, on a path that carries a few
 * hundred primitives per field — measured against the ~80 textured quads a
 * field of Monsters & Co. draws, this is not on the hot path.
 *
 * There is one live GL backend, held in a file-static. The GPU command queue
 * above (s_frame, s_vram_pool, s_pos...) was already file-static, so a second
 * instance would already have been broken; this only makes that visible.
 * ------------------------------------------------------------------------- */

static GlRenderer s_gl_renderer;

/* A NULL impl means "before renderer_init()".
 *
 * gpu_reset_state() pushes the GP0/GP1 reset values through four setters while
 * the machine is being built, which is before main.c reaches renderer_init()
 * (gpu.c:661-668 against main.c:446 vs :448). Those values are not redundant —
 * glr_init() defaults screen_width/height and nothing else, so the drawing
 * area, texture window, display region and blank flag it leaves behind are the
 * ones the reset set. The state is a file-static that exists from program
 * start, so resolving NULL to it preserves the old behaviour exactly: back when
 * the struct was embedded in Gpu, those same writes landed in an equally
 * zeroed, equally not-yet-initialised struct. */
static inline GlRenderer* R(GfxImpl impl) {
    return impl ? (GlRenderer*)impl : &s_gl_renderer;
}

static bool vt_init(GfxImpl* impl, SDL_Window* window, const GfxDeviceRequest* req) {
    (void)window; (void)req;  /* the GL backend has no device or scale to choose: PRIME is
                 * resolved before the context exists, see apply_gpu_preference() */
    if (!glr_init(&s_gl_renderer)) return false;
    *impl = &s_gl_renderer;
    return true;
}

static void vt_destroy(GfxImpl impl)                 { glr_destroy(R(impl)); }

static int vt_enumerate_devices(GfxDeviceInfo* out, int max) {
    /* The GLX vendor library is resolved at the first dlopen of libGL, so
     * neither choice can be applied to a live process. Reported anyway, marked
     * not switchable: a control that says "next launch" is worth more than one
     * that silently does nothing. */
    static const char* names[2] = { "Discrete (PRIME offload)", "Integrated" };
    int n = 0;
    for (int i = 0; i < 2 && i < max; i++) {
        snprintf(out[i].name, sizeof(out[i].name), "%s", names[i]);
        out[i].live_switchable = false;
        n++;
    }
    return n;
}

static void vt_start_thread(GfxImpl impl, SDL_Window* window, void* native_ctx) {
    glr_start_gpu_thread(R(impl), window, (SDL_GLContext)native_ctx);
}
static void vt_stop_thread(GfxImpl impl)             { glr_stop_gpu_thread(R(impl)); }
static void vt_submit_frame(GfxImpl impl, void* dd)  { glr_submit_frame(R(impl), dd); }
static void vt_wait_frame_done(GfxImpl impl)         { glr_wait_frame_done(R(impl)); }

static void vt_push_triangle(GfxImpl impl, RendererPosition p[3], RendererColor c[3],
                             RendererTexCoord t[3], uint16_t clut, uint16_t tpage) {
    glr_push_triangle(R(impl), p, c, t, clut, tpage);
}
static void vt_push_quad(GfxImpl impl, RendererPosition p[4], RendererColor c[4],
                         RendererTexCoord t[4], uint16_t clut, uint16_t tpage) {
    glr_push_quad(R(impl), p, c, t, clut, tpage);
}
static void vt_push_line(GfxImpl impl, RendererPosition p[2], RendererColor c[2]) {
    glr_push_line(R(impl), p, c);
}
static void vt_draw(GfxImpl impl)                    { glr_draw(R(impl)); }
static void vt_display(GfxImpl impl)                 { glr_display(R(impl)); }
static void vt_blit_vram(GfxImpl impl, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    glr_blit_vram(R(impl), x, y, w, h);
}

static void vt_set_texture_mode(GfxImpl impl, bool e)      { glr_set_texture_mode(R(impl), e); }
static void vt_set_raw_texture_mode(GfxImpl impl, bool e)  { glr_set_raw_texture_mode(R(impl), e); }
static void vt_set_screen_scale(GfxImpl impl, uint16_t w, uint16_t h) {
    glr_set_screen_scale(R(impl), w, h);
}
static void vt_set_texture_window(GfxImpl impl, uint8_t mx, uint8_t my, uint8_t ox, uint8_t oy) {
    glr_set_texture_window(R(impl), mx, my, ox, oy);
}
static void vt_set_draw_offset(GfxImpl impl, int16_t x, int16_t y) {
    glr_set_draw_offset(R(impl), x, y);
}
static void vt_set_drawing_area(GfxImpl impl, uint16_t l, uint16_t t, uint16_t r, uint16_t b) {
    glr_set_drawing_area(R(impl), l, t, r, b);
}
static void vt_set_semi_trans_mode(GfxImpl impl, bool e, uint8_t m) {
    glr_set_semi_trans_mode(R(impl), e, m);
}
static void vt_set_dither_mode(GfxImpl impl, bool e)       { glr_set_dither_mode(R(impl), e); }
static void vt_set_mask_mode(GfxImpl impl, bool e)         { glr_set_mask_mode(R(impl), e); }
static void vt_set_mask_test(GfxImpl impl, bool e)         { glr_set_mask_test(R(impl), e); }

static void vt_upload_vram(GfxImpl impl, const uint16_t* d) { glr_upload_vram(R(impl), d); }
static void vt_upload_vram_rect(GfxImpl impl, const uint16_t* d,
                                uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    glr_upload_vram_rect(R(impl), d, x, y, w, h);
}
static bool vt_read_vram_rect(GfxImpl impl, uint16_t* out,
                              uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    return glr_read_vram_rect(R(impl), out, x, y, w, h);
}
static void vt_request_vram_readback(GfxImpl impl)   { glr_request_vram_readback(R(impl)); }

static void vt_set_display_region(GfxImpl impl, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    glr_set_display_region(R(impl), x, y, w, h);
}
static void vt_set_display_depth24(GfxImpl impl, bool d)  { glr_set_display_depth24(R(impl), d); }
static void vt_set_display_blank(GfxImpl impl, bool b)    { glr_set_display_blank(R(impl), b); }

static void vt_set_vram_view_params(GfxImpl impl, const VramViewParams* p) {
    glr_set_vram_view_params(R(impl), p);
}
static void vt_update_vram_viewer(GfxImpl impl, const uint8_t* b) {
    glr_update_vram_viewer(R(impl), b);
}
static void vt_get_pool_stats(GfxImpl impl, uint32_t* used, uint32_t* peak,
                              uint32_t* updates, uint32_t* skips) {
    glr_get_pool_stats(R(impl), used, peak, updates, skips);
}

/* The three handles ImGui draws with. A GLuint widens to uintptr_t here and is
 * narrowed back nowhere: debug_ui.cpp hands it to ImGui and never inspects it. */
static GfxTexHandle vt_get_display_texture(GfxImpl impl) {
    return (GfxTexHandle)glr_get_display_texture(R(impl));
}
static GfxTexHandle vt_get_scanout_texture(GfxImpl impl) {
    return (GfxTexHandle)glr_get_scanout_texture(R(impl));
}
static GfxTexHandle vt_get_vram_viewer_texture(GfxImpl impl) {
    return (GfxTexHandle)glr_get_vram_viewer_texture(R(impl));
}

const GfxBackend gfx_backend_gl33 = {
    .name                   = "OpenGL 3.3",
    .type                   = GFX_BACKEND_GL33,
    .init                   = vt_init,
    .destroy                = vt_destroy,
    .enumerate_devices      = vt_enumerate_devices,
    .start_thread           = vt_start_thread,
    .stop_thread            = vt_stop_thread,
    .submit_frame           = vt_submit_frame,
    .wait_frame_done        = vt_wait_frame_done,
    .push_triangle          = vt_push_triangle,
    .push_quad              = vt_push_quad,
    .push_line              = vt_push_line,
    .draw                   = vt_draw,
    .display                = vt_display,
    .blit_vram              = vt_blit_vram,
    .set_texture_mode       = vt_set_texture_mode,
    .set_raw_texture_mode   = vt_set_raw_texture_mode,
    .set_screen_scale       = vt_set_screen_scale,
    .set_texture_window     = vt_set_texture_window,
    .set_draw_offset        = vt_set_draw_offset,
    .set_drawing_area       = vt_set_drawing_area,
    .set_semi_trans_mode    = vt_set_semi_trans_mode,
    .set_dither_mode        = vt_set_dither_mode,
    .set_mask_mode          = vt_set_mask_mode,
    .set_mask_test          = vt_set_mask_test,
    .upload_vram            = vt_upload_vram,
    .upload_vram_rect       = vt_upload_vram_rect,
    .read_vram_rect         = vt_read_vram_rect,
    .request_vram_readback  = vt_request_vram_readback,
    .get_vram_readback      = glr_get_vram_readback,
    .set_display_region     = vt_set_display_region,
    .set_display_depth24    = vt_set_display_depth24,
    .set_display_blank      = vt_set_display_blank,
    .set_vram_view_params   = vt_set_vram_view_params,
    .update_vram_viewer     = vt_update_vram_viewer,
    .get_pool_stats         = vt_get_pool_stats,
    .get_display_texture    = vt_get_display_texture,
    .get_scanout_texture    = vt_get_scanout_texture,
    .get_vram_viewer_texture= vt_get_vram_viewer_texture,
};
