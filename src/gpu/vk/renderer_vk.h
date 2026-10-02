/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 ZioZoni95
 *
 * Part of ZoniStation One, a PlayStation 1 emulator.
 * See LICENSE for the full licence text and THIRD-PARTY.md for the
 * components of this project that have other authors.
 */
#ifndef RENDERER_VK_H
#define RENDERER_VK_H

/* Private header of the Vulkan 1.3 backend. Only renderer_vk.c and the vk_*
 * sources may include it — nothing above the GfxBackend vtable names a Vulkan
 * type, exactly as nothing names a GL one. */

#include "vk_device.h"
#include "gpu_backend.h"

#define VKR_VRAM_W 1024
#define VKR_VRAM_H 512

/* Recording limits, sized like the GL backend's so a frame that fits there fits
 * here and an A/B never diverges because one side dropped work the other kept. */
#define VKR_MAX_BATCHES      8192
#define VKR_MAX_VRAM_UPDATES 4096   /* fills are uploads: see GPU_MAX_VRAM_UPDATES */
#define VKR_MAX_OPS          (VKR_MAX_BATCHES + VKR_MAX_VRAM_UPDATES)
#define VKR_VRAM_POOL_SIZE   (16 * 1024 * 1024)

/* One vertex, interleaved.
 *
 * GL keeps four parallel arrays and four VBOs. Vulkan gets one 16-byte
 * interleaved stride instead, for a reason that is not taste: the PS1 colour is
 * three bytes, and a three-component 8-bit *vertex* format is optional in
 * Vulkan while the four-component one is mandatory. Padding to four and letting
 * the shader read a uvec3 out of an R8G8B8A8_UINT attribute keeps the backend
 * off an optional feature. The alignment falls out nicely as well. */
typedef struct {
    int16_t  x, y;                 /* loc 0: R16G16_SINT   */
    uint8_t  r, g, b, pad;         /* loc 1: R8G8B8A8_UINT */
    int16_t  u, v;                 /* loc 2: R16G16_SINT   */
    uint16_t clut, tpage;          /* loc 3: R16G16_UINT   */
} VkrVertex;

/* Per-batch state snapshot, replayed on the render thread. Mirrors GpuBatch. */
typedef struct {
    uint32_t first_vertex, vertex_count;
    bool     textured, raw_texture, semi_trans, dither, set_mask, mask_test;
    uint8_t  semi_trans_mode;
    bool     is_line;
    float    screen_w, screen_h;
    int16_t  offset_x, offset_y;
    int32_t  tex_window[4];
    int32_t  scissor[4];           /* x, y, w, h in VRAM pixels */
} VkrBatch;

typedef struct {
    uint16_t x, y, w, h;
    uint32_t data_offset;          /* into the frame's staging pool */
} VkrVramUpdate;

typedef enum { VKR_OP_VRAM_UPDATE = 0, VKR_OP_BATCH } VkrOpType;
typedef struct { VkrOpType type; uint32_t index; } VkrOp;

/* Everything recorded for one frame. Double-buffered: the CPU fills one slot
 * while the render thread replays the other. */
typedef struct {
    VkrBatch      batches[VKR_MAX_BATCHES];
    uint32_t      batch_count;
    VkrVramUpdate vram_updates[VKR_MAX_VRAM_UPDATES];
    uint32_t      vram_update_count;
    VkrOp         ops[VKR_MAX_OPS];
    uint32_t      op_count;
    void*         imgui_draw_data;
    uint16_t      disp_x, disp_y, disp_w, disp_h;
    bool          disp_depth24, disp_blank;
    VramViewParams view;
} VkrFrame;

/* A GPU image plus what is needed to sample and render to it. */
typedef struct {
    VkImage        image;
    VkDeviceMemory memory;
    VkImageView    view;
    VkFormat       format;
    uint32_t       w, h;
    VkImageLayout  layout;
} VkrImage;

