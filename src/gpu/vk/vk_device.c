/* SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 ZioZoni95
 *
 * Part of ZoniStation One, a PlayStation 1 emulator.
 * See LICENSE for the full licence text and THIRD-PARTY.md for the
 * components of this project that have other authors.
 */

#include "vk_device.h"
#include "log.h"
#include "host_info.h"

#include <SDL3/SDL_vulkan.h>
#include <string.h>
#include <stdlib.h>

#define VK_CHECK(expr, what)                                                    \
    do {                                                                        \
        VkResult _r = (expr);                                                   \
        if (_r != VK_SUCCESS) {                                                 \
            LOG_RENDERER_ERROR("[VK] %s: %s", (what), vk_result_str(_r));       \
            return false;                                                       \
        }                                                                       \
    } while (0)

/* ------------------------------------------------------------------------- */
/* Instance                                                                   */
/* ------------------------------------------------------------------------- */

static bool create_instance(VkInstance* out, bool want_validation, bool* validation_on) {
    Uint32 n_sdl_ext = 0;
    const char* const* sdl_ext = SDL_Vulkan_GetInstanceExtensions(&n_sdl_ext);
    if (!sdl_ext) {
        LOG_RENDERER_ERROR("[VK] SDL_Vulkan_GetInstanceExtensions: %s", SDL_GetError());
        return false;
    }

    const char* layers[1] = { "VK_LAYER_KHRONOS_validation" };
    uint32_t n_layers = 0;
    if (want_validation) {
        uint32_t n_avail = 0;
        vkEnumerateInstanceLayerProperties(&n_avail, NULL);
        VkLayerProperties* avail = (VkLayerProperties*)calloc(n_avail ? n_avail : 1, sizeof(*avail));
        if (avail) {
            vkEnumerateInstanceLayerProperties(&n_avail, avail);
            for (uint32_t i = 0; i < n_avail; i++)
                if (!strcmp(avail[i].layerName, layers[0])) { n_layers = 1; break; }
            free(avail);
        }
        if (!n_layers)
            LOG_RENDERER_WARN("[VK] ZS1_VK_VALIDATE set but the validation layer is not installed");
    }
    *validation_on = (n_layers == 1);

    VkApplicationInfo app = {
        .sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName   = "ZoniStation One",
        .applicationVersion = VK_MAKE_VERSION(1, 0, 0),
        .pEngineName        = "ZoniStation One",
        .engineVersion      = VK_MAKE_VERSION(1, 0, 0),
        /* 1.3 for dynamic rendering and synchronization2 in core. */
        .apiVersion         = VK_API_VERSION_1_3,
    };
    VkInstanceCreateInfo ci = {
        .sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo        = &app,
        .enabledExtensionCount   = n_sdl_ext,
        .ppEnabledExtensionNames = sdl_ext,
        .enabledLayerCount       = n_layers,
        .ppEnabledLayerNames     = n_layers ? layers : NULL,
    };
    VK_CHECK(vkCreateInstance(&ci, NULL, out), "vkCreateInstance");
    return true;
}

/* ------------------------------------------------------------------------- */
/* Physical device                                                            */
/* ------------------------------------------------------------------------- */

/* A device is usable only if one queue family can both render and present to
 * this surface. Keeping the two on one family avoids the ownership transfers a
 * split setup needs, and every driver worth supporting offers such a family —
 * on this machine the discrete card, the iGPU and llvmpipe all report family 0
 * doing both, and that includes the iGPU in dGPU-only mode. */
static bool find_queue_family(VkPhysicalDevice pd, VkSurfaceKHR surface, uint32_t* out_family) {
    uint32_t n = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &n, NULL);
    if (!n) return false;
    VkQueueFamilyProperties* qf = (VkQueueFamilyProperties*)calloc(n, sizeof(*qf));
    if (!qf) return false;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &n, qf);

    bool found = false;
    for (uint32_t i = 0; i < n && !found; i++) {
        if (!(qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
        VkBool32 present = VK_FALSE;
        if (surface != VK_NULL_HANDLE)
            vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surface, &present);
        else
            present = VK_TRUE;
        if (present) { *out_family = i; found = true; }
    }
    free(qf);
    return found;
}

