/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 ZioZoni95
 *
 * Part of ZoniStation One, a PlayStation 1 emulator.
 * See LICENSE for the full licence text and THIRD-PARTY.md for the
 * components of this project that have other authors.
 */
#ifndef RENDERER_GL_H
#define RENDERER_GL_H

/* Private header of the OpenGL 3.3 backend.
 *
 * This is the only place in the tree, alongside renderer_gl.c, that is allowed
 * to include <GL/glew.h>. It used to be include/renderer.h, which gpu.h
 * includes, so GLuint reached every translation unit that touched the
 * interconnect — debug_ui.cpp among them. Do not include this from anywhere
 * except the GL backend's own sources. */

#include "gpu_backend.h"

#define GLEW_STATIC
#include <GL/glew.h>

/* The OpenGL 3.3 backend's entire state. Private to renderer_gl.c:
 * nothing outside this backend may name a GL type. */
typedef struct {
    // OpenGL Object IDs
    GLuint vao;             // Vertex Array Object: Groups VBO bindings and attribute pointers
    GLuint position_buffer; // Vertex Buffer Object (VBO) storing vertex positions
    GLuint color_buffer;    // Vertex Buffer Object (VBO) storing vertex colors
    GLuint texcoord_buffer; // VBO for texture coordinates
    GLuint tpage_buffer;    // VBO for CLUT/TPage info
    GLuint shader_program;  // ID of the compiled and linked GLSL shader program
    GLuint vram_texture;    // Texture object for VRAM

    // --- Unified VRAM (single GL texture) ---
    // ONE RGBA8 texture is the rasterization target, the CPU/MDEC upload
    // target, and the scanout source, so anything written to VRAM is on screen
    // by construction. PSX 16-bit halfwords are stored 5:5:5:1 expanded to 8
    // bits per channel ((v<<3)|(v>>2)), which round-trips losslessly, so CLUT
    // index bits survive for texture sampling.
    GLuint display_fbo;     // FBO whose colour attachment is vram_tex
    GLuint vram_tex;        // RGBA8 1024x512 — the one VRAM
    GLuint display_texture; // legacy alias target (kept until Phase 2b)

    // Scanout-extract pass: renders the CRTC display window out of vram_tex,
    // unpacking per depth (15bpp direct / 24bpp packed triplets).
    GLuint scanout_fbo;
    GLuint scanout_texture;   // RGB8 display-ready image
    GLuint scanout_program;
    GLuint dummy_vao;         // attribute-less VAO for the fullscreen triangle
    GLint  scanout_vram_loc;
    GLint  scanout_off_loc;
    GLint  scanout_size_loc;
    GLint  scanout_d24_loc;

    GLuint vram_viewer_texture; // RGBA8 1024x512 for ImGui VRAM viewer
    VramViewParams vram_view;   // how the viewer decodes VRAM (set from the UI)

    // VRAM-viewer pass: decodes the unified VRAM into vram_viewer_texture on
    // the GPU thread. It has to read vram_tex, not the CPU-side mirror: the
    // mirror only ever receives uploads and DMA, so everything the game
    // rasterises was missing from the viewer and the display area showed black.
    GLuint viewer_fbo;
    GLuint viewer_program;
    GLint  viewer_vram_loc;
    GLint  viewer_mode_loc;
    GLint  viewer_clut_loc;
    GLint  viewer_flags_loc;   // x=greyscale, y=show_alpha, z=shift24

    // Shader Uniform Location
    GLint uniform_offset_loc; // Location ID of the 'offset' uniform in the vertex shader
    GLint uniform_use_texture_loc;
    GLint uniform_raw_texture_loc; // 1 = use raw texture color (no modulation)
    GLint uniform_vram_texture_loc;
    GLint uniform_screen_scale_loc; // Location for screen scaling uniform
    GLint uniform_tex_window_loc;   // Location for ivec4 u_texWindow (and_x,and_y,or_x,or_y)
    GLint uniform_dither_loc;       // Location for u_dither_enable (1=on, 0=off)
    GLint uniform_stp_mode_loc;     /* -1=off, 0=opaque pass (discard STP=1), 1=blend pass (discard STP=0) */
    GLint uniform_set_mask_loc;     /* Location for u_set_mask (GP0(E6).0) */
    GLint uniform_mask_test_loc;    /* Location for u_mask_test (GP0(E6).1) */

    // CPU-Side Buffers (Temporary storage before uploading to GPU)
    // These hold the data pushed by the GPU command handlers.
    RendererPosition positions_data[VERTEX_BUFFER_LEN]; // CPU buffer for vertex positions
    RendererColor colors_data[VERTEX_BUFFER_LEN];       // CPU buffer for vertex colors
    RendererTexCoord texcoords_data[VERTEX_BUFFER_LEN]; // CPU buffer for texture coordinates
    RendererTPage tpage_data[VERTEX_BUFFER_LEN];        // CPU buffer for TPage/CLUT

    // State Tracking
    uint32_t vertex_count;      // Number of vertices currently buffered in the CPU-side arrays
    bool initialized;           // Flag indicating if the renderer has been successfully initialized
    bool texture_enabled;       // Current texture mode
    bool raw_texture_enabled;   // Current raw-texture mode (skip modulation)
    float screen_width;         // Target display width (in PSX pixels)
    float screen_height;        // Target display height (in PSX pixels)
    bool semi_trans_enabled;    // Whether semi-transparency blending is active
    uint8_t semi_trans_mode;    // 0=B/2+F/2, 1=B+F, 2=B-F, 3=B+F/4
    bool dither_enabled;        // Whether 4x4 PSX dithering is active for current primitive
    bool set_mask_enabled;      // GP0(E6).0 — force bit 15 set on every pixel drawn
    bool mask_test_enabled;     // GP0(E6).1 — skip pixels whose destination bit 15 is set

    /* Cached pipeline state — snapshot into each batch for GPU thread replay */
    int16_t  cached_offset_x, cached_offset_y;
    int32_t  cached_tex_window[4];  /* and_x, and_y, or_x, or_y */
    int32_t  cached_scissor[4];     /* gl_x, gl_y, clip_w, clip_h (GL coords) */
    int16_t  draw_area[4];          /* left, top, right, bottom as GP0(E3h)/(E4h) gave them */
    bool     last_prim_isolated;    /* the primitive just pushed must not be extended
                                     * (renderer_isolate.h) */

    /* Display region — cropped from CRTC state, passed to GPU thread blit */
    uint16_t display_x, display_y, display_w, display_h;
    bool     display_depth24;   /* GPUSTAT.21 — display area is packed 24bpp */
    bool     display_blank;     /* GPUSTAT.23 — display off: hardware shows black */

    /* GPU render thread (Phase 2 threading refactor) */
    SDL_Thread*  gpu_thread;
    SDL_Mutex*   gpu_mutex;
    SDL_Condition*    frame_ready;   /* GPU wakes when CPU submits a frame */
    SDL_Condition*    frame_done;    /* CPU waits if GPU is behind */
    SDL_AtomicInt gpu_stop;
    int          write_idx;     /* CPU writes to slot [write_idx]; GPU reads [1-write_idx] */
    int          frames_pending;
    SDL_Window*  sdl_window;    /* needed by GPU thread for SwapWindow */
    SDL_GLContext gl_context;   /* moved from main thread to GPU thread */

} GlRenderer;