/* A host-visible buffer, mapped for the process's lifetime. */
typedef struct {
    VkBuffer       buffer;
    VkDeviceMemory memory;
    void*          mapped;
    VkDeviceSize   size;
} VkrBuffer;

/* The five blend configurations a PS1 primitive can be drawn with. */
typedef enum {
    VKR_BLEND_OFF = 0,
    VKR_BLEND_HALF_HALF,      /* mode 0: B/2 + F/2 */
    VKR_BLEND_ADD,            /* mode 1: B + F     */
    VKR_BLEND_SUBTRACT,       /* mode 2: B - F     */
    VKR_BLEND_ADD_QUARTER,    /* mode 3: B + F/4   */
    VKR_BLEND_COUNT
} VkrBlend;

typedef struct {
    VkContext ctx;
    bool      initialized;

    /* Images. vram is the one VRAM: render target, upload target and sample
     * source at once, which is why it lives in GENERAL layout permanently. */
    VkrImage  vram;
    VkrImage  scanout;
    VkrImage  viewer;
    VkSampler sampler;

    /* Descriptors: one layout (a combined image sampler), one set pointing at
     * the VRAM image, shared by all three passes because all three sample it. */
    VkDescriptorSetLayout set_layout;
    VkDescriptorPool      desc_pool;
    VkDescriptorSet       vram_set;

    /* Pipelines. The PS1 rasteriser needs one per blend configuration because
     * blend state is baked into a Vulkan pipeline; the two fullscreen passes
     * need one each. Line and triangle topologies double the rasteriser set. */
    VkPipelineLayout ps1_layout, scanout_layout, viewer_layout;
    VkPipeline       ps1_tri[VKR_BLEND_COUNT];
    VkPipeline       ps1_line[VKR_BLEND_COUNT];
    VkPipeline       scanout_pipe;
    VkPipeline       viewer_pipe;
    VkPipelineCache  pipe_cache;

    /* Per-frame-in-flight GPU objects. */
    VkCommandPool   cmd_pool;
    VkCommandBuffer cmd[VK_FRAMES_IN_FLIGHT];
    VkFence         fence[VK_FRAMES_IN_FLIGHT];
    VkSemaphore     acquire_sem[VK_FRAMES_IN_FLIGHT];
    VkSemaphore     release_sem[VK_MAX_SWAP_IMAGES];
    VkrBuffer       vertex_buf[VK_FRAMES_IN_FLIGHT];
    VkrBuffer       staging_buf[VK_FRAMES_IN_FLIGHT];
    uint32_t        frame_slot;

    /* Readbacks (GP0(80h)/GP0(C0h), the inspector's whole-VRAM copy, the frame
     * dump): one command buffer, one fence and one host-visible buffer for the
     * life of the device. They were a command buffer allocated per call, a
     * vkQueueWaitIdle, and a file-static buffer that outlived the device it
     * was made from, so a Vulkan -> GL -> Vulkan switch handed the new device
     * a dead VkBuffer. Here they go down with the device in vkr_destroy(). */
    VkCommandBuffer rb_cmd;
    VkFence         rb_fence;
    VkrBuffer       readback_buf;

    /* ImGui's descriptor set for each image it draws, created once. */
    VkDescriptorSet imgui_scanout_set;
    VkDescriptorSet imgui_vram_set;
    VkDescriptorSet imgui_viewer_set;

    /* CPU-side recording, the same shape the GL backend uses. */
    VkrVertex* cpu_vertices;        /* VERTEX_BUFFER_LEN staging */
    uint32_t   vertex_count;
    bool       pending_is_line;     /* topology of the vertices staged but not yet flushed:
                                     * a Vulkan pipeline bakes it in, so triangles and lines
                                     * cannot share a batch the way they could share a
                                     * glDrawArrays call */
    bool       texture_enabled, raw_texture_enabled, semi_trans_enabled;
    bool       dither_enabled, set_mask_enabled, mask_test_enabled;
    uint8_t    semi_trans_mode;
    float      screen_width, screen_height;
    int16_t    cached_offset_x, cached_offset_y;
    int32_t    cached_tex_window[4];
    int32_t    cached_scissor[4];
    uint16_t   display_x, display_y, display_w, display_h;
    bool       display_depth24, display_blank;
    VramViewParams vram_view;

    /* Render thread, mirroring the GL backend's. */
    SDL_Thread*    gpu_thread;
    SDL_Mutex*     gpu_mutex;
    SDL_Condition* frame_ready;
    SDL_Condition* frame_done;
    SDL_AtomicInt  gpu_stop;
    int            write_idx;
    int            frames_pending;
    SDL_Window*    sdl_window;
} VkRenderer;

