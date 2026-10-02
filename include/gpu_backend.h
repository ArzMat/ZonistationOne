/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 ZioZoni95
 *
 * Part of ZoniStation One, a PlayStation 1 emulator.
 * See LICENSE for the full licence text and THIRD-PARTY.md for the
 * components of this project that have other authors.
 */
#ifndef GPU_BACKEND_H
#define GPU_BACKEND_H

/* The rendering backend interface.
 *
 * Everything above this line — gpu.c, gpu_commands.c, debug_ui.cpp, main.c —
 * talks to the renderer through renderer.h, which forwards to whichever backend
 * is registered here. Nothing outside a backend's own translation units may
 * name a GL or a Vulkan type; that is the whole point of the file. The public
 * header used to pull <GL/glew.h> into every unit that touched gpu.h, which is
 * why GLuint appeared in debug_ui.cpp and in the vertex structs.
 *
 * There is exactly one live backend at a time. A backend's state is a single
 * file-static instance inside its own .c, reached through `impl` — the GPU
 * command queue in renderer_gl.c was already file-static, so this is not a new
 * restriction, only an honest one.
 */

#include <stdint.h>
#include <stdbool.h>
#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Vertex data handed to a backend ---
 *
 * These were GL-typed (GLshort/GLubyte/GLushort) and lived in renderer.h, which
 * is how <GL/glew.h> reached every unit that included gpu.h. GLshort *is*
 * int16_t, so this is the same layout under a name that does not drag a
 * graphics API along with it. */

/* A 2D vertex position in PSX VRAM coordinates. */
typedef struct { int16_t x, y; } RendererPosition;

/* An RGB colour, 8 bits per component. */
typedef struct { uint8_t r, g, b; } RendererColor;

/* Texture coordinates, absolute VRAM coordinates. */
typedef struct { int16_t u, v; } RendererTexCoord;

/* CLUT id (palette X,Y) and texture page id (BaseX, BaseY, depth). */
typedef struct { uint16_t clut, tpage; } RendererTPage;

/* How many vertices are staged before a draw is forced. */
#define VERTEX_BUFFER_LEN (64 * 1024)

/* VRAM viewer decode modes, mirroring PCSX-Redux's vram-viewer widget: VRAM is
 * a raw 1024x512 halfword store that games address as 4bpp/8bpp indexed, 16bpp
 * direct or 24bpp packed depending on the region, so the viewer has to be told
 * how to read the bytes it is being asked to show. */
typedef enum {
    VRAM_VIEW_4BPP = 0,
    VRAM_VIEW_8BPP,
    VRAM_VIEW_16BPP,
    VRAM_VIEW_24BPP
} VramViewMode;

typedef struct {
    VramViewMode mode;
    int      shift24;         /* 0-3: byte phase for 24bpp unpacking */
    bool     greyscale;
    bool     show_alpha;      /* render the mask bit (bit 15) as intensity */
    uint16_t clut_x, clut_y;  /* CLUT position for the indexed modes */
    /* The viewer is on screen this frame. Set by debug_ui.cpp every frame; a
     * backend skips its decode pass (a 1024x512 fullscreen draw) when false,
     * which is every frame of the gameplay shell. Zero-initialised: off until
     * the viewer is first drawn. */
    bool     enabled;
} VramViewParams;

typedef enum {
    GFX_BACKEND_GL33 = 0,
    GFX_BACKEND_VULKAN
} GfxBackendType;

/* What ImGui is handed for a texture. GL puts a texture name in it, Vulkan a
 * VkDescriptorSet. Neither type is visible from here, and debug_ui.cpp casts it
 * straight to ImTextureID without knowing which backend produced it. */
typedef uintptr_t GfxTexHandle;

/* One selectable GPU. Vulkan fills this from vkEnumeratePhysicalDevices; the GL
 * backend reports the PRIME choices, which cannot be applied without a restart
 * because the GLX vendor is resolved at the first dlopen of libGL. */
typedef struct {
    char name[256];   /* VK_MAX_PHYSICAL_DEVICE_NAME_SIZE, so a device name never truncates */
    bool live_switchable;   /* false on GL: the change needs a restart */
} GfxDeviceInfo;

typedef struct {
    GfxBackendType type;
    int            device_index;   /* -1 = let the backend choose */
    uint32_t       internal_scale; /* 1 = native; >1 is Vulkan-only */
} GfxDeviceRequest;

/* The backend's own state. Opaque here by design. */
typedef void* GfxImpl;

