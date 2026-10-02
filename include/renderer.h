/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 ZioZoni95
 *
 * Part of ZoniStation One, a PlayStation 1 emulator.
 * See LICENSE for the full licence text and THIRD-PARTY.md for the
 * components of this project that have other authors.
 */
#ifndef RENDERER_H
#define RENDERER_H

#include <stdint.h>
#include <stdbool.h>
#include <SDL3/SDL.h>

/* The backend interface, and with it the vertex types, VramViewParams and
 * VERTEX_BUFFER_LEN. Those used to be declared here in GL types, which is how
 * <GL/glew.h> reached every unit that includes gpu.h. */
#include "gpu_backend.h"

/* The renderer as the rest of the machine sees it: which backend is live, and
 * that backend's private state.
 *
 * Everything that used to be here — the VAO, the four VBOs, the uniform
 * locations, the three FBOs and the ~1 MB of CPU-side vertex staging — belongs
 * to one backend and now lives inside it (src/gpu/renderer_gl.h).
 *
 * Gpu embeds this by value. savestate.c derives the two spans it writes from
 * offsetof(Gpu, renderer) and sizeof(Renderer) rather than hard-coding them
 * (savestate.c:76-78), so shrinking this struct moves both boundaries by the
 * same amount and does not change a single saved byte. */
typedef struct {
    const GfxBackend* vt;    /* NULL until renderer_init() has succeeded */
    GfxImpl           impl;  /* the live backend's own state */
} Renderer;

// --- Function Prototypes ---

/**
 * @brief Initializes the OpenGL renderer.
 * Compiles shaders, links program, creates VAO and VBOs, sets initial GL state.
 * Must be called with the GL context current on the calling thread.
 */
/**
 * @brief Binds a rendering backend to this Renderer without touching the GPU.
 *
 * Must be called before anything else, renderer_init() included. gpu_reset_state()
 * pushes the GP0/GP1 reset values through renderer_set_texture_window(),
 * _set_drawing_area(), _set_display_region() and _set_display_blank() while the
 * machine is being built (gpu.c:661-668), which is *before* main.c reaches
 * renderer_init() — and those four calls carry real state, because the GL
 * backend's init does not reset those fields. Splitting the bind from the init
 * keeps that order working instead of quietly dropping the reset values.
 *
 * @return false if the requested backend was not compiled in.
 */
bool renderer_select_backend(Renderer* renderer, GfxBackendType type);

/** @brief The live backend, or NULL before renderer_select_backend(). */
const GfxBackend* renderer_backend(const Renderer* renderer);

bool renderer_init(Renderer* renderer, SDL_Window* window);

/** @brief renderer_init() with a device and internal-scale request.
 *  Vulkan honours both; the GL backend ignores them, because the GLX vendor
 *  is resolved at the first dlopen of libGL and it has no scaled target. */
bool renderer_init_ex(Renderer* renderer, SDL_Window* window, const GfxDeviceRequest* req);

/** @brief The GPUs a backend offers, without needing a live Renderer.
 *  Asked by the interface before a switch, so it can list a backend that is
 *  not the one currently running. Returns how many entries were written. */
int renderer_enumerate_devices(GfxBackendType type, GfxDeviceInfo* out, int max);

/**
 * @brief Starts the GPU render thread. Call after renderer_init() and AFTER
 * releasing the GL context from the main thread with SDL_GL_MakeCurrent(w, NULL).
 */
void renderer_start_gpu_thread(Renderer* renderer, SDL_Window* window, void* native_ctx);

/**
 * @brief Signals GPU thread to stop and waits for it to exit.
 */
void renderer_stop_gpu_thread(Renderer* renderer);

/**
 * @brief Submits the current frame's batch list to the GPU thread for rendering.
 * Swaps write/read buffers. CPU continues immediately with the next frame.
 * If the GPU thread is still rendering the PREVIOUS frame, CPU blocks here
 * (bounded wait — GPU renders one frame per VBlank interval).
 * @param imgui_draw_data  Pointer to ImGui draw data (valid until next ImGui::NewFrame()).
 */
