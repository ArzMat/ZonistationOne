/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 ZioZoni95
 *
 * Part of ZoniStation One, a PlayStation 1 emulator.
 * See LICENSE for the full licence text and THIRD-PARTY.md for the
 * components of this project that have other authors.
 */

/* The Vulkan 1.3 backend.
 *
 * Written for parity with renderer_gl.c, not to be cleverer than it: same
 * unified VRAM image, same 5:5:5:1 expansion, same batching, same scanout, same
 * order of operations. A pixel that differs between the two backends is a
 * defect until proven otherwise, which is only checkable if the two are
 * deliberately built the same way.
 *
 * Where Vulkan forces a difference it is written down at the point it happens.
 * The three that matter:
 *   - blend state is baked into a pipeline, so the four PS1 semi-transparency
 *     modes plus "off" are five pipelines instead of five glBlendFunc calls;
 *   - the VRAM image is both colour attachment and sampled texture, so it lives
 *     in GENERAL layout and gets a pipeline barrier between batches, which is
 *     exactly where GL calls glTextureBarrier();
 *   - per-batch uniforms are push constants, so there are no uniform locations
 *     to cache.
 */

#include "renderer_vk.h"
#include "log.h"
#include "frame_events.h"
#include "vk_imgui.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "ps1.vert.spv.h"
#include "ps1.frag.spv.h"
#include "fullscreen.vert.spv.h"
#include "scanout.frag.spv.h"
#include "vram_view.frag.spv.h"

/* Push-constant blocks, laid out to match the GLSL exactly. std430 rules put
 * ivec4 on a 16-byte boundary and ivec3 likewise, which is where the padding in
 * VkrViewerPush comes from — get this wrong and the shader reads neighbouring
 * fields with no diagnostic at all. */
typedef struct {
    int32_t offset[2];
    float   screen_scale[2];
    int32_t tex_window[4];
    int32_t use_texture, raw_texture, dither_enable, stp_mode, set_mask, mask_test;
} VkrPs1Push;                      /* 56 bytes */

typedef struct {
    int32_t disp_off[2];
    int32_t disp_size[2];
    int32_t depth24;
} VkrScanoutPush;                  /* 20 bytes */

typedef struct {
    int32_t clut[2];
    int32_t _pad[2];               /* ivec3 flags starts on a 16-byte boundary */
    int32_t flags[3];
    int32_t mode;
} VkrViewerPush;                   /* 32 bytes */

#define VK_TRY(expr, what)                                                  \
    do {                                                                    \
        VkResult _r = (expr);                                               \
        if (_r != VK_SUCCESS) {                                             \
            LOG_RENDERER_ERROR("[VK] %s: %s", (what), vk_result_str(_r));   \
            return false;                                                   \
        }                                                                   \
    } while (0)

/* ------------------------------------------------------------------------- */
/* Recording state, file-static like the GL backend's                         */
/* ------------------------------------------------------------------------- */

static VkrFrame  s_frame[2];
static VkrVertex s_vtx_pool[2][VERTEX_BUFFER_LEN];
static uint32_t  s_vtx_used[2];
static uint8_t   s_vram_pool[2][VKR_VRAM_POOL_SIZE];
static uint32_t  s_vram_pool_used[2];
static uint32_t  s_pool_peak, s_pool_updates, s_pool_skips;
/* First op of each slot not yet replayed. A synchronous readback replays the
 * write slot part of the way so the pixels it reads include this field's
 * primitives; the frame replay then carries on from here instead of running
 * those ops twice (a semi-transparent draw would blend twice). Render thread
 * writes it, only while the CPU thread is blocked in vkr_read_vram_rect() or
 * after the slot has been submitted. Same scheme as the GL backend. */
static uint32_t  s_exec_from[2];

/* Whole-VRAM readback, matching the GL backend's async channel: two buffers
 * published by index, for the same reason as there. The render thread fills
 * the one readers are not holding, then flips s_readback_pub and bumps the
 * sequence; a Lua probe reading the result from inside the emulation, which
 * now overlaps the render thread's previous frame, never sees a half-written
 * buffer. The sequence is atomic for the same reason. */
static uint16_t      s_readback_vram[2][VKR_VRAM_W * VKR_VRAM_H];
static SDL_AtomicInt s_readback_pub;
static SDL_AtomicInt s_readback_request;
static SDL_AtomicInt s_readback_seq;

/* ------------------------------------------------------------------------- */
/* Small resource helpers                                                     */
/* ------------------------------------------------------------------------- */

static bool vkr_buffer_create(VkRenderer* r, VkrBuffer* b, VkDeviceSize size,
                              VkBufferUsageFlags usage) {
    VkBufferCreateInfo bci = {
        .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size        = size,
        .usage       = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VK_TRY(vkCreateBuffer(r->ctx.device, &bci, NULL, &b->buffer), "vkCreateBuffer");

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(r->ctx.device, b->buffer, &req);
    /* HOST_VISIBLE | HOST_COHERENT and permanently mapped: every buffer here is
     * written once per frame by the CPU and read once by the GPU, which is the
     * case a staging copy would only make slower. */
    uint32_t type = vk_find_memory_type(&r->ctx, req.memoryTypeBits,
                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == UINT32_MAX) {
        LOG_RENDERER_ERROR("[VK] no host-visible coherent memory type");
        return false;
    }
    VkMemoryAllocateInfo mai = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = req.size,
        .memoryTypeIndex = type,
    };
    VK_TRY(vkAllocateMemory(r->ctx.device, &mai, NULL, &b->memory), "vkAllocateMemory(buffer)");
    VK_TRY(vkBindBufferMemory(r->ctx.device, b->buffer, b->memory, 0), "vkBindBufferMemory");
    VK_TRY(vkMapMemory(r->ctx.device, b->memory, 0, VK_WHOLE_SIZE, 0, &b->mapped), "vkMapMemory");
    b->size = size;
    return true;
}

static void vkr_buffer_destroy(VkRenderer* r, VkrBuffer* b) {
    if (b->mapped) { vkUnmapMemory(r->ctx.device, b->memory); b->mapped = NULL; }
    if (b->buffer) { vkDestroyBuffer(r->ctx.device, b->buffer, NULL); b->buffer = VK_NULL_HANDLE; }
    if (b->memory) { vkFreeMemory(r->ctx.device, b->memory, NULL);    b->memory = VK_NULL_HANDLE; }
}

static bool vkr_image_create(VkRenderer* r, VkrImage* im, uint32_t w, uint32_t h,
                             VkFormat fmt, VkImageUsageFlags usage) {
    VkImageCreateInfo ici = {
        .sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType     = VK_IMAGE_TYPE_2D,
        .format        = fmt,
        .extent        = { w, h, 1 },
        .mipLevels     = 1,
        .arrayLayers   = 1,
        .samples       = VK_SAMPLE_COUNT_1_BIT,
        .tiling        = VK_IMAGE_TILING_OPTIMAL,
        .usage         = usage,
        .sharingMode   = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VK_TRY(vkCreateImage(r->ctx.device, &ici, NULL, &im->image), "vkCreateImage");

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(r->ctx.device, im->image, &req);
    uint32_t type = vk_find_memory_type(&r->ctx, req.memoryTypeBits,
                                        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) { LOG_RENDERER_ERROR("[VK] no device-local memory type"); return false; }
    VkMemoryAllocateInfo mai = {
        .sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize  = req.size,
        .memoryTypeIndex = type,
    };
    VK_TRY(vkAllocateMemory(r->ctx.device, &mai, NULL, &im->memory), "vkAllocateMemory(image)");
    VK_TRY(vkBindImageMemory(r->ctx.device, im->image, im->memory, 0), "vkBindImageMemory");

    VkImageViewCreateInfo vci = {
        .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image    = im->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format   = fmt,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    VK_TRY(vkCreateImageView(r->ctx.device, &vci, NULL, &im->view), "vkCreateImageView");

    im->format = fmt; im->w = w; im->h = h;
    im->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return true;
}

static void vkr_image_destroy(VkRenderer* r, VkrImage* im) {
    if (im->view)   { vkDestroyImageView(r->ctx.device, im->view, NULL); im->view = VK_NULL_HANDLE; }
    if (im->image)  { vkDestroyImage(r->ctx.device, im->image, NULL);    im->image = VK_NULL_HANDLE; }
    if (im->memory) { vkFreeMemory(r->ctx.device, im->memory, NULL);     im->memory = VK_NULL_HANDLE; }
}

/* A full barrier on one image. Deliberately blunt: these fire a handful of
 * times per frame at most, and a precise src/dst mask here would buy nothing
 * measurable while being one more thing to get subtly wrong. The one that runs
 * per *batch* is vkr_barrier_vram_feedback() below, and that one is tight. */
static void vkr_image_barrier(VkCommandBuffer cmd, VkrImage* im,
                              VkImageLayout new_layout,
                              VkAccessFlags src_access, VkAccessFlags dst_access,
                              VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier b = {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask       = src_access,
        .dstAccessMask       = dst_access,
        .oldLayout           = im->layout,
        .newLayout           = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = im->image,
        .subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
    im->layout = new_layout;
}

/* The feedback-loop barrier: what glTextureBarrier() does in the GL backend.
 *
 * The fragment shader samples the VRAM image while that same image is the
 * colour attachment, so between one batch and the next the writes have to be
 * made visible to the sampler. Layout stays GENERAL throughout — a transition
 * would be a no-op here and GENERAL is what lets one image be both things. */
static void vkr_barrier_vram_feedback(VkCommandBuffer cmd, VkrImage* vram) {
    VkImageMemoryBarrier b = {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask       = VK_ACCESS_SHADER_READ_BIT,
        .oldLayout           = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout           = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = vram->image,
        .subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, NULL, 0, NULL, 1, &b);
}

static VkShaderModule vkr_shader(VkRenderer* r, const unsigned char* code, size_t len) {
    VkShaderModuleCreateInfo ci = {
        .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = len,
        .pCode    = (const uint32_t*)code,
    };
    VkShaderModule m = VK_NULL_HANDLE;
    VkResult res = vkCreateShaderModule(r->ctx.device, &ci, NULL, &m);
    if (res != VK_SUCCESS) {
        LOG_RENDERER_ERROR("[VK] vkCreateShaderModule: %s", vk_result_str(res));
        return VK_NULL_HANDLE;
    }
    return m;
}

/* ------------------------------------------------------------------------- */
/* Pipelines                                                                  */
/* ------------------------------------------------------------------------- */

/* The four PS1 semi-transparency modes, as Vulkan blend state.
 *
 * Straight from the GL backend's apply_semi_trans_blend(), which is straight
 * from DOCS/graphicsprocessingunitgpu.md:
 *   0: B/2 + F/2   1: B + F   2: B - F   3: B + F/4
 * The halving and quartering ride on the blend constant, set dynamically to
 * 0.5 and 0.25 — the same trick glBlendColor() plays.
 *
 * Alpha is never blended: srcAlpha ONE, dstAlpha ZERO. It is not opacity, it
 * carries the PS1 mask bit, and blending it would corrupt the bit the scanout
 * and the mask test both read. */
static void vkr_blend_state(VkrBlend mode, VkPipelineColorBlendAttachmentState* out) {
    memset(out, 0, sizeof(*out));
    out->colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (mode == VKR_BLEND_OFF) { out->blendEnable = VK_FALSE; return; }

    out->blendEnable         = VK_TRUE;
    out->srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    out->dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    out->alphaBlendOp        = VK_BLEND_OP_ADD;
    out->colorBlendOp        = VK_BLEND_OP_ADD;

    switch (mode) {
        case VKR_BLEND_HALF_HALF:
            out->srcColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_ALPHA;
            out->dstColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_ALPHA;
            break;
        case VKR_BLEND_ADD:
            out->srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            out->dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            break;
        case VKR_BLEND_SUBTRACT:
            out->srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
            out->dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            out->colorBlendOp        = VK_BLEND_OP_REVERSE_SUBTRACT;
            break;
        case VKR_BLEND_ADD_QUARTER:
            out->srcColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_ALPHA;
            out->dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
            break;
        default: break;
    }
}

/* The blend constant each mode wants. GL sets it with glBlendColor. */
float vkr_blend_constant(VkrBlend mode) {
    switch (mode) {
        case VKR_BLEND_HALF_HALF:   return 0.5f;
        case VKR_BLEND_ADD_QUARTER: return 0.25f;
        default:                    return 1.0f;
    }
}

static bool vkr_create_pipeline(VkRenderer* r,
                                VkShaderModule vs, VkShaderModule fs,
                                VkPipelineLayout layout,
                                VkFormat colour_format,
                                VkPrimitiveTopology topology,
                                const VkVertexInputBindingDescription* binding,
                                const VkVertexInputAttributeDescription* attrs, uint32_t n_attrs,
                                VkrBlend blend,
                                VkPipeline* out) {
    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT,   .module = vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" },
    };
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount   = binding ? 1 : 0,
        .pVertexBindingDescriptions      = binding,
        .vertexAttributeDescriptionCount = n_attrs,
        .pVertexAttributeDescriptions    = attrs,
    };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = topology,
    };
    VkPipelineViewportStateCreateInfo vp = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1,
    };
    /* No culling and no depth: the PS1 GPU has neither, and the draw order in
     * the command stream is the only ordering there is. */
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode    = VK_CULL_MODE_NONE,
        .frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth   = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    VkPipelineColorBlendAttachmentState ba;
    vkr_blend_state(blend, &ba);
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments    = &ba,
    };
    VkDynamicState dyn[3] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                              VK_DYNAMIC_STATE_BLEND_CONSTANTS };
    VkPipelineDynamicStateCreateInfo ds = {
        .sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 3,
        .pDynamicStates    = dyn,
    };
    /* Dynamic rendering: no VkRenderPass, no VkFramebuffer, and the target
     * format declared right here. It is what makes a resolution change a
     * matter of new images rather than a rebuilt pipeline. */
    VkPipelineRenderingCreateInfo prc = {
        .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount    = 1,
        .pColorAttachmentFormats = &colour_format,
    };
    VkGraphicsPipelineCreateInfo gp = {
        .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext               = &prc,
        .stageCount          = 2,
        .pStages             = stages,
        .pVertexInputState   = &vi,
        .pInputAssemblyState = &ia,
        .pViewportState      = &vp,
        .pRasterizationState = &rs,
        .pMultisampleState   = &ms,
        .pColorBlendState    = &cb,
        .pDynamicState       = &ds,
        .layout              = layout,
    };
    VK_TRY(vkCreateGraphicsPipelines(r->ctx.device, r->pipe_cache, 1, &gp, NULL, out),
           "vkCreateGraphicsPipelines");
    return true;
}