typedef struct GfxBackend {
    const char*    name;
    GfxBackendType type;

    /* --- lifecycle --- */
    bool (*init)(GfxImpl* impl, SDL_Window* window, const GfxDeviceRequest* req);
    void (*destroy)(GfxImpl impl);
    int  (*enumerate_devices)(GfxDeviceInfo* out, int max);

    /* --- render thread --- */
    void (*start_thread)(GfxImpl impl, SDL_Window* window, void* native_ctx);
    void (*stop_thread)(GfxImpl impl);
    void (*submit_frame)(GfxImpl impl, void* imgui_draw_data);
    void (*wait_frame_done)(GfxImpl impl);

    /* --- geometry --- */
    void (*push_triangle)(GfxImpl impl, RendererPosition p[3], RendererColor c[3],
                          RendererTexCoord t[3], uint16_t clut, uint16_t tpage);
    void (*push_quad)(GfxImpl impl, RendererPosition p[4], RendererColor c[4],
                      RendererTexCoord t[4], uint16_t clut, uint16_t tpage);
    void (*push_line)(GfxImpl impl, RendererPosition p[2], RendererColor c[2]);
    void (*draw)(GfxImpl impl);
    void (*display)(GfxImpl impl);
    void (*blit_vram)(GfxImpl impl, uint16_t x, uint16_t y, uint16_t w, uint16_t h);

    /* --- PS1 pipeline state; each implementation flushes the pending batch --- */
    void (*set_texture_mode)(GfxImpl impl, bool enabled);
    void (*set_raw_texture_mode)(GfxImpl impl, bool enabled);
    void (*set_screen_scale)(GfxImpl impl, uint16_t w, uint16_t h);
    void (*set_texture_window)(GfxImpl impl, uint8_t mx, uint8_t my, uint8_t ox, uint8_t oy);
    void (*set_draw_offset)(GfxImpl impl, int16_t x, int16_t y);
    void (*set_drawing_area)(GfxImpl impl, uint16_t l, uint16_t t, uint16_t r, uint16_t b);
    void (*set_semi_trans_mode)(GfxImpl impl, bool enabled, uint8_t mode);
    void (*set_dither_mode)(GfxImpl impl, bool enabled);
    void (*set_mask_mode)(GfxImpl impl, bool enabled);
    void (*set_mask_test)(GfxImpl impl, bool enabled);

    /* --- VRAM --- */
    void (*upload_vram)(GfxImpl impl, const uint16_t* data);
    void (*upload_vram_rect)(GfxImpl impl, const uint16_t* data,
                             uint16_t x, uint16_t y, uint16_t w, uint16_t h);
    bool (*read_vram_rect)(GfxImpl impl, uint16_t* out,
                           uint16_t x, uint16_t y, uint16_t w, uint16_t h);
    void (*request_vram_readback)(GfxImpl impl);
    const uint16_t* (*get_vram_readback)(uint32_t* seq_out);

    /* --- CRTC / display --- */
    void (*set_display_region)(GfxImpl impl, uint16_t x, uint16_t y, uint16_t w, uint16_t h);
    void (*set_display_depth24)(GfxImpl impl, bool depth24);
    void (*set_display_blank)(GfxImpl impl, bool blank);

    /* --- debug surfaces --- */
    void (*set_vram_view_params)(GfxImpl impl, const VramViewParams* p);
    void (*update_vram_viewer)(GfxImpl impl, const uint8_t* vram_bytes);
    void (*get_pool_stats)(GfxImpl impl, uint32_t* used, uint32_t* peak,
                           uint32_t* updates, uint32_t* skips);

    /* --- handles handed to ImGui --- */
    GfxTexHandle (*get_display_texture)(GfxImpl impl);
    GfxTexHandle (*get_scanout_texture)(GfxImpl impl);
    GfxTexHandle (*get_vram_viewer_texture)(GfxImpl impl);
} GfxBackend;

/* The registry. renderer.c picks one of these at init. A backend that was not
 * compiled in is absent from the table rather than present and failing, so the
 * UI can say why instead of offering a control that cannot work. */
const GfxBackend* gfx_backend_get(GfxBackendType type);
const char*       gfx_backend_unavailable_reason(GfxBackendType type);

extern const GfxBackend gfx_backend_gl33;
#ifdef ENABLE_VULKAN
extern const GfxBackend gfx_backend_vulkan;
#endif

#ifdef __cplusplus
}
#endif

#endif /* GPU_BACKEND_H */