/* Mirrors the glr_* surface one for one; the vtable adapters are the only
 * callers. */
bool vkr_init(VkRenderer* r, SDL_Window* window, const GfxDeviceRequest* req);
void vkr_destroy(VkRenderer* r);
void vkr_start_gpu_thread(VkRenderer* r, SDL_Window* window);
void vkr_stop_gpu_thread(VkRenderer* r);
void vkr_submit_frame(VkRenderer* r, void* imgui_draw_data);
void vkr_wait_frame_done(VkRenderer* r);

void vkr_push_triangle(VkRenderer* r, RendererPosition p[3], RendererColor c[3],
                       RendererTexCoord t[3], uint16_t clut, uint16_t tpage);
void vkr_push_quad(VkRenderer* r, RendererPosition p[4], RendererColor c[4],
                   RendererTexCoord t[4], uint16_t clut, uint16_t tpage);
void vkr_push_line(VkRenderer* r, RendererPosition p[2], RendererColor c[2]);
void vkr_draw(VkRenderer* r);

void vkr_set_texture_mode(VkRenderer* r, bool e);
void vkr_set_raw_texture_mode(VkRenderer* r, bool e);
void vkr_set_screen_scale(VkRenderer* r, uint16_t w, uint16_t h);
void vkr_set_texture_window(VkRenderer* r, uint8_t mx, uint8_t my, uint8_t ox, uint8_t oy);
void vkr_set_draw_offset(VkRenderer* r, int16_t x, int16_t y);
void vkr_set_drawing_area(VkRenderer* r, uint16_t l, uint16_t t, uint16_t rt, uint16_t b);
void vkr_set_semi_trans_mode(VkRenderer* r, bool e, uint8_t mode);
void vkr_set_dither_mode(VkRenderer* r, bool e);
void vkr_set_mask_mode(VkRenderer* r, bool e);
void vkr_set_mask_test(VkRenderer* r, bool e);

void vkr_upload_vram(VkRenderer* r, const uint16_t* data);
void vkr_upload_vram_rect(VkRenderer* r, const uint16_t* data,
                          uint16_t x, uint16_t y, uint16_t w, uint16_t h);
bool vkr_read_vram_rect(VkRenderer* r, uint16_t* out,
                        uint16_t x, uint16_t y, uint16_t w, uint16_t h);
void vkr_request_vram_readback(VkRenderer* r);
const uint16_t* vkr_get_vram_readback(uint32_t* seq_out);

void vkr_set_display_region(VkRenderer* r, uint16_t x, uint16_t y, uint16_t w, uint16_t h);
void vkr_set_display_depth24(VkRenderer* r, bool d);
void vkr_set_display_blank(VkRenderer* r, bool b);
void vkr_set_vram_view_params(VkRenderer* r, const VramViewParams* p);
void vkr_update_vram_viewer(VkRenderer* r, const uint8_t* bytes);
void vkr_get_pool_stats(VkRenderer* r, uint32_t* used, uint32_t* peak,
                        uint32_t* updates, uint32_t* skips);

GfxTexHandle vkr_get_display_texture(VkRenderer* r);
GfxTexHandle vkr_get_scanout_texture(VkRenderer* r);
GfxTexHandle vkr_get_vram_viewer_texture(VkRenderer* r);

#endif /* RENDERER_VK_H */