static bool vkr_create_pipelines(VkRenderer* r) {
    VkShaderModule ps1_vs  = vkr_shader(r, zs1_shader_ps1_vert,       zs1_shader_ps1_vert_len);
    VkShaderModule ps1_fs  = vkr_shader(r, zs1_shader_ps1_frag,       zs1_shader_ps1_frag_len);
    VkShaderModule full_vs = vkr_shader(r, zs1_shader_fullscreen_vert, zs1_shader_fullscreen_vert_len);
    VkShaderModule scan_fs = vkr_shader(r, zs1_shader_scanout_frag,   zs1_shader_scanout_frag_len);
    VkShaderModule view_fs = vkr_shader(r, zs1_shader_vram_view_frag, zs1_shader_vram_view_frag_len);
    if (!ps1_vs || !ps1_fs || !full_vs || !scan_fs || !view_fs) return false;

    bool ok = true;

    /* One interleaved binding; see VkrVertex for why the colour is padded. */
    VkVertexInputBindingDescription binding = {
        .binding = 0, .stride = sizeof(VkrVertex), .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
    };
    VkVertexInputAttributeDescription attrs[4] = {
        { .location = 0, .binding = 0, .format = VK_FORMAT_R16G16_SINT,     .offset = offsetof(VkrVertex, x)    },
        { .location = 1, .binding = 0, .format = VK_FORMAT_R8G8B8A8_UINT,   .offset = offsetof(VkrVertex, r)    },
        { .location = 2, .binding = 0, .format = VK_FORMAT_R16G16_SINT,     .offset = offsetof(VkrVertex, u)    },
        { .location = 3, .binding = 0, .format = VK_FORMAT_R16G16_UINT,     .offset = offsetof(VkrVertex, clut) },
    };

    for (int b = 0; b < VKR_BLEND_COUNT && ok; b++) {
        ok = ok && vkr_create_pipeline(r, ps1_vs, ps1_fs, r->ps1_layout, r->vram.format,
                                       VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                                       &binding, attrs, 4, (VkrBlend)b, &r->ps1_tri[b]);
        ok = ok && vkr_create_pipeline(r, ps1_vs, ps1_fs, r->ps1_layout, r->vram.format,
                                       VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
                                       &binding, attrs, 4, (VkrBlend)b, &r->ps1_line[b]);
    }
    ok = ok && vkr_create_pipeline(r, full_vs, scan_fs, r->scanout_layout, r->scanout.format,
                                   VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                                   NULL, NULL, 0, VKR_BLEND_OFF, &r->scanout_pipe);
    ok = ok && vkr_create_pipeline(r, full_vs, view_fs, r->viewer_layout, r->viewer.format,
                                   VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                                   NULL, NULL, 0, VKR_BLEND_OFF, &r->viewer_pipe);

    vkDestroyShaderModule(r->ctx.device, ps1_vs,  NULL);
    vkDestroyShaderModule(r->ctx.device, ps1_fs,  NULL);
    vkDestroyShaderModule(r->ctx.device, full_vs, NULL);
    vkDestroyShaderModule(r->ctx.device, scan_fs, NULL);
    vkDestroyShaderModule(r->ctx.device, view_fs, NULL);
    return ok;
}

/* ------------------------------------------------------------------------- */
/* Init / destroy                                                             */
/* ------------------------------------------------------------------------- */

static bool vkr_create_descriptors(VkRenderer* r) {
    VkDescriptorSetLayoutBinding b = {
        .binding         = 0,
        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT,
    };
    VkDescriptorSetLayoutCreateInfo lci = {
        .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings    = &b,
    };
    VK_TRY(vkCreateDescriptorSetLayout(r->ctx.device, &lci, NULL, &r->set_layout),
           "vkCreateDescriptorSetLayout");

    /* One pool for this backend's single set and for every texture ImGui wants
     * to draw. FREE_DESCRIPTOR_SET is set because ImGui's backend frees the
     * sets it made for textures that go away. */
    VkDescriptorPoolSize sizes[1] = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 128 },
    };
    VkDescriptorPoolCreateInfo pci = {
        .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets       = 128,
        .poolSizeCount = 1,
        .pPoolSizes    = sizes,
    };
    VK_TRY(vkCreateDescriptorPool(r->ctx.device, &pci, NULL, &r->desc_pool),
           "vkCreateDescriptorPool");

    VkDescriptorSetAllocateInfo ai = {
        .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool     = r->desc_pool,
        .descriptorSetCount = 1,
        .pSetLayouts        = &r->set_layout,
    };
    VK_TRY(vkAllocateDescriptorSets(r->ctx.device, &ai, &r->vram_set),
           "vkAllocateDescriptorSets(vram)");

    /* GENERAL, not SHADER_READ_ONLY_OPTIMAL: this image is sampled while it is
     * the colour attachment, and the descriptor's layout has to say so. */
    VkDescriptorImageInfo ii = {
        .sampler     = r->sampler,
        .imageView   = r->vram.view,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
    };
    VkWriteDescriptorSet w = {
        .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet          = r->vram_set,
        .dstBinding      = 0,
        .descriptorCount = 1,
        .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo      = &ii,
    };
    vkUpdateDescriptorSets(r->ctx.device, 1, &w, 0, NULL);

    /* Three pipeline layouts, one per push-constant shape. */
    struct { VkPipelineLayout* out; uint32_t size; VkShaderStageFlags stages; } lay[3] = {
        { &r->ps1_layout,     (uint32_t)sizeof(VkrPs1Push),
          VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT },
        { &r->scanout_layout, (uint32_t)sizeof(VkrScanoutPush), VK_SHADER_STAGE_FRAGMENT_BIT },
        { &r->viewer_layout,  (uint32_t)sizeof(VkrViewerPush),  VK_SHADER_STAGE_FRAGMENT_BIT },
    };
    for (int i = 0; i < 3; i++) {
        VkPushConstantRange pcr = { .stageFlags = lay[i].stages, .offset = 0, .size = lay[i].size };
        VkPipelineLayoutCreateInfo pl = {
            .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount         = 1,
            .pSetLayouts            = &r->set_layout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges    = &pcr,
        };
        VK_TRY(vkCreatePipelineLayout(r->ctx.device, &pl, NULL, lay[i].out),
               "vkCreatePipelineLayout");
    }
    return true;
}