void renderer_submit_frame(Renderer* renderer, void* imgui_draw_data);

/**
 * @brief Waits for the GPU thread to finish the most recently submitted frame.
 * Call before ImGui::NewFrame(), to ensure the previous frame's ImGui draw
 * data is no longer in use. main.c calls it after emulating the field, not
 * before: emulation only records into the write slot and needs no wait.
 */
void renderer_wait_frame_done(Renderer* renderer);

/**
 * @brief Gets the OpenGL texture ID used for the off-screen display.
 * @param renderer Pointer to the Renderer.
 * @return OpenGL texture ID.
 */
GfxTexHandle renderer_get_display_texture(Renderer* renderer);
/* The display-ready image produced by the scanout pass (what the screen shows). */
GfxTexHandle renderer_get_scanout_texture(Renderer* renderer);
void   renderer_set_vram_view_params(Renderer* renderer, const VramViewParams* p);
void   renderer_update_vram_viewer(Renderer* renderer, const uint8_t* vram_bytes);
GfxTexHandle renderer_get_vram_viewer_texture(Renderer* renderer);

/* --- Phase 5: cross-thread VRAM readback ---------------------------------
 * The GPU thread owns the GL context, so the CPU cannot read vram_tex. Raise
 * a request, then poll: the sequence number changes once the GPU thread has
 * published a fresh copy. Asynchronous by design - the returned buffer is the
 * VRAM as of some recent frame, not as of this instant.
 * -------------------------------------------------------------------------- */
void            renderer_request_vram_readback(Renderer* renderer);
const uint16_t* renderer_get_vram_readback(uint32_t* seq_out);

/* Synchronous readback of one VRAM rect into the CPU-side VRAM: flushes the
 * ops queued so far, then copies the rendered pixels back. What GP0(0xC0)
 * and GP0(0x80) need to see polygons the rasterizer drew. */
bool renderer_read_vram_rect(Renderer* renderer, uint16_t* vram,
                             uint16_t x, uint16_t y, uint16_t w, uint16_t h);

/**
 * @brief Buffers a triangle's vertex data for later drawing.
 * Copies position and color data into the renderer's CPU-side buffers.
 * If the buffer is full, it forces a draw call before adding the new triangle.
 * @param renderer Pointer to the Renderer instance.
 * @param pos Array of 3 vertex positions.
 * @param col Array of 3 vertex colors.
 * @param tex Array of 3 texture coordinates (can be NULL if untextured).
 * @param clut CLUT ID (only used if tex is not NULL).
 * @param tpage Texture Page ID (only used if tex is not NULL).
 */
void renderer_push_triangle(Renderer* renderer, RendererPosition pos[3], RendererColor col[3], RendererTexCoord tex[3], uint16_t clut, uint16_t tpage);

/**
 * @brief Buffers a quadrilateral's vertex data (as two triangles) for later drawing.
 * Decomposes the quad into two triangles and copies their vertex data.
 * If the buffer is full, it forces a draw call before adding the new quad.
 * @param renderer Pointer to the Renderer instance.
 * @param pos Array of 4 vertex positions (in PSX order).
 * @param col Array of 4 vertex colors (corresponding to positions).
 * @param tex Array of 4 texture coordinates (can be NULL if untextured).
 * @param clut CLUT ID (only used if tex is not NULL).
 * @param tpage Texture Page ID (only used if tex is not NULL).
 */
void renderer_push_quad(Renderer* renderer, RendererPosition pos[4], RendererColor col[4], RendererTexCoord tex[4], uint16_t clut, uint16_t tpage);

/**
 * @brief Sets the texture mode. Flushes the renderer if the mode changes.
 * @param renderer Pointer to the Renderer instance.
 * @param enabled True to enable texturing, false to disable.
 */
void renderer_set_texture_mode(Renderer* renderer, bool enabled);

/**
 * @brief Toggles raw-texture mode. When enabled, textures are used without color modulation.
 */