static bool has_device_extension(VkPhysicalDevice pd, const char* name) {
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &n, NULL);
    if (!n) return false;
    VkExtensionProperties* ext = (VkExtensionProperties*)calloc(n, sizeof(*ext));
    if (!ext) return false;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &n, ext);
    bool found = false;
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(ext[i].extensionName, name)) { found = true; break; }
    free(ext);
    return found;
}

static void query_caps(VkPhysicalDevice pd, VkCaps* caps) {
    memset(caps, 0, sizeof(*caps));

    VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT interlock = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT,
    };
    VkPhysicalDeviceFeatures2 f2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &interlock,
    };
    vkGetPhysicalDeviceFeatures2(pd, &f2);

    caps->dual_src_blend = f2.features.dualSrcBlend == VK_TRUE;
    caps->pixel_interlock = interlock.fragmentShaderPixelInterlock == VK_TRUE &&
                            has_device_extension(pd, VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME);
    caps->raster_order_access =
        has_device_extension(pd, VK_EXT_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME);
}

/* Preference order when nothing was asked for: discrete, then integrated, then
 * anything else. llvmpipe is a real device and will be listed, but picking a
 * software rasteriser by default would look exactly like the emulator being
 * slow, which is a diagnosis this project has already paid for twice. */
static int device_rank(VkPhysicalDeviceType t) {
    switch (t) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return 0;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 1;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return 2;
        default:                                     return 3;
    }
}

int vk_device_enumerate(GfxDeviceInfo* out, int max) {
    if (!out || max <= 0) return 0;
    if (!vk_loader_init()) return 0;

    VkInstance inst = VK_NULL_HANDLE;
    bool validation = false;
    if (!create_instance(&inst, false, &validation)) return 0;
    if (!vk_loader_load_instance(inst)) { vkDestroyInstance(inst, NULL); return 0; }

    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, NULL);
    VkPhysicalDevice* pds = n ? (VkPhysicalDevice*)calloc(n, sizeof(*pds)) : NULL;
    if (pds) vkEnumeratePhysicalDevices(inst, &n, pds);

    int written = 0;
    for (uint32_t i = 0; i < n && written < max; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pds[i], &p);
        uint32_t fam;
        /* No surface here: this runs before a window may exist. A device that
         * cannot present is filtered out at create time, not listed away. */
        if (!find_queue_family(pds[i], VK_NULL_HANDLE, &fam)) continue;
        snprintf(out[written].name, sizeof(out[written].name), "%s", p.deviceName);
        out[written].live_switchable = true;   /* a Vulkan device swap keeps the window */
        written++;
    }
    free(pds);
    vkDestroyInstance(inst, NULL);
    return written;
}

/* ------------------------------------------------------------------------- */
/* Context                                                                    */
/* ------------------------------------------------------------------------- */

static bool pick_physical_device(VkContext* c, int wanted) {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(c->instance, &n, NULL);
    if (!n) { LOG_RENDERER_ERROR("[VK] no physical devices"); return false; }
    VkPhysicalDevice* pds = (VkPhysicalDevice*)calloc(n, sizeof(*pds));
    if (!pds) return false;
    vkEnumeratePhysicalDevices(c->instance, &n, pds);

    int  best = -1, best_rank = 99, index = 0;
    uint32_t best_family = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t fam;
        if (!find_queue_family(pds[i], c->surface, &fam)) continue;
        if (!has_device_extension(pds[i], VK_KHR_SWAPCHAIN_EXTENSION_NAME)) continue;

        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pds[i], &p);

        if (wanted >= 0) {
            if (index == wanted) { best = (int)i; best_family = fam; break; }
        } else {
            int r = device_rank(p.deviceType);
            if (r < best_rank) { best_rank = r; best = (int)i; best_family = fam; }
        }
        index++;
    }
    if (best < 0) {
        LOG_RENDERER_ERROR("[VK] no device can render and present to this surface");
        free(pds);
        return false;
    }

    c->phys         = pds[best];
    c->queue_family = best_family;
    free(pds);

    VkPhysicalDeviceDriverProperties drv = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
    VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &drv };
    vkGetPhysicalDeviceProperties2(c->phys, &p2);
    c->api_version = p2.properties.apiVersion;
    snprintf(c->device_name, sizeof(c->device_name), "%s", p2.properties.deviceName);
    snprintf(c->driver_name, sizeof(c->driver_name), "%s", drv.driverName);

    vkGetPhysicalDeviceMemoryProperties(c->phys, &c->mem_props);
    query_caps(c->phys, &c->caps);

    /* Say which GPU actually got the device, the same way the GL path says which
     * one got the context. On a hybrid machine that line is the first thing to
     * read before calling a rendering difference an emulator bug. */
    LOG_RENDERER_INFO("[VK] device: %s (%s, API %u.%u.%u)", c->device_name, c->driver_name,
                      VK_API_VERSION_MAJOR(c->api_version),
                      VK_API_VERSION_MINOR(c->api_version),
                      VK_API_VERSION_PATCH(c->api_version));
    LOG_RENDERER_INFO("[VK] caps: pixel_interlock=%d dual_src_blend=%d raster_order=%d",
                      c->caps.pixel_interlock, c->caps.dual_src_blend, c->caps.raster_order_access);
    return true;
}