/* --- The backend's own entry points ---
 * These were declared in include/renderer.h until the vtable split; the
 * public header now declares only the backend-neutral API. */
void glr_request_vram_readback(GlRenderer* renderer);
const uint16_t* glr_get_vram_readback(uint32_t* seq_out);
bool glr_init(GlRenderer* renderer);
void glr_push_triangle(GlRenderer* renderer, RendererPosition pos[3], RendererColor col[3], RendererTexCoord tex[3], uint16_t clut, uint16_t tpage);
void glr_push_quad(GlRenderer* renderer, RendererPosition pos[4], RendererColor col[4], RendererTexCoord tex[4], uint16_t clut, uint16_t tpage);
void glr_set_texture_mode(GlRenderer* renderer, bool enabled);
void glr_set_raw_texture_mode(GlRenderer* renderer, bool enabled);
void glr_set_screen_scale(GlRenderer* renderer, uint16_t width, uint16_t height);
void glr_set_texture_window(GlRenderer* renderer, uint8_t mask_x, uint8_t mask_y, uint8_t offset_x, uint8_t offset_y);
void glr_get_pool_stats(GlRenderer* renderer, uint32_t* used, uint32_t* peak, uint32_t* updates, uint32_t* skips);
void glr_upload_vram(GlRenderer* renderer, const uint16_t* vram_data);
void glr_upload_vram_rect(GlRenderer* renderer, const uint16_t* vram_data, uint16_t x, uint16_t y, uint16_t w, uint16_t h);
void glr_draw(GlRenderer* renderer);
void glr_blit_vram(GlRenderer* renderer, uint16_t vram_x, uint16_t vram_y, uint16_t width, uint16_t height);
void glr_display(GlRenderer* renderer);
void glr_set_draw_offset(GlRenderer* renderer, int16_t x, int16_t y);
void glr_set_drawing_area(GlRenderer* renderer, uint16_t left, uint16_t top, uint16_t right, uint16_t bottom);
void glr_set_dither_mode(GlRenderer* renderer, bool enabled);
void glr_set_mask_mode(GlRenderer* renderer, bool enabled);
void glr_set_mask_test(GlRenderer* renderer, bool enabled);
void glr_set_semi_trans_mode(GlRenderer* renderer, bool enabled, uint8_t mode);
void glr_push_line(GlRenderer* renderer, RendererPosition pos[2], RendererColor col[2]);
GLuint glr_get_display_texture(GlRenderer* renderer);
GLuint glr_get_scanout_texture(GlRenderer* renderer);
void glr_update_vram_viewer(GlRenderer* renderer, const uint8_t* vram_bytes);
GLuint glr_get_vram_viewer_texture(GlRenderer* renderer);
void glr_destroy(GlRenderer* renderer);
void glr_start_gpu_thread(GlRenderer* renderer, SDL_Window* window, SDL_GLContext ctx);
void glr_stop_gpu_thread(GlRenderer* renderer);
void glr_set_display_region(GlRenderer* renderer, uint16_t x, uint16_t y, uint16_t w, uint16_t h);
void glr_set_vram_view_params(GlRenderer* renderer, const VramViewParams* p);
void glr_set_display_depth24(GlRenderer* renderer, bool depth24);
void glr_set_display_blank(GlRenderer* renderer, bool blank);
void glr_submit_frame(GlRenderer* renderer, void* imgui_draw_data);
bool glr_read_vram_rect(GlRenderer* renderer, uint16_t* vram, uint16_t x, uint16_t y, uint16_t w, uint16_t h);
void glr_wait_frame_done(GlRenderer* renderer);

#endif /* RENDERER_GL_H */