void renderer_set_raw_texture_mode(Renderer* renderer, bool enabled);

/**
 * @brief Adjusts how PSX coordinates map to the OpenGL screen by setting the
 *        effective display width/height (in PSX pixels).
 * @param renderer Pointer to the Renderer.
 * @param width Horizontal resolution in PSX pixels (e.g., 320, 512, 640).
 * @param height Vertical resolution in PSX pixels (e.g., 240, 480).
 */
void renderer_set_screen_scale(Renderer* renderer, uint16_t width, uint16_t height);

/**
 * @brief Sets the texture window mask and offset.
 * @param renderer Pointer to the Renderer.
 * @param mask_x Texture window X mask (5 bits).
 * @param mask_y Texture window Y mask (5 bits).
 * @param offset_x Texture window X offset (5 bits).
 * @param offset_y Texture window Y offset (5 bits).
 */
void renderer_set_texture_window(Renderer* renderer, uint8_t mask_x, uint8_t mask_y, uint8_t offset_x, uint8_t offset_y);

/**
 * @brief Uploads VRAM data to the GPU texture (full 1024x512).
 * @param renderer Pointer to the Renderer instance.
 * @param vram_data Pointer to the VRAM data (1024x512 uint16_t).
 */
void renderer_upload_vram(Renderer* renderer, const uint16_t* vram_data);

/**
 * @brief Uploads a rectangular sub-region of VRAM to the GPU texture.
 * Uses glTexSubImage2D with GL_UNPACK_ROW_LENGTH for efficiency.
 * @param renderer  Pointer to the Renderer instance.
 * @param vram_data Pointer to full 1024x512 VRAM buffer.
 * @param x, y      Top-left of the dirty region (VRAM coords).
 * @param w, h      Width and height of the dirty region.
 */
void renderer_upload_vram_rect(Renderer* renderer, const uint16_t* vram_data,
                                uint16_t x, uint16_t y, uint16_t w, uint16_t h);

/**
 * @brief Uploads buffered vertex data to the GPU and performs the OpenGL draw call.
 * Uses glBufferSubData to update VBOs with data from CPU buffers.
 * Issues a glDrawArrays call to render the buffered primitives (as triangles).
 * Resets the vertex count after drawing.
 * @param renderer Pointer to the Renderer instance.
 */
void renderer_draw(Renderer* renderer);

/**
 * @brief Helper function to draw buffered primitives and swap the window buffers.
 * Typically called once per frame from the main loop.
 * @param renderer Pointer to the Renderer instance.
 * // Removed SDL_Window* - swap happens in main loop
 */
void renderer_display(Renderer* renderer);

/**
 * @brief Sets the drawing offset uniform in the vertex shader.
 * Forces a draw of currently buffered primitives before updating the offset.
 * @param renderer Pointer to the Renderer instance.
 * @param x The signed horizontal drawing offset.
 * @param y The signed vertical drawing offset.
 */
void renderer_set_draw_offset(Renderer* renderer, int16_t x, int16_t y);

/**
 * @brief Destroys OpenGL resources (VBOs, VAO, Shader Program).
 * Should be called before the OpenGL context is destroyed.
 * @param renderer Pointer to the Renderer instance to destroy.
 */
void renderer_destroy(Renderer* renderer);

/**
 * @brief Blits the VRAM texture to the screen as a full-screen quad.
 * This displays the actual VRAM contents (used for BIOS logo, etc.).
 * @param renderer Pointer to the Renderer instance.
 * @param vram_x X start coordinate in VRAM.
 * @param vram_y Y start coordinate in VRAM.
 * @param width Display width.
 * @param height Display height.
 */
void renderer_blit_vram(Renderer* renderer, uint16_t vram_x, uint16_t vram_y, uint16_t width, uint16_t height);

/**
 * @brief Checks for OpenGL errors using glGetError() and prints them.
 * Useful for debugging OpenGL calls.
 * @param location A string indicating where the check is being performed.
 */