static bool vkr_create_frame_objects(VkRenderer* r) {
    VkCommandPoolCreateInfo cpi = {
        .sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = r->ctx.queue_family,
    };
    VK_TRY(vkCreateCommandPool(r->ctx.device, &cpi, NULL, &r->cmd_pool), "vkCreateCommandPool");

    VkCommandBufferAllocateInfo cbi = {
        .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool        = r->cmd_pool,
        .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = VK_FRAMES_IN_FLIGHT,
    };
    VK_TRY(vkAllocateCommandBuffers(r->ctx.device, &cbi, r->cmd), "vkAllocateCommandBuffers");

    for (int i = 0; i < VK_FRAMES_IN_FLIGHT; i++) {
        /* Signalled, so the first frame does not wait on a fence nothing ever
         * submitted. */
        VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                                  .flags = VK_FENCE_CREATE_SIGNALED_BIT };
        VK_TRY(vkCreateFence(r->ctx.device, &fci, NULL, &r->fence[i]), "vkCreateFence");
        VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VK_TRY(vkCreateSemaphore(r->ctx.device, &sci, NULL, &r->acquire_sem[i]),
               "vkCreateSemaphore(acquire)");

        if (!vkr_buffer_create(r, &r->vertex_buf[i],
                               (VkDeviceSize)VERTEX_BUFFER_LEN * sizeof(VkrVertex),
                               VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) return false;
        if (!vkr_buffer_create(r, &r->staging_buf[i], VKR_VRAM_POOL_SIZE,
                               VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) return false;
    }
    /* The readback objects, see VkRenderer. The fence starts unsignalled: it is
     * only ever waited on right after the submit that signals it. */
    {
        VkCommandBufferAllocateInfo rbi = {
            .sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool        = r->cmd_pool,
            .level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        VK_TRY(vkAllocateCommandBuffers(r->ctx.device, &rbi, &r->rb_cmd), "vkAllocateCommandBuffers(readback)");
        VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        VK_TRY(vkCreateFence(r->ctx.device, &fci, NULL, &r->rb_fence), "vkCreateFence(readback)");
        if (!vkr_buffer_create(r, &r->readback_buf, (VkDeviceSize)VKR_VRAM_W * VKR_VRAM_H * 4,
                               VK_BUFFER_USAGE_TRANSFER_DST_BIT)) return false;
    }

    /* One release semaphore per swapchain image, not per frame slot: a
     * semaphore signalled by a present cannot be reused until that present has
     * been consumed, and the number of images in flight is the swapchain's
     * business, not ours. */
    for (uint32_t i = 0; i < r->ctx.swap_image_count; i++) {
        VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VK_TRY(vkCreateSemaphore(r->ctx.device, &sci, NULL, &r->release_sem[i]),
               "vkCreateSemaphore(release)");
    }
    return true;
}

/* Puts the three images into the layouts the frame loop assumes, once, with a
 * one-shot command buffer. Doing it here means the per-frame path never has to
 * ask "is this the first time". */
static bool vkr_initial_transitions(VkRenderer* r) {
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VkCommandBuffer cmd = r->cmd[0];
    VK_TRY(vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer(init)");

    vkr_image_barrier(cmd, &r->vram, VK_IMAGE_LAYOUT_GENERAL, 0,
                      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT);
    vkr_image_barrier(cmd, &r->scanout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0,
                      VK_ACCESS_SHADER_READ_BIT,
                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    vkr_image_barrier(cmd, &r->viewer, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0,
                      VK_ACCESS_SHADER_READ_BIT,
                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

    VK_TRY(vkEndCommandBuffer(cmd), "vkEndCommandBuffer(init)");
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                        .commandBufferCount = 1, .pCommandBuffers = &cmd };
    VK_TRY(vkQueueSubmit(r->ctx.queue, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit(init)");
    VK_TRY(vkQueueWaitIdle(r->ctx.queue), "vkQueueWaitIdle(init)");
    return true;
}

bool vkr_init(VkRenderer* r, SDL_Window* window, const GfxDeviceRequest* req) {
    memset(r, 0, sizeof(*r));
    r->sdl_window = window;

    int device_index = req ? req->device_index : -1;
    if (!vk_context_create(&r->ctx, window, device_index)) return false;

    /* RGBA8_UNORM at 1024x512, exactly the GL backend's vram_tex: PS1 halfwords
     * stored 5:5:5:1 expanded to 8 bits per channel, alpha carrying the mask
     * bit. Same format, same expansion, same size — the parity check between
     * the backends is a byte comparison or it is nothing. */
    if (!vkr_image_create(r, &r->vram, VKR_VRAM_W, VKR_VRAM_H, VK_FORMAT_R8G8B8A8_UNORM,
                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
        return false;
    if (!vkr_image_create(r, &r->scanout, VKR_VRAM_W, VKR_VRAM_H, VK_FORMAT_R8G8B8A8_UNORM,
                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
        return false;
    if (!vkr_image_create(r, &r->viewer, VKR_VRAM_W, VKR_VRAM_H, VK_FORMAT_R8G8B8A8_UNORM,
                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT))
        return false;

    /* NEAREST everywhere, no mips, clamp to edge. The PS1 has no filtering and
     * texelFetch ignores the sampler's filter anyway; this matters for what
     * ImGui draws with these images. */
    VkSamplerCreateInfo sci = {
        .sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter    = VK_FILTER_NEAREST,
        .minFilter    = VK_FILTER_NEAREST,
        .mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxLod       = 0.0f,
        .borderColor  = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
    };
    VK_TRY(vkCreateSampler(r->ctx.device, &sci, NULL, &r->sampler), "vkCreateSampler");

    VkPipelineCacheCreateInfo pcc = { .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
    VK_TRY(vkCreatePipelineCache(r->ctx.device, &pcc, NULL, &r->pipe_cache), "vkCreatePipelineCache");

    if (!vkr_create_descriptors(r))    return false;
    if (!vkr_create_pipelines(r))      return false;
    if (!vkr_create_frame_objects(r))  return false;
    if (!vkr_initial_transitions(r))   return false;

    r->cpu_vertices = (VkrVertex*)calloc(VERTEX_BUFFER_LEN, sizeof(VkrVertex));
    if (!r->cpu_vertices) return false;

    /* The GL backend defaults these when they are still zero; do the same, and
     * for the same reason — gpu_reset_state() may not have run yet. */
    if (r->screen_width  <= 0.0f) r->screen_width  = (float)VKR_VRAM_W;
    if (r->screen_height <= 0.0f) r->screen_height = (float)VKR_VRAM_H;
    r->cached_tex_window[0] = 0xFF; r->cached_tex_window[1] = 0xFF;
    r->cached_scissor[2] = VKR_VRAM_W; r->cached_scissor[3] = VKR_VRAM_H;

    memset(s_frame, 0, sizeof(s_frame));
    r->initialized = true;
    LOG_RENDERER_INFO("[VK] backend initialised on %s", r->ctx.device_name);
    return true;
}

void vkr_destroy(VkRenderer* r) {
    if (!r->initialized) return;
    vkDeviceWaitIdle(r->ctx.device);

    for (int b = 0; b < VKR_BLEND_COUNT; b++) {
        if (r->ps1_tri[b])  vkDestroyPipeline(r->ctx.device, r->ps1_tri[b], NULL);
        if (r->ps1_line[b]) vkDestroyPipeline(r->ctx.device, r->ps1_line[b], NULL);
        r->ps1_tri[b] = r->ps1_line[b] = VK_NULL_HANDLE;
    }
    if (r->scanout_pipe) vkDestroyPipeline(r->ctx.device, r->scanout_pipe, NULL);
    if (r->viewer_pipe)  vkDestroyPipeline(r->ctx.device, r->viewer_pipe, NULL);
    if (r->pipe_cache)   vkDestroyPipelineCache(r->ctx.device, r->pipe_cache, NULL);

    if (r->ps1_layout)     vkDestroyPipelineLayout(r->ctx.device, r->ps1_layout, NULL);
    if (r->scanout_layout) vkDestroyPipelineLayout(r->ctx.device, r->scanout_layout, NULL);
    if (r->viewer_layout)  vkDestroyPipelineLayout(r->ctx.device, r->viewer_layout, NULL);
    if (r->desc_pool)      vkDestroyDescriptorPool(r->ctx.device, r->desc_pool, NULL);
    if (r->set_layout)     vkDestroyDescriptorSetLayout(r->ctx.device, r->set_layout, NULL);

    for (int i = 0; i < VK_FRAMES_IN_FLIGHT; i++) {
        if (r->fence[i])       vkDestroyFence(r->ctx.device, r->fence[i], NULL);
        if (r->acquire_sem[i]) vkDestroySemaphore(r->ctx.device, r->acquire_sem[i], NULL);
        vkr_buffer_destroy(r, &r->vertex_buf[i]);
        vkr_buffer_destroy(r, &r->staging_buf[i]);
    }
    for (uint32_t i = 0; i < VK_MAX_SWAP_IMAGES; i++)
        if (r->release_sem[i]) vkDestroySemaphore(r->ctx.device, r->release_sem[i], NULL);
    if (r->rb_fence) { vkDestroyFence(r->ctx.device, r->rb_fence, NULL); r->rb_fence = VK_NULL_HANDLE; }
    vkr_buffer_destroy(r, &r->readback_buf);
    r->rb_cmd = VK_NULL_HANDLE;   /* freed with the pool below */
    if (r->cmd_pool) vkDestroyCommandPool(r->ctx.device, r->cmd_pool, NULL);

    if (r->sampler) vkDestroySampler(r->ctx.device, r->sampler, NULL);
    vkr_image_destroy(r, &r->viewer);
    vkr_image_destroy(r, &r->scanout);
    vkr_image_destroy(r, &r->vram);

    vk_context_destroy(&r->ctx);
    free(r->cpu_vertices);
    r->cpu_vertices = NULL;
    r->initialized = false;
    LOG_RENDERER_INFO("[VK] backend destroyed");
}

/* ------------------------------------------------------------------------- */
/* Recording (CPU thread)                                                     */
/* ------------------------------------------------------------------------- */

static void vkr_record_batch(VkRenderer* r, bool is_line) {
    if (r->vertex_count == 0) return;

    VkrFrame* f  = &s_frame[r->write_idx];
    uint32_t  wi = (uint32_t)r->write_idx;

    if (f->batch_count >= VKR_MAX_BATCHES || f->op_count >= VKR_MAX_OPS) {
        LOG_RENDERER_WARN("[VK] batch list full, dropping %u vertices", r->vertex_count);
        r->vertex_count = 0;
        return;
    }
    if (s_vtx_used[wi] + r->vertex_count > VERTEX_BUFFER_LEN) {
        LOG_RENDERER_WARN("[VK] vertex pool full, dropping %u vertices", r->vertex_count);
        r->vertex_count = 0;
        return;
    }

    /* The Pipeline view's "Draw batches" counter, fed exactly where the GL
     * backend feeds it (glr_draw): one event per recorded batch. It was GL-only,
     * which hid how many batches this backend made. */
    frame_events_record(FEV_DRAW_BATCH, r->vertex_count);

    memcpy(&s_vtx_pool[wi][s_vtx_used[wi]], r->cpu_vertices,
           (size_t)r->vertex_count * sizeof(VkrVertex));

    VkrBatch* b = &f->batches[f->batch_count];
    memset(b, 0, sizeof(*b));
    b->first_vertex    = s_vtx_used[wi];
    b->vertex_count    = r->vertex_count;
    b->textured        = r->texture_enabled;
    b->raw_texture     = r->raw_texture_enabled;
    b->semi_trans      = r->semi_trans_enabled;
    b->semi_trans_mode = r->semi_trans_mode;
    b->dither          = r->dither_enabled;
    b->set_mask        = r->set_mask_enabled;
    b->mask_test       = r->mask_test_enabled;
    b->is_line         = is_line;
    b->screen_w        = r->screen_width;
    b->screen_h        = r->screen_height;
    b->offset_x        = r->cached_offset_x;
    b->offset_y        = r->cached_offset_y;
    memcpy(b->tex_window, r->cached_tex_window, sizeof(b->tex_window));
    memcpy(b->scissor,    r->cached_scissor,    sizeof(b->scissor));

    f->ops[f->op_count].type  = VKR_OP_BATCH;
    f->ops[f->op_count].index = f->batch_count;
    f->op_count++;
    f->batch_count++;

    s_vtx_used[wi] += r->vertex_count;
    r->vertex_count = 0;
}

void vkr_draw(VkRenderer* r) {
    if (!r->initialized) return;
    vkr_record_batch(r, r->pending_is_line);
}

/* Every state setter flushes first, exactly as the GL backend does: the state
 * is snapshotted into the batch, so vertices staged under the old state must
 * become their own batch before the new value lands. */
#define VKR_FLUSH(r) do { if ((r)->vertex_count) vkr_record_batch((r), (r)->pending_is_line); } while (0)

static void vkr_stage_vertex(VkRenderer* r, const RendererPosition* p, const RendererColor* c,
                             const RendererTexCoord* t, uint16_t clut, uint16_t tpage) {
    VkrVertex* v = &r->cpu_vertices[r->vertex_count++];
    v->x = p->x; v->y = p->y;
    v->r = c->r; v->g = c->g; v->b = c->b; v->pad = 0;
    v->u = t ? t->u : 0;
    v->v = t ? t->v : 0;
    v->clut = clut; v->tpage = tpage;
}

void vkr_push_triangle(VkRenderer* r, RendererPosition p[3], RendererColor c[3],
                       RendererTexCoord t[3], uint16_t clut, uint16_t tpage) {
    if (!r->initialized) return;
    if (r->pending_is_line) VKR_FLUSH(r);
    r->pending_is_line = false;
    if (r->vertex_count + 3 > VERTEX_BUFFER_LEN) vkr_record_batch(r, false);
    for (int i = 0; i < 3; i++) vkr_stage_vertex(r, &p[i], &c[i], t ? &t[i] : NULL, clut, tpage);
}

void vkr_push_quad(VkRenderer* r, RendererPosition p[4], RendererColor c[4],
                   RendererTexCoord t[4], uint16_t clut, uint16_t tpage) {
    if (!r->initialized) return;
    if (r->pending_is_line) VKR_FLUSH(r);
    r->pending_is_line = false;
    if (r->vertex_count + 6 > VERTEX_BUFFER_LEN) vkr_record_batch(r, false);
    /* Same split the GL backend uses: 0-1-2 then 1-2-3. Keeping the winding
     * identical matters even with culling off, because it fixes which vertex is
     * the provoking one for the flat-interpolated tpage_info. */
    static const int order[6] = { 0, 1, 2, 1, 2, 3 };
    for (int i = 0; i < 6; i++) {
        int k = order[i];
        vkr_stage_vertex(r, &p[k], &c[k], t ? &t[k] : NULL, clut, tpage);
    }
}

void vkr_push_line(VkRenderer* r, RendererPosition p[2], RendererColor c[2]) {
    if (!r->initialized) return;
    if (!r->pending_is_line) VKR_FLUSH(r);
    r->pending_is_line = true;
    if (r->vertex_count + 2 > VERTEX_BUFFER_LEN) vkr_record_batch(r, true);
    for (int i = 0; i < 2; i++) vkr_stage_vertex(r, &p[i], &c[i], NULL, 0, 0);
}

/* Every setter compares before it flushes, as the GL backend's do.
 *
 * They used to flush unconditionally, and gpu_commands.c calls three or four
 * of them in front of every primitive, so the first setter of primitive N
 * closed the batch of primitive N-1 even when nothing had changed: every
 * primitive became its own batch, its own rendering scope and its own
 * pipeline barrier, and a 3D field ran into VKR_MAX_BATCHES. The comparisons
 * cover every field vkr_record_batch() copies into the batch snapshot
 * (textured, raw_texture, semi_trans + mode, dither, set_mask, mask_test,
 * screen size, offset, texture window, scissor); the topology is handled by
 * the push functions through pending_is_line. A value that does not change
 * cannot make two batches differ, so merging them changes no pixel. */
void vkr_set_texture_mode(VkRenderer* r, bool e) {
    if (r->texture_enabled == e) return;
    VKR_FLUSH(r); r->texture_enabled = e;
}
void vkr_set_raw_texture_mode(VkRenderer* r, bool e) {
    if (r->raw_texture_enabled == e) return;
    VKR_FLUSH(r); r->raw_texture_enabled = e;
}
void vkr_set_dither_mode(VkRenderer* r, bool e) {
    if (r->dither_enabled == e) return;
    VKR_FLUSH(r); r->dither_enabled = e;
}
void vkr_set_mask_mode(VkRenderer* r, bool e) {
    if (r->set_mask_enabled == e) return;
    VKR_FLUSH(r); r->set_mask_enabled = e;
}
void vkr_set_mask_test(VkRenderer* r, bool e) {
    if (r->mask_test_enabled == e) return;
    VKR_FLUSH(r); r->mask_test_enabled = e;
}

void vkr_set_semi_trans_mode(VkRenderer* r, bool e, uint8_t mode) {
    if (r->semi_trans_enabled == e && r->semi_trans_mode == mode) return;
    VKR_FLUSH(r);
    r->semi_trans_enabled = e;
    r->semi_trans_mode    = mode;
}

void vkr_set_screen_scale(VkRenderer* r, uint16_t w, uint16_t h) {
    /* Full extent here, halved when it becomes a push constant; the GL backend
     * stores it the same way and halves it at glUniform2f time. Keeping the
     * field meaning identical in both is what makes them diffable. */
    const float sw = (w > 0) ? (float)w : (float)VKR_VRAM_W;
    const float sh = (h > 0) ? (float)h : (float)VKR_VRAM_H;
    if (r->screen_width == sw && r->screen_height == sh) return;
    VKR_FLUSH(r);
    r->screen_width  = sw;
    r->screen_height = sh;
}

void vkr_set_texture_window(VkRenderer* r, uint8_t mx, uint8_t my, uint8_t ox, uint8_t oy) {
    /* GP0(E2) gives masks in 8-pixel units. The shader wants the AND/OR pair
     * ready to use, so the arithmetic happens once here, as it does in GL:
     * "Texcoord = (Texcoord AND (NOT (Mask * 8))) OR ((Offset AND Mask) * 8)"
     * (psx-spx gpu/rendering-attributes.md:110). The offset used to go in
     * without the AND, so offset bits outside the mask were ORed into every
     * coordinate on Vulkan and not on GL. */
    const int32_t tw[4] = {
        (int32_t)(~(mx * 8) & 0xFF), (int32_t)(~(my * 8) & 0xFF),
        (int32_t)(((ox & mx) * 8) & 0xFF), (int32_t)(((oy & my) * 8) & 0xFF),
    };
    if (memcmp(r->cached_tex_window, tw, sizeof(tw)) == 0) return;
    VKR_FLUSH(r);
    memcpy(r->cached_tex_window, tw, sizeof(tw));
}

void vkr_set_draw_offset(VkRenderer* r, int16_t x, int16_t y) {
    if (r->cached_offset_x == x && r->cached_offset_y == y) return;
    VKR_FLUSH(r);
    r->cached_offset_x = x;
    r->cached_offset_y = y;
}

void vkr_set_drawing_area(VkRenderer* r, uint16_t l, uint16_t t, uint16_t rt, uint16_t b) {
    /* No Y flip, for the same reason the vertex shader has none: VRAM row N is
     * image row N in both APIs. GL does not flip here either. */
    int w = (int)rt - (int)l + 1;
    int h = (int)b  - (int)t + 1;
    if (w < 0) w = 0;
    if (h < 0) h = 0;
    const int32_t sc[4] = { l, t, w, h };
    if (memcmp(r->cached_scissor, sc, sizeof(sc)) == 0) return;
    VKR_FLUSH(r);
    memcpy(r->cached_scissor, sc, sizeof(sc));
}

void vkr_set_display_region(VkRenderer* r, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    r->display_x = x; r->display_y = y; r->display_w = w; r->display_h = h;
}
void vkr_set_display_depth24(VkRenderer* r, bool d) { r->display_depth24 = d; }
void vkr_set_display_blank(VkRenderer* r, bool b)   { r->display_blank = b; }
void vkr_set_vram_view_params(VkRenderer* r, const VramViewParams* p) { if (p) r->vram_view = *p; }

/* The viewer is filled by a shader pass on the render thread from the VRAM
 * image, not from the CPU mirror — the mirror never receives rasterised pixels,
 * which is exactly the bug the GL backend fixed by moving this to a pass. The
 * CPU-side entry point therefore does nothing but exist for the vtable. */
void vkr_update_vram_viewer(VkRenderer* r, const uint8_t* bytes) { (void)r; (void)bytes; }

void vkr_get_pool_stats(VkRenderer* r, uint32_t* used, uint32_t* peak,
                        uint32_t* updates, uint32_t* skips) {
    /* The write slot only, as the GL backend reports it. The other slot belongs
     * to the render thread, which may be resetting it right now: emulation (and
     * the Lua probe that asks for this) overlaps the previous frame's replay. */
    if (used)    *used    = s_vram_pool_used[r->write_idx];
    if (peak)    *peak    = s_pool_peak;
    if (updates) *updates = s_pool_updates;
    if (skips)   *skips   = s_pool_skips;
}

/* ------------------------------------------------------------------------- */
/* VRAM upload                                                                */
/* ------------------------------------------------------------------------- */

/* Stage a rectangle of PS1 halfwords, expanded to RGBA8 on the way in.
 *
 * GL uploads the raw halfwords and expands in a separate step; doing the
 * expansion here instead means the staging buffer holds exactly what
 * vkCmdCopyBufferToImage will write, with no intermediate. The expansion is
 * ((v<<3)|(v>>2)) per 5-bit channel and bit 15 into alpha — bit-identical to
 * the GL backend's, which is what keeps the two images comparable. */
static void vkr_record_vram_update(VkRenderer* r, const uint16_t* src,
                                   uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    if (!w || !h) return;
    VkrFrame* f  = &s_frame[r->write_idx];
    uint32_t  wi = (uint32_t)r->write_idx;

    if (f->vram_update_count >= VKR_MAX_VRAM_UPDATES || f->op_count >= VKR_MAX_OPS) {
        s_pool_skips++;
        return;
    }
    size_t bytes = (size_t)w * h * 4;
    if (s_vram_pool_used[wi] + bytes > VKR_VRAM_POOL_SIZE) {
        s_pool_skips++;
        return;
    }

    uint8_t* dst = &s_vram_pool[wi][s_vram_pool_used[wi]];
    for (uint32_t row = 0; row < h; row++) {
        const uint16_t* s = src + (size_t)(y + row) * VKR_VRAM_W + x;
        uint8_t*        d = dst + (size_t)row * w * 4;
        for (uint32_t col = 0; col < w; col++) {
            uint16_t v = s[col];
            uint32_t r5 =  v        & 0x1F;
            uint32_t g5 = (v >>  5) & 0x1F;
            uint32_t b5 = (v >> 10) & 0x1F;
            d[col * 4 + 0] = (uint8_t)((r5 << 3) | (r5 >> 2));
            d[col * 4 + 1] = (uint8_t)((g5 << 3) | (g5 >> 2));
            d[col * 4 + 2] = (uint8_t)((b5 << 3) | (b5 >> 2));
            d[col * 4 + 3] = (v & 0x8000) ? 0xFF : 0x00;
        }
    }

    VkrVramUpdate* u = &f->vram_updates[f->vram_update_count];
    u->x = x; u->y = y; u->w = w; u->h = h;
    u->data_offset = s_vram_pool_used[wi];

    f->ops[f->op_count].type  = VKR_OP_VRAM_UPDATE;
    f->ops[f->op_count].index = f->vram_update_count;
    f->op_count++;
    f->vram_update_count++;

    s_vram_pool_used[wi] += (uint32_t)bytes;
    if (s_vram_pool_used[wi] > s_pool_peak) s_pool_peak = s_vram_pool_used[wi];
    s_pool_updates++;
}

void vkr_upload_vram(VkRenderer* r, const uint16_t* data) {
    (void)r; (void)data;
    /* Deliberately nothing. This is not an omission.
     *
     * main.c calls this once per frame with the whole of gpu.vram.data, and on
     * the GL side that upload is recorded with update_display=false, which
     * writes *only* the R16UI mirror and never vram_tex. The mirror exists for
     * one case: a driver without ARB_texture_barrier, where the fragment shader
     * cannot sample the render target and falls back to it. Wherever the
     * barrier exists — every machine this runs on — nothing samples the mirror
     * and this upload changes no pixel.
     *
     * The Vulkan backend has no mirror, because it does not need one: the
     * feedback loop is a pipeline barrier, always available. So writing this
     * data into the one VRAM image is not the equivalent of what GL does, it is
     * the opposite of it — gpu.vram.data holds only CPU, DMA and MDEC writes,
     * never a rasterised pixel, so copying it over the image erases everything
     * the GPU drew, once per frame, for ever. That was the completely broken
     * picture on first light.
     *
     * The image is fed by vkr_upload_vram_rect() (GP0(A0), MDEC, DMA, savestate
     * restore) and by rasterisation, which is the whole of what belongs in it. */
}

void vkr_upload_vram_rect(VkRenderer* r, const uint16_t* data,
                          uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    if (!r->initialized || !data) return;
    /* Flush first. renderer_draw() both builds a batch and records its place in
     * the frame's op list, so a primitive that has been submitted but not
     * flushed has no place in the order yet and a VRAM op recorded meanwhile
     * would be replayed before it. That defect cost a session on the GL side
     * (Dino Crisis lost every uploaded picture under its own back-buffer
     * clear); it is not being reintroduced here. */
    VKR_FLUSH(r);
    vkr_record_vram_update(r, data, x, y, w, h);
}

void vkr_request_vram_readback(VkRenderer* r) {
    (void)r;
    SDL_SetAtomicInt(&s_readback_request, 1);
}

const uint16_t* vkr_get_vram_readback(uint32_t* seq_out) {
    if (seq_out) *seq_out = (uint32_t)SDL_GetAtomicInt(&s_readback_seq);
    return s_readback_vram[SDL_GetAtomicInt(&s_readback_pub)];
}

/* ------------------------------------------------------------------------- */
/* Render thread                                                              */
/* ------------------------------------------------------------------------- */

/* Synchronous rect readback, the GP0(C0)/GP0(80) path.
 *
 * The CPU thread cannot touch the queue — the render thread owns it — so it
 * parks the request here, wakes the render thread and blocks. Same shape as the
 * GL backend's sync readback, for the same reason. */
static SDL_AtomicInt  s_sync_rb_pending;
static SDL_Condition* s_sync_rb_done;
static uint16_t*      s_sync_rb_dest;      /* the whole CPU-side VRAM, written in place */
static uint16_t       s_sync_rb_x, s_sync_rb_y, s_sync_rb_w, s_sync_rb_h;
static int            s_sync_rb_slot;      /* the write slot whose pending ops must land first */
static bool           s_sync_rb_ok;

/* How much of the write slot's vertex and staging pools a synchronous readback
 * has already copied into the next frame's GPU buffers (vertex_buf/staging_buf
 * [frame_slot]), so a second readback in the same field copies only what was
 * recorded since. Valid for s_rb_staged_slot only, and forgotten whenever a
 * frame goes through (that frame rewrites those buffers). Render thread. */
static int            s_rb_staged_slot = -1;
static uint32_t       s_rb_staged_vtx, s_rb_staged_pool;


static void vkr_fill_ps1_push(const VkrBatch* b, int stp_mode, VkrPs1Push* out) {
    out->offset[0] = b->offset_x;
    out->offset[1] = b->offset_y;
    out->screen_scale[0] = b->screen_w * 0.5f;
    out->screen_scale[1] = b->screen_h * 0.5f;
    for (int i = 0; i < 4; i++) out->tex_window[i] = b->tex_window[i];
    out->use_texture   = b->textured    ? 1 : 0;
    out->raw_texture   = b->raw_texture ? 1 : 0;
    out->dither_enable = b->dither      ? 1 : 0;
    out->stp_mode      = stp_mode;
    out->set_mask      = b->set_mask    ? 1 : 0;
    out->mask_test     = b->mask_test   ? 1 : 0;
}

static void vkr_begin_vram_rendering(VkRenderer* r, VkCommandBuffer cmd, bool* active) {
    if (*active) return;
    VkRenderingAttachmentInfo att = {
        .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView   = r->vram.view,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
        .loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD,   /* VRAM persists; never cleared */
        .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,
    };
    VkRenderingInfo ri = {
        .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea           = { { 0, 0 }, { VKR_VRAM_W, VKR_VRAM_H } },
        .layerCount           = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments    = &att,
    };
    vkCmdBeginRendering(cmd, &ri);
    VkViewport vp = { 0.0f, 0.0f, (float)VKR_VRAM_W, (float)VKR_VRAM_H, 0.0f, 1.0f };
    vkCmdSetViewport(cmd, 0, 1, &vp);
    *active = true;
}

static void vkr_end_vram_rendering(VkCommandBuffer cmd, bool* active) {
    if (!*active) return;
    vkCmdEndRendering(cmd);
    *active = false;
}

static void vkr_exec_batch(VkRenderer* r, VkCommandBuffer cmd, const VkrBatch* b,
                           uint32_t slot, bool* rendering) {
    if (!b->vertex_count) return;
    vkr_begin_vram_rendering(r, cmd, rendering);

    VkRect2D sc = {
        { b->scissor[0], b->scissor[1] },
        { (uint32_t)b->scissor[2], (uint32_t)b->scissor[3] }
    };
    if (sc.extent.width == 0 || sc.extent.height == 0) return;
    vkCmdSetScissor(cmd, 0, 1, &sc);

    VkDeviceSize zero = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &r->vertex_buf[slot].buffer, &zero);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r->ps1_layout,
                            0, 1, &r->vram_set, 0, NULL);

    VkrBlend blend = VKR_BLEND_OFF;
    if (b->semi_trans) {
        switch (b->semi_trans_mode) {
            case 0:  blend = VKR_BLEND_HALF_HALF;   break;
            case 1:  blend = VKR_BLEND_ADD;         break;
            case 2:  blend = VKR_BLEND_SUBTRACT;    break;
            default: blend = VKR_BLEND_ADD_QUARTER; break;
        }
    }
    VkPipeline* set = b->is_line ? r->ps1_line : r->ps1_tri;
    VkrPs1Push push;

    if (b->semi_trans && b->textured) {
        /* Two passes, exactly as the GL backend does it: blend state is
         * per-draw but the STP bit is per-texel, so the opaque texels go down
         * with blending off and the semi-transparent ones with it on. A device
         * with dualSrcBlend could fold these together — every device here
         * reports it — but that is an optimisation to make once the two
         * backends have been proven to agree, not before. */
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, set[VKR_BLEND_OFF]);
        vkr_fill_ps1_push(b, 0, &push);
        vkCmdPushConstants(cmd, r->ps1_layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(push), &push);
        vkCmdDraw(cmd, b->vertex_count, 1, b->first_vertex, 0);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, set[blend]);
        float bc[4] = { 0.0f, 0.0f, 0.0f, vkr_blend_constant(blend) };
        vkCmdSetBlendConstants(cmd, bc);
        vkr_fill_ps1_push(b, 1, &push);
        vkCmdPushConstants(cmd, r->ps1_layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(push), &push);
        vkCmdDraw(cmd, b->vertex_count, 1, b->first_vertex, 0);
    } else {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, set[blend]);
        float bc[4] = { 0.0f, 0.0f, 0.0f, vkr_blend_constant(blend) };
        vkCmdSetBlendConstants(cmd, bc);
        vkr_fill_ps1_push(b, -1, &push);
        vkCmdPushConstants(cmd, r->ps1_layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(push), &push);
        vkCmdDraw(cmd, b->vertex_count, 1, b->first_vertex, 0);
    }
}

static void vkr_exec_vram_update(VkRenderer* r, VkCommandBuffer cmd, const VkrVramUpdate* u,
                                 uint32_t slot, bool* rendering) {
    /* A copy cannot happen inside a rendering scope, so close it — this is the
     * ordering the op list exists to preserve. */
    vkr_end_vram_rendering(cmd, rendering);

    /* And wait for what the batches before it did to the same image: their
     * colour writes (write-after-write) and their texture reads
     * (write-after-read), and an earlier copy to an overlapping rect. Without
     * this the transfer could land first and be drawn over, the Dino Crisis
     * symptom in a new place; the old replay had a barrier between batches but
     * none between the last batch and a copy. */
    {
        VkImageMemoryBarrier pre = {
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                   VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout           = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout           = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image               = r->vram.image,
            .subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        vkCmdPipelineBarrier(cmd,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, NULL, 0, NULL, 1, &pre);
    }

    VkBufferImageCopy c = {
        .bufferOffset      = u->data_offset,
        .bufferRowLength   = u->w,
        .bufferImageHeight = u->h,
        .imageSubresource  = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageOffset       = { u->x, u->y, 0 },
        .imageExtent       = { u->w, u->h, 1 },
    };
    vkCmdCopyBufferToImage(cmd, r->staging_buf[slot].buffer, r->vram.image,
                           VK_IMAGE_LAYOUT_GENERAL, 1, &c);

    VkImageMemoryBarrier b = {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask       = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .oldLayout           = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout           = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = r->vram.image,
        .subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         0, 0, NULL, 0, NULL, 1, &b);
}

/* Fullscreen pass into `target`, sampling VRAM. Used by both the scanout and
 * the VRAM viewer, which differ only in shader and push constants. */
static void vkr_fullscreen_pass(VkRenderer* r, VkCommandBuffer cmd, VkrImage* target,
                                VkPipeline pipe, VkPipelineLayout layout,
                                const void* push, uint32_t push_size,
                                uint32_t w, uint32_t h, bool clear_only) {
    vkr_image_barrier(cmd, target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

    VkRenderingAttachmentInfo att = {
        .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView   = target->view,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue  = { .color = { { 0.0f, 0.0f, 0.0f, 1.0f } } },
    };
    VkRenderingInfo ri = {
        .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea           = { { 0, 0 }, { w, h } },
        .layerCount           = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments    = &att,
    };
    vkCmdBeginRendering(cmd, &ri);
    if (!clear_only) {
        VkViewport vp = { 0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f };
        VkRect2D   sc = { { 0, 0 }, { w, h } };
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout,
                                0, 1, &r->vram_set, 0, NULL);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, push_size, push);
        vkCmdDraw(cmd, 3, 1, 0, 0);   /* attribute-less fullscreen triangle */
    }
    vkCmdEndRendering(cmd);

    vkr_image_barrier(cmd, target, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}

/* Replay state for one pass over a slot's ops (a frame, or the part of the
 * write slot a synchronous readback runs).
 *
 * The old replay ended rendering, issued the feedback barrier and began
 * rendering again between every two batches, whatever they did. Only a batch
 * that *reads* VRAM needs that barrier: a textured one, or one with the mask
 * test on (it reads the destination). A batch that only writes, or blends,
 * is ordered by the pipeline within one rendering scope, so consecutive
 * non-reading batches now share a scope, and a reading batch gets the barrier
 * only if something was written since the last one. VRAM update ops count as
 * writes. A replay starts as "written", conservatively. */
typedef struct {
    bool rendering;    /* inside vkCmdBeginRendering on the VRAM image */
    bool written;      /* colour writes may not be visible to texel fetches yet */
} VkrReplay;

static void vkr_replay_slot(VkRenderer* r, VkCommandBuffer cmd, int ri, uint32_t buf_slot,
                            VkrReplay* st) {
    VkrFrame* f = &s_frame[ri];
    for (uint32_t i = s_exec_from[ri]; i < f->op_count; i++) {
        if (f->ops[i].type == VKR_OP_VRAM_UPDATE) {
            vkr_exec_vram_update(r, cmd, &f->vram_updates[f->ops[i].index], buf_slot, &st->rendering);
            st->written = true;
        } else {
            const VkrBatch* b = &f->batches[f->ops[i].index];
            if ((b->textured || b->mask_test) && st->written) {
                /* The glTextureBarrier() equivalent: make earlier writes visible
                 * to this batch's sampler. A barrier cannot sit inside a
                 * dynamic rendering scope, so the scope closes first. */
                vkr_end_vram_rendering(cmd, &st->rendering);
                vkr_barrier_vram_feedback(cmd, &r->vram);
                st->written = false;
            }
            vkr_exec_batch(r, cmd, b, buf_slot, &st->rendering);
            if (b->vertex_count) st->written = true;
        }
    }
    s_exec_from[ri] = f->op_count;
}

/* Readback plumbing: the one command buffer and fence in VkRenderer.
 *
 * The command buffer opens with a global barrier against everything submitted
 * before it, so whatever earlier frames or replays wrote to the VRAM image is
 * complete and visible before this one reads or writes it. That is what the
 * vkQueueWaitIdle in front of the old readback was for, expressed as a
 * dependency instead of a full drain, and it also covers the previous frame's
 * scanout and ImGui reads of images this may touch. */
static void vkr_rb_begin(VkRenderer* r) {
    VkCommandBuffer cmd = r->rb_cmd;
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkBeginCommandBuffer(cmd, &bi);
    VkMemoryBarrier mb = {
        .sType         = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 1, &mb, 0, NULL, 0, NULL);
}

static bool vkr_rb_submit_wait(VkRenderer* r) {
    VkCommandBuffer cmd = r->rb_cmd;
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                        .commandBufferCount = 1, .pCommandBuffers = &cmd };
    if (vkQueueSubmit(r->ctx.queue, 1, &si, r->rb_fence) != VK_SUCCESS) return false;
    VkResult w = vkWaitForFences(r->ctx.device, 1, &r->rb_fence, VK_TRUE, UINT64_MAX);
    vkResetFences(r->ctx.device, 1, &r->rb_fence);
    return w == VK_SUCCESS;
}

/* Record the copy of a VRAM rect (or, when the rect wraps past the right or
 * bottom edge, the whole image, as the GL backend does) into readback_buf, with
 * the barriers on both sides: rendered and uploaded pixels before the transfer
 * read, and the transfer write before the host reads the mapped buffer. */
static void vkr_rb_record_copy(VkRenderer* r, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                               bool whole) {
    VkCommandBuffer cmd = r->rb_cmd;
    VkImageMemoryBarrier ib = {
        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout           = VK_IMAGE_LAYOUT_GENERAL,
        .newLayout           = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image               = r->vram.image,
        .subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    vkCmdPipelineBarrier(cmd,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
    VkBufferImageCopy c = {
        .bufferOffset      = 0,
        .bufferRowLength   = whole ? VKR_VRAM_W : w,
        .bufferImageHeight = whole ? VKR_VRAM_H : h,
        .imageSubresource  = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageOffset       = { whole ? 0 : x, whole ? 0 : y, 0 },
        .imageExtent       = { whole ? VKR_VRAM_W : w, whole ? VKR_VRAM_H : h, 1 },
    };
    vkCmdCopyImageToBuffer(cmd, r->vram.image, VK_IMAGE_LAYOUT_GENERAL,
                           r->readback_buf.buffer, 1, &c);
    VkBufferMemoryBarrier bb = {
        .sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask       = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer              = r->readback_buf.buffer,
        .offset              = 0,
        .size                = VK_WHOLE_SIZE,
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, NULL, 1, &bb, 0, NULL);
}

/* Back down to 5:5:5:1 into the CPU-side VRAM, in place: row (y+row) & 511,
 * column (x+col) & 1023, stride 1024, exactly where the caller's rect lives.
 *
 * The old version wrote the rect packed (row stride w) from the start of the
 * array it was given, and both callers (gpu_commands.c GP0(80h) and GP0(C0h))
 * pass the whole of gpu->vram.data: on Vulkan every such readback scribbled a
 * w-wide copy of the rect over the top-left of VRAM and left the rect itself
 * stale. The GL backend has always written in place. The +4 rounding the
 * expansion used is undone by the >>3, so the round-trip is exact. */
static void vkr_rb_unpack(const VkRenderer* r, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                          bool whole, uint16_t* vram) {
    const uint8_t* src = (const uint8_t*)r->readback_buf.mapped;
    for (uint32_t row = 0; row < h; row++) {
        const uint32_t vy = ((uint32_t)y + row) & (VKR_VRAM_H - 1);
        for (uint32_t col = 0; col < w; col++) {
            const uint32_t vx = ((uint32_t)x + col) & (VKR_VRAM_W - 1);
            const uint8_t* p = whole ? &src[((size_t)vy * VKR_VRAM_W + vx) * 4]
                                     : &src[((size_t)row * w + col) * 4];
            vram[(size_t)vy * VKR_VRAM_W + vx] =
                (uint16_t)((p[0] >> 3) | ((p[1] >> 3) << 5) | ((p[2] >> 3) << 10) |
                           ((p[3] >= 128) ? 0x8000 : 0));
        }
    }
}

/* The synchronous readback GP0(80h) and GP0(C0h) need: the CPU thread is
 * blocked in vkr_read_vram_rect() and no frame is pending.
 *
 * The rect has to include what the current field has drawn so far, and those
 * primitives are still unexecuted ops in the write slot. The old version copied
 * the image without them, so a game that draws and then copies or reads back
 * in the same field got the pixels from before the draw (the GL backend has
 * always replayed them; see glr_service_sync_readback). Here the slot's
 * outstanding ops are replayed into the image first, into the GPU buffers the
 * next frame will use: vertex_buf/staging_buf[frame_slot], free once that
 * slot's fence has signalled, and rewritten in full by that frame anyway. The
 * frame replay then continues from s_exec_from, so nothing runs twice. */
static bool vkr_service_sync_readback(VkRenderer* r, int wi, uint16_t x, uint16_t y,
                                      uint16_t w, uint16_t h, uint16_t* vram) {
    if (!w || !h || !vram || !r->rb_cmd || !r->readback_buf.mapped) return false;
    const uint32_t slot = r->frame_slot;
    const bool pending = s_exec_from[wi] < s_frame[wi].op_count;

    if (pending) {
        vkWaitForFences(r->ctx.device, 1, &r->fence[slot], VK_TRUE, UINT64_MAX);
        if (s_rb_staged_slot != wi) {
            s_rb_staged_slot = wi;
            s_rb_staged_vtx = s_rb_staged_pool = 0;
        }
        if (s_vram_pool_used[wi] > s_rb_staged_pool)
            memcpy((uint8_t*)r->staging_buf[slot].mapped + s_rb_staged_pool,
                   s_vram_pool[wi] + s_rb_staged_pool, s_vram_pool_used[wi] - s_rb_staged_pool);
        if (s_vtx_used[wi] > s_rb_staged_vtx)
            memcpy((VkrVertex*)r->vertex_buf[slot].mapped + s_rb_staged_vtx,
                   &s_vtx_pool[wi][s_rb_staged_vtx],
                   (size_t)(s_vtx_used[wi] - s_rb_staged_vtx) * sizeof(VkrVertex));
        s_rb_staged_pool = s_vram_pool_used[wi];
        s_rb_staged_vtx  = s_vtx_used[wi];
    }

    vkr_rb_begin(r);
    if (pending) {
        VkrReplay st = { false, true };
        vkr_replay_slot(r, r->rb_cmd, wi, slot, &st);
        vkr_end_vram_rendering(r->rb_cmd, &st.rendering);
    }
    const bool whole = (uint32_t)x + w > VKR_VRAM_W || (uint32_t)y + h > VKR_VRAM_H;
    vkr_rb_record_copy(r, x, y, w, h, whole);
    if (!vkr_rb_submit_wait(r)) return false;
    vkr_rb_unpack(r, x, y, w, h, whole, vram);
    return true;
}

/* The whole image, for the asynchronous inspector/Lua channel: no replay, the
 * frame that requested it has just been submitted in full. */
static bool vkr_readback_whole(VkRenderer* r, uint16_t* vram) {
    if (!r->rb_cmd || !r->readback_buf.mapped) return false;
    vkr_rb_begin(r);
    vkr_rb_record_copy(r, 0, 0, VKR_VRAM_W, VKR_VRAM_H, false);
    if (!vkr_rb_submit_wait(r)) return false;
    vkr_rb_unpack(r, 0, 0, VKR_VRAM_W, VKR_VRAM_H, false, vram);
    return true;
}


/* ZS1_DUMP_FRAME, byte-for-byte compatible with the GL backend's.
 *
 * Same source (the scanout image), same size (1024x512), same layout (packed
 * RGB, alpha dropped), so a dump from either backend can be compared with cmp.
 * That comparison is the whole parity check between the two renderers and there
 * is no other way to run it without a person looking at a screen. */
static void vkr_dump_scanout(VkRenderer* r, const char* path) {
    if (!r->rb_cmd || !r->readback_buf.mapped) return;
    vkr_rb_begin(r);
    VkCommandBuffer cmd = r->rb_cmd;
    /* The scanout image rests in SHADER_READ_ONLY_OPTIMAL between frames; a
     * transfer read needs it in TRANSFER_SRC and put back afterwards, or the
     * next frame's ImGui sampler sees the wrong layout. */
    vkr_image_barrier(cmd, &r->scanout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy c = {
        .bufferRowLength   = VKR_VRAM_W,
        .bufferImageHeight = VKR_VRAM_H,
        .imageSubresource  = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent       = { VKR_VRAM_W, VKR_VRAM_H, 1 },
    };
    vkCmdCopyImageToBuffer(cmd, r->scanout.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           r->readback_buf.buffer, 1, &c);
    vkr_image_barrier(cmd, &r->scanout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    if (!vkr_rb_submit_wait(r)) return;

    const uint8_t* src = (const uint8_t*)r->readback_buf.mapped;
    uint8_t* rgb = (uint8_t*)malloc((size_t)VKR_VRAM_W * VKR_VRAM_H * 3);
    if (!rgb) return;
    for (size_t i = 0; i < (size_t)VKR_VRAM_W * VKR_VRAM_H; i++) {
        rgb[i * 3 + 0] = src[i * 4 + 0];
        rgb[i * 3 + 1] = src[i * 4 + 1];
        rgb[i * 3 + 2] = src[i * 4 + 2];
    }
    FILE* f = fopen(path, "wb");
    if (f) { fwrite(rgb, 1, (size_t)VKR_VRAM_W * VKR_VRAM_H * 3, f); fclose(f); }
    free(rgb);
    LOG_RENDERER_INFO("[VK] dumped scanout to %s", path);
}

static int vkr_thread_main(void* userdata) {
    VkRenderer* r = (VkRenderer*)userdata;

    while (!SDL_GetAtomicInt(&r->gpu_stop)) {
        SDL_LockMutex(r->gpu_mutex);
        while (!r->frames_pending && !SDL_GetAtomicInt(&r->gpu_stop) &&
               !SDL_GetAtomicInt(&s_sync_rb_pending))
            SDL_WaitCondition(r->frame_ready, r->gpu_mutex);
        SDL_UnlockMutex(r->gpu_mutex);
        if (SDL_GetAtomicInt(&r->gpu_stop)) break;

        if (SDL_GetAtomicInt(&s_sync_rb_pending)) {
            s_sync_rb_ok = vkr_service_sync_readback(r, s_sync_rb_slot, s_sync_rb_x, s_sync_rb_y,
                                                     s_sync_rb_w, s_sync_rb_h, s_sync_rb_dest);
            SDL_SetAtomicInt(&s_sync_rb_pending, 0);
            SDL_LockMutex(r->gpu_mutex);
            SDL_SignalCondition(s_sync_rb_done);
            SDL_UnlockMutex(r->gpu_mutex);
            if (!r->frames_pending) continue;
        }

        int       ri   = 1 - r->write_idx;
        VkrFrame* f    = &s_frame[ri];
        uint32_t  slot = r->frame_slot;

        vkWaitForFences(r->ctx.device, 1, &r->fence[slot], VK_TRUE, UINT64_MAX);

        uint32_t image_index = 0;
        VkResult acq = vkAcquireNextImageKHR(r->ctx.device, r->ctx.swapchain, UINT64_MAX,
                                             r->acquire_sem[slot], VK_NULL_HANDLE, &image_index);
        if (acq == VK_ERROR_OUT_OF_DATE_KHR || acq == VK_SUBOPTIMAL_KHR) {
            /* The window changed size under us. Rebuild and skip this frame —
             * dropping one field is invisible, and the alternative is
             * presenting to a swapchain that no longer matches the surface. */
            vkDeviceWaitIdle(r->ctx.device);
            vk_swapchain_destroy(&r->ctx);
            vk_swapchain_create(&r->ctx, r->sdl_window);
            s_rb_staged_slot = -1;
            SDL_LockMutex(r->gpu_mutex);
            r->frames_pending = 0;
            SDL_SignalCondition(r->frame_done);
            SDL_UnlockMutex(r->gpu_mutex);
            continue;
        }
        vkResetFences(r->ctx.device, 1, &r->fence[slot]);

        /* Hand the frame's staged bytes to the GPU-visible buffers. Both are
         * host-coherent and permanently mapped, so this is a memcpy and no
         * flush. */
        memcpy(r->staging_buf[slot].mapped, s_vram_pool[ri], s_vram_pool_used[ri]);
        memcpy(r->vertex_buf[slot].mapped, s_vtx_pool[ri],
               (size_t)s_vtx_used[ri] * sizeof(VkrVertex));

        VkCommandBuffer cmd = r->cmd[slot];
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
        vkBeginCommandBuffer(cmd, &bi);

        /* Replay in submission order, from wherever a synchronous readback
         * left the slot. The op list exists precisely so a texture page
         * uploaded halfway through a frame is only visible to the draws that
         * were actually issued after it. */
        VkrReplay st = { false, true };
        vkr_replay_slot(r, cmd, ri, slot, &st);
        vkr_end_vram_rendering(cmd, &st.rendering);
        vkr_barrier_vram_feedback(cmd, &r->vram);

        /* Scanout. A blanked display is a cleared image, not a stale one:
         * GP1(03) shows black on hardware (DOCS/graphicsprocessingunitgpu.md:647). */
        {
            uint32_t dw = f->disp_w ? f->disp_w : 320;
            uint32_t dh = f->disp_h ? f->disp_h : 240;
            VkrScanoutPush push = {
                .disp_off  = { f->disp_x, f->disp_y },
                .disp_size = { (int32_t)dw, (int32_t)dh },
                .depth24   = f->disp_depth24 ? 1 : 0,
            };
            vkr_fullscreen_pass(r, cmd, &r->scanout, r->scanout_pipe, r->scanout_layout,
                                &push, sizeof(push), dw, dh, f->disp_blank);
        }

        /* VRAM viewer, for the debug workspace: only while it is on screen
         * (view.enabled, from debug_ui.cpp). A 1024x512 fullscreen pass every
         * field otherwise, the gameplay shell included. Skipping it leaves the
         * viewer image in SHADER_READ_ONLY_OPTIMAL, where ImGui expects it. */
        if (f->view.enabled) {
            VkrViewerPush push = {
                .clut  = { f->view.clut_x, f->view.clut_y },
                .flags = { f->view.greyscale ? 1 : 0, f->view.show_alpha ? 1 : 0, f->view.shift24 },
                .mode  = (int32_t)f->view.mode,
            };
            vkr_fullscreen_pass(r, cmd, &r->viewer, r->viewer_pipe, r->viewer_layout,
                                &push, sizeof(push), VKR_VRAM_W, VKR_VRAM_H, false);
        }

        /* The window: clear, then ImGui — which is what draws the PS1 picture,
         * as an ImGui::Image of the scanout. There is no dedicated present
         * pass, exactly as in the GL backend. */
        {
            VkImageMemoryBarrier to_attach = {
                .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .srcAccessMask       = 0,
                .dstAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                .oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image               = r->ctx.swap_images[image_index],
                .subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
            };
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 0, 0, NULL, 0, NULL, 1, &to_attach);

            VkRenderingAttachmentInfo att = {
                .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                .imageView   = r->ctx.swap_views[image_index],
                .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR,
                .storeOp     = VK_ATTACHMENT_STORE_OP_STORE,
                /* The same 0.1 grey the GL backend clears the default
                 * framebuffer to, so the two look identical outside the image. */
                .clearValue  = { .color = { { 0.1f, 0.1f, 0.1f, 1.0f } } },
            };
            VkRenderingInfo rinf = {
                .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
                .renderArea           = { { 0, 0 }, r->ctx.swap_extent },
                .layerCount           = 1,
                .colorAttachmentCount = 1,
                .pColorAttachments    = &att,
            };
            vkCmdBeginRendering(cmd, &rinf);
            vk_imgui_new_frame();
            vk_imgui_render(f->imgui_draw_data, cmd);
            vkCmdEndRendering(cmd);

            VkImageMemoryBarrier to_present = to_attach;
            to_present.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            to_present.dstAccessMask = 0;
            to_present.oldLayout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            to_present.newLayout     = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                 0, 0, NULL, 0, NULL, 1, &to_present);
        }

        vkEndCommandBuffer(cmd);

        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si = {
            .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount   = 1,
            .pWaitSemaphores      = &r->acquire_sem[slot],
            .pWaitDstStageMask    = &wait_stage,
            .commandBufferCount   = 1,
            .pCommandBuffers      = &cmd,
            .signalSemaphoreCount = 1,
            .pSignalSemaphores    = &r->release_sem[image_index],
        };
        vkQueueSubmit(r->ctx.queue, 1, &si, r->fence[slot]);

        VkPresentInfoKHR pi = {
            .sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores    = &r->release_sem[image_index],
            .swapchainCount     = 1,
            .pSwapchains        = &r->ctx.swapchain,
            .pImageIndices      = &image_index,
        };
        VkResult pres = vkQueuePresentKHR(r->ctx.queue, &pi);
        if (pres == VK_ERROR_OUT_OF_DATE_KHR || pres == VK_SUBOPTIMAL_KHR) {
            vkDeviceWaitIdle(r->ctx.device);
            vk_swapchain_destroy(&r->ctx);
            vk_swapchain_create(&r->ctx, r->sdl_window);
        }

        {
            static int s_dump_counter = 0;
            const char* dump_path = getenv("ZS1_DUMP_FRAME");
            if (dump_path) {
                int target = 300;
                const char* t = getenv("ZS1_DUMP_FRAME_N");
                if (t) target = atoi(t);
                if (s_dump_counter == target) { vkQueueWaitIdle(r->ctx.queue); vkr_dump_scanout(r, dump_path); }
                s_dump_counter++;
            }
        }

        /* The async whole-VRAM readback the inspector's CPU-vs-GPU diff uses. */
        if (SDL_GetAtomicInt(&s_readback_request)) {
            const int back = 1 - SDL_GetAtomicInt(&s_readback_pub);
            if (vkr_readback_whole(r, s_readback_vram[back])) {
                SDL_SetAtomicInt(&s_readback_pub, back);
                SDL_AddAtomicInt(&s_readback_seq, 1);
            }
            SDL_SetAtomicInt(&s_readback_request, 0);
        }

        r->frame_slot = (slot + 1) % VK_FRAMES_IN_FLIGHT;

        f->batch_count = f->vram_update_count = f->op_count = 0;
        f->imgui_draw_data = NULL;
        s_vtx_used[ri] = 0;
        s_vram_pool_used[ri] = 0;
        s_exec_from[ri] = 0;
        s_rb_staged_slot = -1;   /* this frame rewrote the buffers a readback staged into */

        SDL_LockMutex(r->gpu_mutex);
        r->frames_pending = 0;
        SDL_SignalCondition(r->frame_done);
        SDL_UnlockMutex(r->gpu_mutex);
    }
    LOG_RENDERER_INFO("[VK] render thread exiting");
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Thread control and the remaining public surface                            */
/* ------------------------------------------------------------------------- */

void vkr_start_gpu_thread(VkRenderer* r, SDL_Window* window) {
    r->sdl_window   = window;
    r->gpu_mutex    = SDL_CreateMutex();
    r->frame_ready  = SDL_CreateCondition();
    r->frame_done   = SDL_CreateCondition();
    s_sync_rb_done  = SDL_CreateCondition();
    SDL_SetAtomicInt(&r->gpu_stop, 0);
    r->write_idx = 0;
    r->frames_pending = 0;
    s_exec_from[0] = s_exec_from[1] = 0;
    s_rb_staged_slot = -1;
    r->gpu_thread = SDL_CreateThread(vkr_thread_main, "GPU-VK", r);
    if (!r->gpu_thread) LOG_RENDERER_ERROR("[VK] SDL_CreateThread: %s", SDL_GetError());
}

void vkr_stop_gpu_thread(VkRenderer* r) {
    if (!r->gpu_thread) return;
    SDL_SetAtomicInt(&r->gpu_stop, 1);
    SDL_LockMutex(r->gpu_mutex);
    SDL_SignalCondition(r->frame_ready);
    SDL_UnlockMutex(r->gpu_mutex);
    SDL_WaitThread(r->gpu_thread, NULL);
    r->gpu_thread = NULL;
    if (s_sync_rb_done) { SDL_DestroyCondition(s_sync_rb_done); s_sync_rb_done = NULL; }
    if (r->frame_ready) { SDL_DestroyCondition(r->frame_ready); r->frame_ready = NULL; }
    if (r->frame_done)  { SDL_DestroyCondition(r->frame_done);  r->frame_done = NULL; }
    if (r->gpu_mutex)   { SDL_DestroyMutex(r->gpu_mutex);       r->gpu_mutex = NULL; }
}

void vkr_submit_frame(VkRenderer* r, void* imgui_draw_data) {
    if (!r->initialized || !r->gpu_thread) return;
    VKR_FLUSH(r);

    SDL_LockMutex(r->gpu_mutex);
    while (r->frames_pending > 0) SDL_WaitCondition(r->frame_done, r->gpu_mutex);

    VkrFrame* f = &s_frame[r->write_idx];
    f->imgui_draw_data = imgui_draw_data;
    f->disp_x = r->display_x; f->disp_y = r->display_y;
    f->disp_w = r->display_w; f->disp_h = r->display_h;
    f->disp_depth24 = r->display_depth24;
    f->disp_blank   = r->display_blank;
    f->view         = r->vram_view;

    r->write_idx      = 1 - r->write_idx;
    r->frames_pending = 1;
    SDL_SignalCondition(r->frame_ready);
    SDL_UnlockMutex(r->gpu_mutex);
}

void vkr_wait_frame_done(VkRenderer* r) {
    if (!r->initialized || !r->gpu_thread) return;
    SDL_LockMutex(r->gpu_mutex);
    while (r->frames_pending > 0) SDL_WaitCondition(r->frame_done, r->gpu_mutex);
    SDL_UnlockMutex(r->gpu_mutex);
}

bool vkr_read_vram_rect(VkRenderer* r, uint16_t* out,
                        uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    if (!r->initialized || !r->gpu_thread || !out || !w || !h) return false;
    /* The pending batch has to be in the frame before its pixels can be read
     * back, or GP0(C0) returns the state from before the draw it was issued
     * after. */
    VKR_FLUSH(r);

    SDL_LockMutex(r->gpu_mutex);
    /* The previous frame first. The render thread looks at a parked readback
     * before it looks at a pending frame, so a request that arrives while a
     * frame is still pending would be served from a VRAM image that does not
     * yet hold that frame. Before main.c started emulating while the previous
     * frame renders this could not happen (the frame was always done by the
     * time the machine ran); now it can, and the GL backend has always waited
     * here for the same reason. */
    while (r->frames_pending > 0) SDL_WaitCondition(r->frame_done, r->gpu_mutex);
    s_sync_rb_slot = r->write_idx;
    s_sync_rb_dest = out;
    s_sync_rb_x = x; s_sync_rb_y = y; s_sync_rb_w = w; s_sync_rb_h = h;
    s_sync_rb_ok = false;
    SDL_SetAtomicInt(&s_sync_rb_pending, 1);
    SDL_SignalCondition(r->frame_ready);
    while (SDL_GetAtomicInt(&s_sync_rb_pending))
        SDL_WaitCondition(s_sync_rb_done, r->gpu_mutex);
    bool ok = s_sync_rb_ok;
    SDL_UnlockMutex(r->gpu_mutex);
    return ok;
}

/* The three handles ImGui draws with are descriptor sets, created once the
 * ImGui Vulkan backend is up. Made lazily because the images exist before it
 * does. */
static GfxTexHandle vkr_imgui_handle(VkRenderer* r, VkDescriptorSet* cache, VkrImage* im) {
    if (!r->initialized) return 0;
    if (*cache == VK_NULL_HANDLE)
        *cache = vk_imgui_add_texture(r->sampler, im->view,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return (GfxTexHandle)(uintptr_t)*cache;
}

GfxTexHandle vkr_get_scanout_texture(VkRenderer* r) {
    return vkr_imgui_handle(r, &r->imgui_scanout_set, &r->scanout);
}
GfxTexHandle vkr_get_vram_viewer_texture(VkRenderer* r) {
    return vkr_imgui_handle(r, &r->imgui_viewer_set, &r->viewer);
}
GfxTexHandle vkr_get_display_texture(VkRenderer* r) {
    /* The VRAM image is in GENERAL, never SHADER_READ_ONLY_OPTIMAL, because it
     * is permanently a colour attachment as well. */
    if (!r->initialized) return 0;
    if (r->imgui_vram_set == VK_NULL_HANDLE)
        r->imgui_vram_set = vk_imgui_add_texture(r->sampler, r->vram.view,
                                                 VK_IMAGE_LAYOUT_GENERAL);
    return (GfxTexHandle)(uintptr_t)r->imgui_vram_set;
}

/* ------------------------------------------------------------------------- */
/* The GfxBackend vtable                                                      */
/* ------------------------------------------------------------------------- */

/* One live backend, one file-static instance — the same arrangement the GL
 * backend uses, and for the same reason: the frame queue above is file-static
 * too, so a second instance was never possible. */
static VkRenderer s_vk_renderer;

static VkRenderer* R(GfxImpl impl) {
    return impl ? (VkRenderer*)impl : &s_vk_renderer;
}

static bool vkvt_init(GfxImpl* impl, SDL_Window* window, const GfxDeviceRequest* req) {
    if (!vkr_init(&s_vk_renderer, window, req)) return false;
    *impl = &s_vk_renderer;

    /* ImGui's Vulkan backend, brought up here because it needs the device this
     * function just created. The ImGui *context* already exists — debug_ui_init
     * runs before this — which is exactly the split a hot backend switch needs:
     * the context survives, only the GPU-side half is rebuilt. */
    if (!vk_imgui_init(s_vk_renderer.ctx.instance, s_vk_renderer.ctx.phys,
                       s_vk_renderer.ctx.device, s_vk_renderer.ctx.queue_family,
                       s_vk_renderer.ctx.queue, s_vk_renderer.desc_pool,
                       2, s_vk_renderer.ctx.swap_image_count,
                       s_vk_renderer.ctx.swap_format,
                       VK_API_VERSION_1_3)) {
        vkr_destroy(&s_vk_renderer);
        return false;
    }
    return true;
}

static void vkvt_destroy(GfxImpl impl) {
    vk_imgui_shutdown();
    vkr_destroy(R(impl));
}

static int vkvt_enumerate_devices(GfxDeviceInfo* out, int max) {
    return vk_device_enumerate(out, max);
}

static void vkvt_start_thread(GfxImpl impl, SDL_Window* window, void* native_ctx) {
    (void)native_ctx;   /* Vulkan has no context to hand over; the device is ours */
    vkr_start_gpu_thread(R(impl), window);
}
static void vkvt_stop_thread(GfxImpl impl)            { vkr_stop_gpu_thread(R(impl)); }
static void vkvt_submit_frame(GfxImpl impl, void* dd) { vkr_submit_frame(R(impl), dd); }
static void vkvt_wait_frame_done(GfxImpl impl)        { vkr_wait_frame_done(R(impl)); }

static void vkvt_push_triangle(GfxImpl impl, RendererPosition p[3], RendererColor c[3],
                               RendererTexCoord t[3], uint16_t clut, uint16_t tpage) {
    vkr_push_triangle(R(impl), p, c, t, clut, tpage);
}
static void vkvt_push_quad(GfxImpl impl, RendererPosition p[4], RendererColor c[4],
                           RendererTexCoord t[4], uint16_t clut, uint16_t tpage) {
    vkr_push_quad(R(impl), p, c, t, clut, tpage);
}
static void vkvt_push_line(GfxImpl impl, RendererPosition p[2], RendererColor c[2]) {
    vkr_push_line(R(impl), p, c);
}
static void vkvt_draw(GfxImpl impl)    { vkr_draw(R(impl)); }
static void vkvt_display(GfxImpl impl) { (void)impl; }   /* scanout is a frame-loop pass */
static void vkvt_blit_vram(GfxImpl impl, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    (void)impl; (void)x; (void)y; (void)w; (void)h;   /* dead in the GL path too */
}

static void vkvt_set_texture_mode(GfxImpl impl, bool e)     { vkr_set_texture_mode(R(impl), e); }
static void vkvt_set_raw_texture_mode(GfxImpl impl, bool e) { vkr_set_raw_texture_mode(R(impl), e); }
static void vkvt_set_screen_scale(GfxImpl impl, uint16_t w, uint16_t h) {
    vkr_set_screen_scale(R(impl), w, h);
}
static void vkvt_set_texture_window(GfxImpl impl, uint8_t mx, uint8_t my, uint8_t ox, uint8_t oy) {
    vkr_set_texture_window(R(impl), mx, my, ox, oy);
}
static void vkvt_set_draw_offset(GfxImpl impl, int16_t x, int16_t y) {
    vkr_set_draw_offset(R(impl), x, y);
}
static void vkvt_set_drawing_area(GfxImpl impl, uint16_t l, uint16_t t, uint16_t rt, uint16_t b) {
    vkr_set_drawing_area(R(impl), l, t, rt, b);
}
static void vkvt_set_semi_trans_mode(GfxImpl impl, bool e, uint8_t m) {
    vkr_set_semi_trans_mode(R(impl), e, m);
}
static void vkvt_set_dither_mode(GfxImpl impl, bool e) { vkr_set_dither_mode(R(impl), e); }
static void vkvt_set_mask_mode(GfxImpl impl, bool e)   { vkr_set_mask_mode(R(impl), e); }
static void vkvt_set_mask_test(GfxImpl impl, bool e)   { vkr_set_mask_test(R(impl), e); }

static void vkvt_upload_vram(GfxImpl impl, const uint16_t* d) { vkr_upload_vram(R(impl), d); }
static void vkvt_upload_vram_rect(GfxImpl impl, const uint16_t* d,
                                  uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    vkr_upload_vram_rect(R(impl), d, x, y, w, h);
}
static bool vkvt_read_vram_rect(GfxImpl impl, uint16_t* out,
                                uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    return vkr_read_vram_rect(R(impl), out, x, y, w, h);
}
static void vkvt_request_vram_readback(GfxImpl impl) { vkr_request_vram_readback(R(impl)); }

static void vkvt_set_display_region(GfxImpl impl, uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    vkr_set_display_region(R(impl), x, y, w, h);
}
static void vkvt_set_display_depth24(GfxImpl impl, bool d) { vkr_set_display_depth24(R(impl), d); }
static void vkvt_set_display_blank(GfxImpl impl, bool b)   { vkr_set_display_blank(R(impl), b); }
static void vkvt_set_vram_view_params(GfxImpl impl, const VramViewParams* p) {
    vkr_set_vram_view_params(R(impl), p);
}
static void vkvt_update_vram_viewer(GfxImpl impl, const uint8_t* b) {
    vkr_update_vram_viewer(R(impl), b);
}
static void vkvt_get_pool_stats(GfxImpl impl, uint32_t* used, uint32_t* peak,
                                uint32_t* updates, uint32_t* skips) {
    vkr_get_pool_stats(R(impl), used, peak, updates, skips);
}
static GfxTexHandle vkvt_get_display_texture(GfxImpl impl)     { return vkr_get_display_texture(R(impl)); }
static GfxTexHandle vkvt_get_scanout_texture(GfxImpl impl)     { return vkr_get_scanout_texture(R(impl)); }
static GfxTexHandle vkvt_get_vram_viewer_texture(GfxImpl impl) { return vkr_get_vram_viewer_texture(R(impl)); }

const GfxBackend gfx_backend_vulkan = {
    .name                    = "Vulkan 1.3",
    .type                    = GFX_BACKEND_VULKAN,
    .init                    = vkvt_init,
    .destroy                 = vkvt_destroy,
    .enumerate_devices       = vkvt_enumerate_devices,
    .start_thread            = vkvt_start_thread,
    .stop_thread             = vkvt_stop_thread,
    .submit_frame            = vkvt_submit_frame,
    .wait_frame_done         = vkvt_wait_frame_done,
    .push_triangle           = vkvt_push_triangle,
    .push_quad               = vkvt_push_quad,
    .push_line               = vkvt_push_line,
    .draw                    = vkvt_draw,
    .display                 = vkvt_display,
    .blit_vram               = vkvt_blit_vram,
    .set_texture_mode        = vkvt_set_texture_mode,
    .set_raw_texture_mode    = vkvt_set_raw_texture_mode,
    .set_screen_scale        = vkvt_set_screen_scale,
    .set_texture_window      = vkvt_set_texture_window,
    .set_draw_offset         = vkvt_set_draw_offset,
    .set_drawing_area        = vkvt_set_drawing_area,
    .set_semi_trans_mode     = vkvt_set_semi_trans_mode,
    .set_dither_mode         = vkvt_set_dither_mode,
    .set_mask_mode           = vkvt_set_mask_mode,
    .set_mask_test           = vkvt_set_mask_test,
    .upload_vram             = vkvt_upload_vram,
    .upload_vram_rect        = vkvt_upload_vram_rect,
    .read_vram_rect          = vkvt_read_vram_rect,
    .request_vram_readback   = vkvt_request_vram_readback,
    .get_vram_readback       = vkr_get_vram_readback,
    .set_display_region      = vkvt_set_display_region,
    .set_display_depth24     = vkvt_set_display_depth24,
    .set_display_blank       = vkvt_set_display_blank,
    .set_vram_view_params    = vkvt_set_vram_view_params,
    .update_vram_viewer      = vkvt_update_vram_viewer,
    .get_pool_stats          = vkvt_get_pool_stats,
    .get_display_texture     = vkvt_get_display_texture,
    .get_scanout_texture     = vkvt_get_scanout_texture,
    .get_vram_viewer_texture = vkvt_get_vram_viewer_texture,
};