static bool create_logical_device(VkContext* c) {
    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = c->queue_family,
        .queueCount       = 1,
        .pQueuePriorities = &prio,
    };

    const char* exts[4];
    uint32_t n_ext = 0;
    exts[n_ext++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    /* Dynamic rendering is 1.3 core, but ImGui's Vulkan backend requires the
     * extension to be enabled explicitly before it will use it — its own header
     * says so — so it is asked for by name where the driver offers it. */
    if (has_device_extension(c->phys, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME))
        exts[n_ext++] = VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME;
    if (c->caps.pixel_interlock)
        exts[n_ext++] = VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME;

    VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT interlock = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT,
        .fragmentShaderPixelInterlock = c->caps.pixel_interlock ? VK_TRUE : VK_FALSE,
    };
    /* Dynamic rendering and synchronization2 are 1.3 core, but still have to be
     * switched on explicitly through the feature chain. */
    VkPhysicalDeviceVulkan13Features v13 = {
        .sType            = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .pNext            = c->caps.pixel_interlock ? &interlock : NULL,
        .dynamicRendering = VK_TRUE,
        .synchronization2 = VK_TRUE,
    };
    VkPhysicalDeviceFeatures2 f2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &v13,
    };
    f2.features.dualSrcBlend = c->caps.dual_src_blend ? VK_TRUE : VK_FALSE;

    VkDeviceCreateInfo dci = {
        .sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext                   = &f2,
        .queueCreateInfoCount    = 1,
        .pQueueCreateInfos       = &qci,
        .enabledExtensionCount   = n_ext,
        .ppEnabledExtensionNames = exts,
    };
    VK_CHECK(vkCreateDevice(c->phys, &dci, NULL, &c->device), "vkCreateDevice");
    if (!vk_loader_load_device(c->device)) return false;
    vkGetDeviceQueue(c->device, c->queue_family, 0, &c->queue);
    return true;
}

uint32_t vk_find_memory_type(const VkContext* c, uint32_t type_bits, VkMemoryPropertyFlags props) {
    for (uint32_t i = 0; i < c->mem_props.memoryTypeCount; i++) {
        if (!(type_bits & (1u << i))) continue;
        if ((c->mem_props.memoryTypes[i].propertyFlags & props) == props) return i;
    }
    return UINT32_MAX;
}

/* ------------------------------------------------------------------------- */
/* Swapchain                                                                  */
/* ------------------------------------------------------------------------- */

void vk_swapchain_destroy(VkContext* c) {
    if (!c->device) return;
    for (uint32_t i = 0; i < c->swap_image_count; i++) {
        if (c->swap_views[i]) vkDestroyImageView(c->device, c->swap_views[i], NULL);
        c->swap_views[i] = VK_NULL_HANDLE;
        c->swap_images[i] = VK_NULL_HANDLE;
    }
    c->swap_image_count = 0;
    if (c->swapchain) { vkDestroySwapchainKHR(c->device, c->swapchain, NULL); c->swapchain = VK_NULL_HANDLE; }
}

/* ZS1_VSYNC=0|1|-1, the Vulkan side of the GL backend's swap interval.
 *
 * Unset: FIFO, exactly as before. Set, the closest supported mode:
 *    0  MAILBOX (no wait, no tearing), else IMMEDIATE, else FIFO
 *    1  FIFO
 *   -1  FIFO_RELAXED (a late image tears instead of waiting a whole refresh),
 *       else FIFO
 * With FIFO, vkAcquireNextImageKHR blocks once every swapchain image is
 * queued, and the render thread holds the frame (and so main.c's
 * renderer_wait_frame_done) until it returns; this is the knob that takes
 * that wait out for a measurement. Logged when the choice changes, not on
 * every resize. */