void check_gl_error(const char* location);

/**
 * @brief Renders a line segment (2 vertices) immediately.
 * Flushes any pending triangle batch first, then draws the line.
 * @param renderer Pointer to the Renderer instance.
 * @param pos Array of 2 vertex positions.
 * @param col Array of 2 vertex colors.
 */
void renderer_push_line(Renderer* renderer, RendererPosition pos[2], RendererColor col[2]);

/**
 * @brief Sets the OpenGL scissor test rectangle from the PSX drawing area.
 * Enables GL_SCISSOR_TEST and calls glScissor with the given coordinates.
 * Forces a draw of any pending primitives before updating the scissor.
 * @param renderer Pointer to the Renderer.
 * @param left   Drawing area left boundary (inclusive).
 * @param top    Drawing area top boundary (inclusive).
 * @param right  Drawing area right boundary (inclusive).
 * @param bottom Drawing area bottom boundary (inclusive).
 */
void renderer_set_drawing_area(Renderer* renderer, uint16_t left, uint16_t top,
                                uint16_t right, uint16_t bottom);

/**
 * @brief Enables or disables semi-transparency blending and sets the blend mode.
 * Flushes any pending primitives before changing blend state.
 * @param renderer  Pointer to the Renderer.
 * @param enabled   True to enable blending for semi-transparent primitives.
 * @param mode      0=B/2+F/2, 1=B+F, 2=B-F, 3=B+F/4
 */
void renderer_set_semi_trans_mode(Renderer* renderer, bool enabled, uint8_t mode);

/**
 * @brief Enables or disables PSX 4x4 dithering in the fragment shader.
 * Per PSX spec: applies to gouraud-shaded/textured polygons and lines only.
 * Flush is forced before mode change.
 * @param renderer  Pointer to the Renderer.
 * @param enabled   True to enable dithering.
 */
void renderer_set_dither_mode(Renderer* renderer, bool enabled);

/**
 * @brief Sets GP0(0xE6) bit 0 — force the mask bit set on every pixel drawn.
 * The unified VRAM texture keeps the PSX mask bit in its alpha channel, so a
 * rasterized pixel has to carry the same bit 15 a CPU write would: 0 normally,
 * 1 when this is enabled (or when a textured pixel's source texel has it set).
 * In 24bpp display modes that bit is picture data, so getting it wrong tints
 * whole areas of the screen.
 * @param renderer  Pointer to the Renderer.
 * @param enabled   True when GP0(E6).0 is set.
 */
void renderer_set_mask_mode(Renderer* renderer, bool enabled);
/* GP0(E6).1 - skip drawing a pixel whose destination halfword already has bit
 * 15 set. Honoured only where the texture barrier lets a shader read the render
 * target; without it the test cannot see rasterized pixels and is ignored. */
void renderer_set_mask_test(Renderer* renderer, bool enabled);

/**
 * @brief Sets the PSX display region to crop from the FBO when blitting to screen.
 * Called from gpu_update_display_mapping() whenever GP1(05/06/07/08) are written.
 * Thread-safe to call from CPU thread — snapshotted into GpuFrame at submit time.
 */
void renderer_set_display_region(Renderer* renderer, uint16_t x, uint16_t y, uint16_t w, uint16_t h);
void renderer_set_display_depth24(Renderer* renderer, bool depth24);
/* GP1(03).0: display off. DOCS/graphicsprocessingunitgpu.md:647 — "The \"Off\"
 * settings displays a black picture". Games blank the screen across a scene
 * change while they rebuild VRAM; without this the raw VRAM is on screen. */
void renderer_set_display_blank(Renderer* renderer, bool blank);

/* Staging-pool telemetry for the Lua console (bytes used in the current write
 * slot, all-time single-frame peak, queued updates, rects dropped for space). */
void renderer_get_pool_stats(Renderer* renderer, uint32_t* used, uint32_t* peak,
                             uint32_t* updates, uint32_t* skips);

#endif // RENDERER_H