static VkPresentModeKHR vk_pick_present_mode(const VkContext* c) {
    const char* v = getenv("ZS1_VSYNC");
    if (!v) return VK_PRESENT_MODE_FIFO_KHR;

    VkPresentModeKHR modes[16];
    uint32_t n = 16;
    if (vkGetPhysicalDeviceSurfacePresentModesKHR(c->phys, c->surface, &n, modes) < 0) n = 0;
    bool has_mailbox = false, has_immediate = false, has_relaxed = false;
    for (uint32_t i = 0; i < n; i++) {
        if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR)      has_mailbox   = true;
        if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR)    has_immediate = true;
        if (modes[i] == VK_PRESENT_MODE_FIFO_RELAXED_KHR) has_relaxed   = true;
    }

    int want = atoi(v);
    VkPresentModeKHR pick = VK_PRESENT_MODE_FIFO_KHR;
    const char* name = "FIFO";
    if (want == 0) {
        if (has_mailbox)        { pick = VK_PRESENT_MODE_MAILBOX_KHR;   name = "MAILBOX"; }
        else if (has_immediate) { pick = VK_PRESENT_MODE_IMMEDIATE_KHR; name = "IMMEDIATE"; }
    } else if (want == -1) {
        if (has_relaxed)        { pick = VK_PRESENT_MODE_FIFO_RELAXED_KHR; name = "FIFO_RELAXED"; }
    } else if (want != 1) {
        LOG_RENDERER_WARN("[VK] ZS1_VSYNC=\"%s\" not recognised (want 0, 1 or -1); using FIFO", v);
    }
    static int logged = -1;
    if (logged != (int)pick) {
        logged = (int)pick;
        LOG_RENDERER_INFO("[VK] ZS1_VSYNC=%s: present mode %s (mailbox %s, immediate %s, fifo_relaxed %s)",
                          v, name, has_mailbox ? "yes" : "no", has_immediate ? "yes" : "no",
                          has_relaxed ? "yes" : "no");
    }
    return pick;
}

bool vk_swapchain_create(VkContext* c, SDL_Window* window) {
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(c->phys, c->surface, &caps),
             "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");

    /* Format: a plain 8-bit BGRA/RGBA in sRGB space. Deliberately NOT a _SRGB
     * format — the GL path writes the finished picture with no colour-space
     * conversion at all, and asking the hardware for one here would make the
     * two backends differ by a gamma curve on every single pixel, which is the
     * last thing an A/B needs. */
    uint32_t n_fmt = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(c->phys, c->surface, &n_fmt, NULL);
    VkSurfaceFormatKHR* fmts = n_fmt ? (VkSurfaceFormatKHR*)calloc(n_fmt, sizeof(*fmts)) : NULL;
    if (!fmts) return false;
    vkGetPhysicalDeviceSurfaceFormatsKHR(c->phys, c->surface, &n_fmt, fmts);
    VkSurfaceFormatKHR chosen = fmts[0];
    for (uint32_t i = 0; i < n_fmt; i++) {
        if ((fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM || fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM) &&
            fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { chosen = fmts[i]; break; }
    }
    free(fmts);
    c->swap_format     = chosen.format;
    c->swap_colorspace = chosen.colorSpace;

    /* FIFO is the only mode guaranteed present, and it is the default: pacing
     * is done in software against the audio ring or the emulated refresh
     * (main.c). ZS1_VSYNC overrides it for an A/B, see vk_pick_present_mode. */
    c->present_mode = vk_pick_present_mode(c);

    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(window, &w, &h);
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {   /* the platform lets us choose */
        extent.width  = (uint32_t)(w > 0 ? w : 1);
        extent.height = (uint32_t)(h > 0 ? h : 1);
        if (extent.width  < caps.minImageExtent.width)  extent.width  = caps.minImageExtent.width;
        if (extent.height < caps.minImageExtent.height) extent.height = caps.minImageExtent.height;
        if (extent.width  > caps.maxImageExtent.width)  extent.width  = caps.maxImageExtent.width;
        if (extent.height > caps.maxImageExtent.height) extent.height = caps.maxImageExtent.height;
    }
    if (extent.width == 0 || extent.height == 0) return false;   /* minimised */
    c->swap_extent = extent;

    uint32_t want = caps.minImageCount + 1;
    if (caps.maxImageCount && want > caps.maxImageCount) want = caps.maxImageCount;
    if (want > VK_MAX_SWAP_IMAGES) want = VK_MAX_SWAP_IMAGES;

    VkSwapchainCreateInfoKHR sci = {
        .sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface          = c->surface,
        .minImageCount    = want,
        .imageFormat      = c->swap_format,
        .imageColorSpace  = c->swap_colorspace,
        .imageExtent      = extent,
        .imageArrayLayers = 1,
        .imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform     = caps.currentTransform,
        .compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode      = c->present_mode,
        .clipped          = VK_TRUE,
        .oldSwapchain     = VK_NULL_HANDLE,
    };
    VK_CHECK(vkCreateSwapchainKHR(c->device, &sci, NULL, &c->swapchain), "vkCreateSwapchainKHR");

    uint32_t n_img = 0;
    vkGetSwapchainImagesKHR(c->device, c->swapchain, &n_img, NULL);
    if (n_img > VK_MAX_SWAP_IMAGES) n_img = VK_MAX_SWAP_IMAGES;
    vkGetSwapchainImagesKHR(c->device, c->swapchain, &n_img, c->swap_images);
    c->swap_image_count = n_img;

    for (uint32_t i = 0; i < n_img; i++) {
        VkImageViewCreateInfo vci = {
            .sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image    = c->swap_images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format   = c->swap_format,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        VK_CHECK(vkCreateImageView(c->device, &vci, NULL, &c->swap_views[i]), "vkCreateImageView(swap)");
    }

    LOG_RENDERER_INFO("[VK] swapchain %ux%u, %u images, format %d",
                      extent.width, extent.height, n_img, (int)c->swap_format);
    return true;
}

/* ------------------------------------------------------------------------- */

bool vk_context_create(VkContext* c, SDL_Window* window, int device_index) {
    memset(c, 0, sizeof(*c));
    if (!vk_loader_init()) {
        LOG_RENDERER_ERROR("[VK] %s", vk_loader_failure());
        return false;
    }
    const bool want_validation = SDL_getenv("ZS1_VK_VALIDATE") != NULL;
    if (!create_instance(&c->instance, want_validation, &c->validation)) return false;
    if (!vk_loader_load_instance(c->instance)) return false;
    if (c->validation) LOG_RENDERER_INFO("[VK] validation layer on");

    if (!SDL_Vulkan_CreateSurface(window, c->instance, NULL, &c->surface)) {
        LOG_RENDERER_ERROR("[VK] SDL_Vulkan_CreateSurface: %s", SDL_GetError());
        return false;
    }
    if (!pick_physical_device(c, device_index)) return false;
    if (!create_logical_device(c))              return false;
    if (!vk_swapchain_create(c, window))        return false;

    /* The Host HW panel and the inspector read this, and until now only the GL
     * path filled it in — so a Vulkan run showed whatever GL string a previous
     * run had left, or nothing. Same fields, different source: the device name
     * and the driver name come from VkPhysicalDeviceProperties2 and
     * VkPhysicalDeviceDriverProperties rather than from glGetString. The
     * "request honoured" pair is genuinely true here, because a Vulkan device
     * is chosen by index rather than asked for through the environment. */
    {
        char version[64];
        snprintf(version, sizeof(version), "Vulkan %u.%u.%u",
                 VK_API_VERSION_MAJOR(c->api_version),
                 VK_API_VERSION_MINOR(c->api_version),
                 VK_API_VERSION_PATCH(c->api_version));
        host_info_set_gl(c->driver_name, c->device_name, version, c->driver_name, "", true);
    }
    return true;
}

void vk_context_destroy(VkContext* c) {
    if (c->device) {
        vkDeviceWaitIdle(c->device);
        vk_swapchain_destroy(c);
        vkDestroyDevice(c->device, NULL);
        c->device = VK_NULL_HANDLE;
    }
    if (c->surface)  { vkDestroySurfaceKHR(c->instance, c->surface, NULL); c->surface = VK_NULL_HANDLE; }
    if (c->instance) { vkDestroyInstance(c->instance, NULL); c->instance = VK_NULL_HANDLE; }
}
