/*
 * DRAGON GL - 3D ARCANE ENGINE
 * Copyright (C) 2026 Nicola Taibi
 * License: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*Vulkan 1.3 backend.
 *
 * Draws exactly the same SceneFrame / UIFrame as the OpenGL backend with
 * the same shading (the shaders/ directory holds the GLSL 4.50 mirror of the
 * GL sources, compiled to SPIR-V and embedded in the executable).
 *
 * Design:
 *   - volk loads libvulkan / vulkan-1.dll at run time (no link-time
 *     dependency: without a driver the game falls back to OpenGL);
 *   - dynamic rendering (no render pass objects), 4x MSAA resolved into
 *     the swapchain image, 2 frames in flight;
 *   - the static world mesh lives in device-local memory (staged copy
 *     recorded in the frame's command buffer); per-frame streams (dynamic
 *     entities, effects, UI) go to a persistently mapped ring per frame;
 *   - resources replaced while a frame may still use them are destroyed
 *     only after that frame's fence has signalled (per-frame garbage).*/

#include "gfx.h"
#include "assets.h"
#include "volk.h"
#include <GLFW/glfw3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAMES 2
#define MAX_SWAP_IMAGES 8
#define MAX_GARBAGE 32

#define VK_CHECK(x)                                                           \
    do {                                                                      \
        VkResult r_ = (x);                                                    \
        if (r_ != VK_SUCCESS) {                                               \
            fprintf(stderr, "[VK] %s failed (%d) at line %d\n", #x, (int)r_, __LINE__); \
            return false;                                                     \
        }                                                                     \
    } while (0)

typedef struct {
    VkBuffer buf;
    VkDeviceMemory mem;
    VkDeviceSize size;
    void *map;
} Buffer;

typedef struct {
    VkImage img;
    VkDeviceMemory mem;
    VkImageView view;
    int w, h, layers, mips;
    VkFormat fmt;
} Image;

typedef struct {
    enum { G_BUFFER, G_IMAGE } kind;
    Buffer b;
    Image i;
} Garbage;

typedef struct {
    VkCommandBuffer cmd;
    VkFence fence;
    VkSemaphore acquired;
    Buffer stream;          /*host-visible vertex ring for this frame*/
    VkDeviceSize stream_off;
    Buffer ubo;
    VkDescriptorSet set_scene, set_ui;
    Garbage garbage[MAX_GARBAGE];
    int ngarbage;
} Frame;

static GLFWwindow *g_win;
static VkInstance g_inst;
static VkSurfaceKHR g_surface;
static VkPhysicalDevice g_phys;
static VkDevice g_dev;
static VkQueue g_queue;
static uint32_t g_qfam;
static VkPhysicalDeviceMemoryProperties g_memprops;
static VkPhysicalDeviceProperties g_props;
static bool g_aniso;
static char g_device[300];
static bool g_vsync = true;

static VkSwapchainKHR g_swap;
static VkFormat g_swap_fmt;
static VkExtent2D g_extent;
static uint32_t g_nimages;
static VkImage g_images[MAX_SWAP_IMAGES];
static VkImageView g_views[MAX_SWAP_IMAGES];
static VkSemaphore g_rendered[MAX_SWAP_IMAGES];
static VkSampleCountFlagBits g_samples = VK_SAMPLE_COUNT_4_BIT;
static VkFormat g_depth_fmt;
static Image g_color_ms, g_depth;
static bool g_need_recreate;

static VkCommandPool g_pool;
static VkDescriptorPool g_dpool;
static VkDescriptorSetLayout g_dsl_scene, g_dsl_ui;
static VkPipelineLayout g_pl_scene, g_pl_ui, g_pl_post;
static VkPipeline g_p_opaque, g_p_trans, g_p_add, g_p_post, g_p_ui;
static VkSampler g_samp_mat, g_samp_linear, g_samp_nearest;

static Frame g_frames[FRAMES];
static uint32_t g_frame;

static Image g_tex_mat, g_tex_atlas, g_tex_expl, g_tex_dummy;
static Buffer g_world;
static uint32_t g_world_count;
static uint32_t world_ver = 0xFFFFFFFFu, atlas_ver = 0xFFFFFFFFu, expl_ver = 0xFFFFFFFFu;

static bool g_shot_armed;
static uint8_t *g_shot;
static int g_shot_w, g_shot_h;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static int find_memory(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < g_memprops.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (g_memprops.memoryTypes[i].propertyFlags & want) == want) return (int)i;
    return -1;
}

static bool buffer_create(Buffer *b, VkDeviceSize size, VkBufferUsageFlags usage, bool host) {
    memset(b, 0, sizeof(*b));
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VK_CHECK(vkCreateBuffer(g_dev, &bi, NULL, &b->buf));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g_dev, b->buf, &req);
    int mt = host ? find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                  : find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt < 0 && !host) mt = find_memory(req.memoryTypeBits, 0);
    if (mt < 0) { fprintf(stderr, "[VK] no suitable memory type\n"); return false; }
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
                               .memoryTypeIndex = (uint32_t)mt};
    VK_CHECK(vkAllocateMemory(g_dev, &ai, NULL, &b->mem));
    VK_CHECK(vkBindBufferMemory(g_dev, b->buf, b->mem, 0));
    if (host) VK_CHECK(vkMapMemory(g_dev, b->mem, 0, VK_WHOLE_SIZE, 0, &b->map));
    b->size = size;
    return true;
}

static void buffer_destroy(Buffer *b) {
    if (b->map) vkUnmapMemory(g_dev, b->mem);
    if (b->buf) vkDestroyBuffer(g_dev, b->buf, NULL);
    if (b->mem) vkFreeMemory(g_dev, b->mem, NULL);
    memset(b, 0, sizeof(*b));
}

static bool image_create(Image *im, int w, int h, int layers, int mips, VkFormat fmt, VkImageUsageFlags usage,
                         VkSampleCountFlagBits samples, VkImageAspectFlags aspect) {
    memset(im, 0, sizeof(*im));
    VkImageCreateInfo ii = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = fmt,
        .extent = {(uint32_t)w, (uint32_t)h, 1},
        .mipLevels = (uint32_t)mips,
        .arrayLayers = (uint32_t)layers,
        .samples = samples,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VK_CHECK(vkCreateImage(g_dev, &ii, NULL, &im->img));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g_dev, im->img, &req);
    int mt = find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt < 0) mt = find_memory(req.memoryTypeBits, 0);
    if (mt < 0) return false;
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
                               .memoryTypeIndex = (uint32_t)mt};
    VK_CHECK(vkAllocateMemory(g_dev, &ai, NULL, &im->mem));
    VK_CHECK(vkBindImageMemory(g_dev, im->img, im->mem, 0));
    VkImageViewCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = im->img,
        .viewType = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D,
        .format = fmt,
        .subresourceRange = {aspect, 0, (uint32_t)mips, 0, (uint32_t)layers},
    };
    VK_CHECK(vkCreateImageView(g_dev, &vi, NULL, &im->view));
    im->w = w;
    im->h = h;
    im->layers = layers;
    im->mips = mips;
    im->fmt = fmt;
    return true;
}

/*The material array is always viewed as an array, even with 1 layer.*/
static bool image_view_as_array(Image *im) {
    if (im->view) vkDestroyImageView(g_dev, im->view, NULL);
    VkImageViewCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = im->img,
        .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
        .format = im->fmt,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, (uint32_t)im->mips, 0, (uint32_t)im->layers},
    };
    VK_CHECK(vkCreateImageView(g_dev, &vi, NULL, &im->view));
    return true;
}

static void image_destroy(Image *im) {
    if (im->view) vkDestroyImageView(g_dev, im->view, NULL);
    if (im->img) vkDestroyImage(g_dev, im->img, NULL);
    if (im->mem) vkFreeMemory(g_dev, im->mem, NULL);
    memset(im, 0, sizeof(*im));
}

static void garbage_buffer(Frame *f, Buffer *b) {
    if (!b->buf) return;
    if (f->ngarbage >= MAX_GARBAGE) {
        vkDeviceWaitIdle(g_dev);
        buffer_destroy(b);
        return;
    }
    f->garbage[f->ngarbage].kind = G_BUFFER;
    f->garbage[f->ngarbage++].b = *b;
    memset(b, 0, sizeof(*b));
}

static void garbage_image(Frame *f, Image *im) {
    if (!im->img) return;
    if (f->ngarbage >= MAX_GARBAGE) {
        vkDeviceWaitIdle(g_dev);
        image_destroy(im);
        return;
    }
    f->garbage[f->ngarbage].kind = G_IMAGE;
    f->garbage[f->ngarbage++].i = *im;
    memset(im, 0, sizeof(*im));
}

static void garbage_flush(Frame *f) {
    for (int i = 0; i < f->ngarbage; i++) {
        if (f->garbage[i].kind == G_BUFFER) buffer_destroy(&f->garbage[i].b);
        else image_destroy(&f->garbage[i].i);
    }
    f->ngarbage = 0;
}

static void barrier_image(VkCommandBuffer cmd, VkImage img, VkImageAspectFlags aspect, uint32_t mips, uint32_t layers,
                          VkImageLayout from, VkImageLayout to, VkPipelineStageFlags src_stage, VkAccessFlags src_access,
                          VkPipelineStageFlags dst_stage, VkAccessFlags dst_access) {
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = from,
        .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img,
        .subresourceRange = {aspect, 0, mips, 0, layers},
    };
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

/*Upload pixels into a sampled image, inside `cmd`. Staging buffer goes to
 * the frame's garbage list (or is returned for one-shot uploads).*/
static bool record_image_upload(VkCommandBuffer cmd, Frame *f, Image *im, const void *pixels, size_t bytes,
                                bool first_use, bool gen_mips, Buffer *staging_out) {
    Buffer st;
    if (!buffer_create(&st, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true)) return false;
    memcpy(st.map, pixels, bytes);
    barrier_image(cmd, im->img, VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)im->mips, (uint32_t)im->layers,
                  first_use ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferImageCopy rg = {
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, (uint32_t)im->layers},
        .imageExtent = {(uint32_t)im->w, (uint32_t)im->h, 1},
    };
    vkCmdCopyBufferToImage(cmd, st.buf, im->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg);
    if (gen_mips && im->mips > 1) {
        int w = im->w, h = im->h;
        for (int m = 1; m < im->mips; m++) {
            VkImageMemoryBarrier b = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = im->img,
                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)(m - 1), 1, 0, (uint32_t)im->layers},
            };
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
            int nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;
            VkImageBlit blit = {
                .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)(m - 1), 0, (uint32_t)im->layers},
                .srcOffsets = {{0, 0, 0}, {w, h, 1}},
                .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)m, 0, (uint32_t)im->layers},
                .dstOffsets = {{0, 0, 0}, {nw, nh, 1}},
            };
            vkCmdBlitImage(cmd, im->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, im->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &blit, VK_FILTER_LINEAR);
            w = nw;
            h = nh;
        }
        /*levels 0..mips-2 are TRANSFER_SRC, the last one TRANSFER_DST*/
        VkImageMemoryBarrier bs[2] = {
            {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = im->img,
                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, (uint32_t)(im->mips - 1), 0, (uint32_t)im->layers},
            },
            {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = im->img,
                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)(im->mips - 1), 1, 0, (uint32_t)im->layers},
            },
        };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 2, bs);
    } else {
        barrier_image(cmd, im->img, VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)im->mips, (uint32_t)im->layers,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                      VK_ACCESS_SHADER_READ_BIT);
    }
    if (staging_out) *staging_out = st;
    else garbage_buffer(f, &st);
    return true;
}

static VkCommandBuffer one_shot_begin(void) {
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = g_pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VkCommandBuffer cmd;
    if (vkAllocateCommandBuffers(g_dev, &ai, &cmd) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cmd, &bi);
    return cmd;
}

static void one_shot_end(VkCommandBuffer cmd) {
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd};
    vkQueueSubmit(g_queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(g_queue);
    vkFreeCommandBuffers(g_dev, g_pool, 1, &cmd);
}

/* ------------------------------------------------------------------ */
/* Swapchain                                                           */
/* ------------------------------------------------------------------ */

static void swapchain_destroy(void) {
    for (uint32_t i = 0; i < g_nimages; i++) {
        if (g_views[i]) vkDestroyImageView(g_dev, g_views[i], NULL);
        if (g_rendered[i]) vkDestroySemaphore(g_dev, g_rendered[i], NULL);
        g_views[i] = VK_NULL_HANDLE;
        g_rendered[i] = VK_NULL_HANDLE;
    }
    image_destroy(&g_color_ms);
    image_destroy(&g_depth);
}

static bool swapchain_create(void) {
    int fw = 0, fh = 0;
    glfwGetFramebufferSize(g_win, &fw, &fh);
    if (fw <= 0 || fh <= 0) return false;
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_phys, g_surface, &caps));

    uint32_t nf = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_phys, g_surface, &nf, NULL);
    VkSurfaceFormatKHR fmts[64];
    if (nf > 64) nf = 64;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_phys, g_surface, &nf, fmts);
    if (!nf) return false;
    /*UNORM, like the default GL framebuffer: identical colours*/
    VkSurfaceFormatKHR sf = fmts[0];
    for (uint32_t i = 0; i < nf; i++)
        if ((fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM || fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM) &&
            fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            sf = fmts[i];
            break;
        }

    uint32_t npm = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_phys, g_surface, &npm, NULL);
    VkPresentModeKHR pms[16];
    if (npm > 16) npm = 16;
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_phys, g_surface, &npm, pms);
    VkPresentModeKHR pm = VK_PRESENT_MODE_FIFO_KHR;
    if (!g_vsync) {
        for (uint32_t i = 0; i < npm; i++)
            if (pms[i] == VK_PRESENT_MODE_MAILBOX_KHR) pm = pms[i];
        if (pm == VK_PRESENT_MODE_FIFO_KHR)
            for (uint32_t i = 0; i < npm; i++)
                if (pms[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) pm = pms[i];
    }

    VkExtent2D ext = caps.currentExtent;
    if (ext.width == UINT32_MAX) {
        ext.width = (uint32_t)fw;
        ext.height = (uint32_t)fh;
    }
    if (ext.width < caps.minImageExtent.width) ext.width = caps.minImageExtent.width;
    if (ext.height < caps.minImageExtent.height) ext.height = caps.minImageExtent.height;
    if (caps.maxImageExtent.width && ext.width > caps.maxImageExtent.width) ext.width = caps.maxImageExtent.width;
    if (caps.maxImageExtent.height && ext.height > caps.maxImageExtent.height) ext.height = caps.maxImageExtent.height;
    if (ext.width == 0 || ext.height == 0) return false;

    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    if (count > MAX_SWAP_IMAGES) count = MAX_SWAP_IMAGES;

    VkCompositeAlphaFlagBitsKHR ca = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & ca)) {
        if (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR) ca = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
        else if (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR) ca = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
    }
    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    VkSwapchainKHR old = g_swap;
    VkSwapchainCreateInfoKHR ci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = g_surface,
        .minImageCount = count,
        .imageFormat = sf.format,
        .imageColorSpace = sf.colorSpace,
        .imageExtent = ext,
        .imageArrayLayers = 1,
        .imageUsage = usage,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                                                                                              : caps.currentTransform,
        .compositeAlpha = ca,
        .presentMode = pm,
        .clipped = VK_TRUE,
        .oldSwapchain = old,
    };
    VK_CHECK(vkCreateSwapchainKHR(g_dev, &ci, NULL, &g_swap));
    if (old) vkDestroySwapchainKHR(g_dev, old, NULL);
    g_swap_fmt = sf.format;
    g_extent = ext;

    g_nimages = MAX_SWAP_IMAGES;
    VK_CHECK(vkGetSwapchainImagesKHR(g_dev, g_swap, &g_nimages, g_images));
    for (uint32_t i = 0; i < g_nimages; i++) {
        VkImageViewCreateInfo vi = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = g_images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = g_swap_fmt,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
        };
        VK_CHECK(vkCreateImageView(g_dev, &vi, NULL, &g_views[i]));
        VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(g_dev, &si, NULL, &g_rendered[i]));
    }
    if (g_samples != VK_SAMPLE_COUNT_1_BIT &&
        !image_create(&g_color_ms, (int)ext.width, (int)ext.height, 1, 1, g_swap_fmt,
                      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT, g_samples,
                      VK_IMAGE_ASPECT_COLOR_BIT))
        return false;
    if (!image_create(&g_depth, (int)ext.width, (int)ext.height, 1, 1, g_depth_fmt, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                      g_samples, VK_IMAGE_ASPECT_DEPTH_BIT))
        return false;
    g_need_recreate = false;
    return true;
}

static bool swapchain_recreate(void) {
    vkDeviceWaitIdle(g_dev);
    swapchain_destroy();
    return swapchain_create();
}

/* ------------------------------------------------------------------ */
/* Pipelines                                                           */
/* ------------------------------------------------------------------ */

static VkShaderModule shader(const unsigned char *code, size_t size) {
    /*the embedded arrays are not guaranteed 4-byte aligned: copy*/
    uint32_t *words = malloc(size + 4);
    if (!words) return VK_NULL_HANDLE;
    memcpy(words, code, size);
    VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = words};
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(g_dev, &ci, NULL, &m) != VK_SUCCESS) m = VK_NULL_HANDLE;
    free(words);
    return m;
}

typedef enum { BLEND_NONE, BLEND_ALPHA, BLEND_ADD } BlendMode;

static VkPipeline make_pipeline(VkPipelineLayout layout, VkShaderModule vs, VkShaderModule fs,
                                const VkPipelineVertexInputStateCreateInfo *vin, BlendMode blend, bool depth_test,
                                bool depth_write) {
    VkPipelineShaderStageCreateInfo st[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main"},
    };
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                            .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
                                                 .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = g_samples};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                .depthTestEnable = depth_test, .depthWriteEnable = depth_write,
                                                .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL};
    VkPipelineColorBlendAttachmentState ba = {.colorWriteMask = 0xF};
    if (blend == BLEND_ALPHA) {
        ba.blendEnable = VK_TRUE;
        ba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        ba.colorBlendOp = VK_BLEND_OP_ADD;
        ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        ba.alphaBlendOp = VK_BLEND_OP_ADD;
    } else if (blend == BLEND_ADD) {
        ba.blendEnable = VK_TRUE;
        ba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        ba.colorBlendOp = VK_BLEND_OP_ADD;
        ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        ba.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = 1, .pAttachments = &ba};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                        .colorAttachmentCount = 1, .pColorAttachmentFormats = &g_swap_fmt,
                                        .depthAttachmentFormat = g_depth_fmt};
    VkGraphicsPipelineCreateInfo pi = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &ri,
        .stageCount = 2,
        .pStages = st,
        .pVertexInputState = vin,
        .pInputAssemblyState = &ia,
        .pViewportState = &vp,
        .pRasterizationState = &rs,
        .pMultisampleState = &ms,
        .pDepthStencilState = &ds,
        .pColorBlendState = &cb,
        .pDynamicState = &dy,
        .layout = layout,
    };
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(g_dev, VK_NULL_HANDLE, 1, &pi, NULL, &p) != VK_SUCCESS) {
        fprintf(stderr, "[VK] pipeline creation failed\n");
        return VK_NULL_HANDLE;
    }
    return p;
}

static bool pipelines_create(void) {
    /*descriptor layouts*/
    VkDescriptorSetLayoutBinding sb[3] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
    };
    VkDescriptorSetLayoutCreateInfo dl = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 3,
                                          .pBindings = sb};
    VK_CHECK(vkCreateDescriptorSetLayout(g_dev, &dl, NULL, &g_dsl_scene));
    VkDescriptorSetLayoutBinding ub = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    dl.bindingCount = 1;
    dl.pBindings = &ub;
    VK_CHECK(vkCreateDescriptorSetLayout(g_dev, &dl, NULL, &g_dsl_ui));

    VkPushConstantRange pr_scene = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &g_dsl_scene, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr_scene};
    VK_CHECK(vkCreatePipelineLayout(g_dev, &li, NULL, &g_pl_scene));
    VkPushConstantRange pr_ui = {VK_SHADER_STAGE_VERTEX_BIT, 0, 16};
    li.pSetLayouts = &g_dsl_ui;
    li.pPushConstantRanges = &pr_ui;
    VK_CHECK(vkCreatePipelineLayout(g_dev, &li, NULL, &g_pl_ui));
    VkPushConstantRange pr_post = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
    li.setLayoutCount = 0;
    li.pSetLayouts = NULL;
    li.pPushConstantRanges = &pr_post;
    VK_CHECK(vkCreatePipelineLayout(g_dev, &li, NULL, &g_pl_post));

    VkShaderModule svs = shader(asset_spv_scene_vert, asset_spv_scene_vert_size);
    VkShaderModule sfs = shader(asset_spv_scene_frag, asset_spv_scene_frag_size);
    VkShaderModule uvs = shader(asset_spv_ui_vert, asset_spv_ui_vert_size);
    VkShaderModule ufs = shader(asset_spv_ui_frag, asset_spv_ui_frag_size);
    VkShaderModule pvs = shader(asset_spv_post_vert, asset_spv_post_vert_size);
    VkShaderModule pfs = shader(asset_spv_post_frag, asset_spv_post_frag_size);
    bool ok = svs && sfs && uvs && ufs && pvs && pfs;

    VkVertexInputBindingDescription sbind = {0, sizeof(SVert), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription sattr[7] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(SVert, pos)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(SVert, nrm)},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(SVert, uv)},
        {3, 0, VK_FORMAT_R32_SFLOAT, offsetof(SVert, layer)},
        {4, 0, VK_FORMAT_R32_SFLOAT, offsetof(SVert, glow)},
        {5, 0, VK_FORMAT_R32_SFLOAT, offsetof(SVert, flags)},
        {6, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(SVert, col)},
    };
    VkPipelineVertexInputStateCreateInfo svin = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                                 .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &sbind,
                                                 .vertexAttributeDescriptionCount = 7, .pVertexAttributeDescriptions = sattr};
    VkVertexInputBindingDescription ubind = {0, sizeof(UIVert), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription uattr[3] = {
        {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(UIVert, pos)},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(UIVert, uv)},
        {2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(UIVert, col)},
    };
    VkPipelineVertexInputStateCreateInfo uvin = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                                 .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &ubind,
                                                 .vertexAttributeDescriptionCount = 3, .pVertexAttributeDescriptions = uattr};
    VkPipelineVertexInputStateCreateInfo pvin = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};

    if (ok) {
        g_p_opaque = make_pipeline(g_pl_scene, svs, sfs, &svin, BLEND_NONE, true, true);
        g_p_trans = make_pipeline(g_pl_scene, svs, sfs, &svin, BLEND_ALPHA, true, false);
        g_p_add = make_pipeline(g_pl_scene, svs, sfs, &svin, BLEND_ADD, true, false);
        g_p_post = make_pipeline(g_pl_post, pvs, pfs, &pvin, BLEND_ALPHA, false, false);
        g_p_ui = make_pipeline(g_pl_ui, uvs, ufs, &uvin, BLEND_ALPHA, false, false);
        ok = g_p_opaque && g_p_trans && g_p_add && g_p_post && g_p_ui;
    }
    VkShaderModule mods[6] = {svs, sfs, uvs, ufs, pvs, pfs};
    for (int i = 0; i < 6; i++)
        if (mods[i]) vkDestroyShaderModule(g_dev, mods[i], NULL);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Init                                                                */
/* ------------------------------------------------------------------ */

static int g_vk_loaded = -1;

bool gfx_vk_available(void) {
    if (g_vk_loaded < 0) g_vk_loaded = (volkInitialize() == VK_SUCCESS && glfwVulkanSupported()) ? 1 : 0;
    return g_vk_loaded == 1;
}

static void vk_hints(void) {
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
}

static bool pick_device(void) {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(g_inst, &n, NULL);
    if (!n) { fprintf(stderr, "[VK] no physical device\n"); return false; }
    VkPhysicalDevice devs[16];
    if (n > 16) n = 16;
    vkEnumeratePhysicalDevices(g_inst, &n, devs);
    int best_score = -1;
    for (uint32_t i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devs[i], &p);
        if (p.apiVersion < VK_API_VERSION_1_3) continue;
        VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f13};
        vkGetPhysicalDeviceFeatures2(devs[i], &f2);
        if (!f13.dynamicRendering) continue;
        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &nq, NULL);
        VkQueueFamilyProperties qp[16];
        if (nq > 16) nq = 16;
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &nq, qp);
        int fam = -1;
        for (uint32_t q = 0; q < nq; q++) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(devs[i], q, g_surface, &present);
            if ((qp[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) { fam = (int)q; break; }
        }
        if (fam < 0) continue;
        int score = 1;
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) score = 4;
        else if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) score = 3;
        else if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU) score = 2;
        if (score > best_score) {
            best_score = score;
            g_phys = devs[i];
            g_qfam = (uint32_t)fam;
            g_props = p;
            g_aniso = f2.features.samplerAnisotropy;
        }
    }
    if (best_score < 0) {
        fprintf(stderr, "[VK] no Vulkan 1.3 device with dynamic rendering and presentation\n");
        return false;
    }
    return true;
}

static bool create_sampler(VkSampler *s, VkFilter filter, VkSamplerMipmapMode mip, VkSamplerAddressMode addr, float max_lod,
                           bool aniso) {
    VkSamplerCreateInfo si = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = filter,
        .minFilter = filter,
        .mipmapMode = mip,
        .addressModeU = addr,
        .addressModeV = addr,
        .addressModeW = addr,
        .anisotropyEnable = aniso && g_aniso,
        .maxAnisotropy = aniso && g_aniso ? (g_props.limits.maxSamplerAnisotropy < 8.0f ? g_props.limits.maxSamplerAnisotropy : 8.0f) : 1.0f,
        .maxLod = max_lod,
    };
    VK_CHECK(vkCreateSampler(g_dev, &si, NULL, s));
    return true;
}

static bool vk_init(GLFWwindow *win, bool vsync) {
    g_win = win;
    g_vsync = vsync;
    if (!gfx_vk_available()) return false;

    uint32_t next = 0;
    const char **exts = glfwGetRequiredInstanceExtensions(&next);
    if (!exts) { fprintf(stderr, "[VK] no window-system extensions\n"); return false; }
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "DragonGL",
                             .applicationVersion = VK_MAKE_VERSION(3, 0, 0), .pEngineName = "DragonGL Arcane Engine",
                             .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                                .enabledExtensionCount = next, .ppEnabledExtensionNames = exts};
    const char *layer = "VK_LAYER_KHRONOS_validation";
    if (getenv("DRAGONGL_VK_VALIDATION")) {
        ici.enabledLayerCount = 1;
        ici.ppEnabledLayerNames = &layer;
    }
    VK_CHECK(vkCreateInstance(&ici, NULL, &g_inst));
    volkLoadInstanceOnly(g_inst);
    VK_CHECK(glfwCreateWindowSurface(g_inst, win, NULL, &g_surface));
    if (!pick_device()) return false;
    vkGetPhysicalDeviceMemoryProperties(g_phys, &g_memprops);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = g_qfam,
                                   .queueCount = 1, .pQueuePriorities = &prio};
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .dynamicRendering = VK_TRUE};
    VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f13};
    f2.features.samplerAnisotropy = g_aniso;
    const char *dext = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f2, .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &qci, .enabledExtensionCount = 1, .ppEnabledExtensionNames = &dext};
    VK_CHECK(vkCreateDevice(g_phys, &dci, NULL, &g_dev));
    volkLoadDevice(g_dev);
    vkGetDeviceQueue(g_dev, g_qfam, 0, &g_queue);
    int nlen = 0;
    while (g_props.deviceName[nlen] && g_props.deviceName[nlen] != '(' && nlen < 60) nlen++;
    while (nlen > 0 && g_props.deviceName[nlen - 1] == ' ') nlen--;
    snprintf(g_device, sizeof(g_device), "Vulkan %u.%u - %.*s", VK_API_VERSION_MAJOR(g_props.apiVersion),
             VK_API_VERSION_MINOR(g_props.apiVersion), nlen, g_props.deviceName);
    fprintf(stderr, "[VK] %s\n", g_device);

    /*sample count & depth format*/
    VkSampleCountFlags sc = g_props.limits.framebufferColorSampleCounts & g_props.limits.framebufferDepthSampleCounts;
    g_samples = (sc & VK_SAMPLE_COUNT_4_BIT) ? VK_SAMPLE_COUNT_4_BIT : ((sc & VK_SAMPLE_COUNT_2_BIT) ? VK_SAMPLE_COUNT_2_BIT : VK_SAMPLE_COUNT_1_BIT);
    const VkFormat dfs[3] = {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_X8_D24_UNORM_PACK32};
    g_depth_fmt = VK_FORMAT_UNDEFINED;
    for (int i = 0; i < 3; i++) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(g_phys, dfs[i], &fp);
        if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) { g_depth_fmt = dfs[i]; break; }
    }
    if (g_depth_fmt == VK_FORMAT_UNDEFINED) return false;

    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = g_qfam};
    VK_CHECK(vkCreateCommandPool(g_dev, &pci, NULL, &g_pool));

    if (!swapchain_create()) {
        int w = 0, h = 0;
        glfwGetFramebufferSize(win, &w, &h);
        if (w > 0 && h > 0) return false; /*a real failure, not a minimised window*/
        g_need_recreate = true;
    }
    if (!pipelines_create()) return false;
    if (!create_sampler(&g_samp_mat, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT, 16.0f, true)) return false;
    if (!create_sampler(&g_samp_linear, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 0.0f, false)) return false;
    if (!create_sampler(&g_samp_nearest, VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 0.0f, false)) return false;

    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, FRAMES}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, FRAMES * 3}};
    VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = FRAMES * 2,
                                      .poolSizeCount = 2, .pPoolSizes = ps};
    VK_CHECK(vkCreateDescriptorPool(g_dev, &dpi, NULL, &g_dpool));

    for (int i = 0; i < FRAMES; i++) {
        Frame *f = &g_frames[i];
        VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = g_pool,
                                          .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        VK_CHECK(vkAllocateCommandBuffers(g_dev, &ai, &f->cmd));
        VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
        VK_CHECK(vkCreateFence(g_dev, &fi, NULL, &f->fence));
        VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(g_dev, &si, NULL, &f->acquired));
        if (!buffer_create(&f->ubo, sizeof(SceneUniforms), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, true)) return false;
        if (!buffer_create(&f->stream, 4u << 20, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true)) return false;
        VkDescriptorSetLayout ls[2] = {g_dsl_scene, g_dsl_ui};
        VkDescriptorSet sets[2];
        VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = g_dpool,
                                           .descriptorSetCount = 2, .pSetLayouts = ls};
        VK_CHECK(vkAllocateDescriptorSets(g_dev, &dai, sets));
        f->set_scene = sets[0];
        f->set_ui = sets[1];
    }

    /*1x1 placeholder so every descriptor is valid before the first upload*/
    if (!image_create(&g_tex_dummy, 1, 1, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                      VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT))
        return false;
    VkCommandBuffer cmd = one_shot_begin();
    const uint8_t white[4] = {255, 255, 255, 255};
    Buffer st;
    record_image_upload(cmd, NULL, &g_tex_dummy, white, 4, true, false, &st);
    one_shot_end(cmd);
    buffer_destroy(&st);
    return true;
}

static void vk_upload_materials(const uint8_t *rgba, int size, int layers) {
    vkDeviceWaitIdle(g_dev);
    image_destroy(&g_tex_mat);
    int mips = 1;
    for (int s = size; s > 1; s >>= 1) mips++;
    if (!image_create(&g_tex_mat, size, size, layers, mips, VK_FORMAT_R8G8B8A8_UNORM,
                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                      VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT))
        return;
    image_view_as_array(&g_tex_mat);
    VkCommandBuffer cmd = one_shot_begin();
    Buffer st;
    record_image_upload(cmd, NULL, &g_tex_mat, rgba, (size_t)size * size * 4 * layers, true, true, &st);
    one_shot_end(cmd);
    buffer_destroy(&st);
}

/* ------------------------------------------------------------------ */
/* Frame                                                               */
/* ------------------------------------------------------------------ */

/*Copy vertices into this frame's stream buffer; returns the offset or
 * -1 (the buffer grows for the next frame when it overflows).*/
static VkDeviceSize g_stream_need;
static long long stream_push(Frame *f, const void *data, size_t bytes) {
    VkDeviceSize off = (f->stream_off + 15) & ~(VkDeviceSize)15;
    g_stream_need += bytes + 16;
    if (off + bytes > f->stream.size) return -1;
    memcpy((char *)f->stream.map + off, data, bytes);
    f->stream_off = off + bytes;
    return (long long)off;
}

static void draw_scene_stream(Frame *f, VkCommandBuffer cmd, VkPipeline p, int pass, const SVert *v, uint32_t n) {
    if (!n) return;
    long long off = stream_push(f, v, (size_t)n * sizeof(SVert));
    if (off < 0) return;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    int32_t pc[4] = {pass, 0, 0, 0};
    vkCmdPushConstants(cmd, g_pl_scene, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, pc);
    VkDeviceSize o = (VkDeviceSize)off;
    vkCmdBindVertexBuffers(cmd, 0, 1, &f->stream.buf, &o);
    vkCmdDraw(cmd, n, 1, 0, 0);
}

static void update_descriptors(Frame *f) {
    VkDescriptorBufferInfo bi = {f->ubo.buf, 0, sizeof(SceneUniforms)};
    VkDescriptorImageInfo mat = {g_samp_mat, g_tex_mat.view ? g_tex_mat.view : VK_NULL_HANDLE, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo expl = {g_samp_nearest, g_tex_expl.view ? g_tex_expl.view : g_tex_dummy.view,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo atl = {g_samp_linear, g_tex_atlas.view ? g_tex_atlas.view : g_tex_dummy.view,
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w[4] = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = f->set_scene, .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = f->set_scene, .dstBinding = 1, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &mat},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = f->set_scene, .dstBinding = 2, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &expl},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = f->set_ui, .dstBinding = 0, .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &atl},
    };
    if (g_tex_mat.view) vkUpdateDescriptorSets(g_dev, 4, w, 0, NULL);
    else vkUpdateDescriptorSets(g_dev, 1, &w[3], 0, NULL);
}

static void capture_swap_image(VkCommandBuffer cmd, VkImage img, Buffer *dst) {
    barrier_image(cmd, img, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy rg = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {g_extent.width, g_extent.height, 1}};
    vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->buf, 1, &rg);
    barrier_image(cmd, img, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
}

static void vk_render(const SceneFrame *s, const UIFrame *ui, int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (g_need_recreate || (uint32_t)w != g_extent.width || (uint32_t)h != g_extent.height) {
        if (!swapchain_recreate()) return;
    }
    Frame *f = &g_frames[g_frame];
    vkWaitForFences(g_dev, 1, &f->fence, VK_TRUE, UINT64_MAX);
    garbage_flush(f);

    uint32_t idx = 0;
    VkResult r = vkAcquireNextImageKHR(g_dev, g_swap, UINT64_MAX, f->acquired, VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        g_need_recreate = true;
        return;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return;
    vkResetFences(g_dev, 1, &f->fence);

    /*grow the stream buffer if the previous use of this frame overflowed*/
    if (g_stream_need > f->stream.size) {
        Buffer nb;
        if (buffer_create(&nb, g_stream_need + g_stream_need / 2, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true)) {
            buffer_destroy(&f->stream); /*fence waited: not in use*/
            f->stream = nb;
        }
    }
    g_stream_need = 0;
    f->stream_off = 0;

    VkCommandBuffer cmd = f->cmd;
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cmd, &bi);

    /*--- uploads ---*/
    if (s->draw_scene && s->world_version != world_ver) {
        garbage_buffer(f, &g_world);
        g_world_count = 0;
        size_t bytes = (size_t)s->world_count * sizeof(SVert);
        if (bytes) {
            Buffer st;
            if (buffer_create(&st, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true) &&
                buffer_create(&g_world, bytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false)) {
                memcpy(st.map, s->world, bytes);
                VkBufferCopy c = {0, 0, bytes};
                vkCmdCopyBuffer(cmd, st.buf, g_world.buf, 1, &c);
                VkBufferMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                                            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                            .dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT,
                                            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                            .buffer = g_world.buf, .size = VK_WHOLE_SIZE};
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 0, NULL, 1, &mb, 0, NULL);
                g_world_count = s->world_count;
            }
            garbage_buffer(f, &st);
        }
        world_ver = s->world_version;
    }
    if (s->draw_scene && s->explored.data && s->explored.version != expl_ver) {
        bool first = false;
        if (g_tex_expl.w != s->explored.w || g_tex_expl.h != s->explored.h) {
            garbage_image(f, &g_tex_expl);
            image_create(&g_tex_expl, s->explored.w, s->explored.h, 1, 1, VK_FORMAT_R8_UNORM,
                         VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
            first = true;
        }
        if (g_tex_expl.img)
            record_image_upload(cmd, f, &g_tex_expl, s->explored.data, (size_t)s->explored.w * s->explored.h, first, false, NULL);
        expl_ver = s->explored.version;
    }
    if (ui && ui->atlas && ui->atlas_version != atlas_ver) {
        bool first = false;
        if (g_tex_atlas.w != ui->atlas_w || g_tex_atlas.h != ui->atlas_h) {
            garbage_image(f, &g_tex_atlas);
            image_create(&g_tex_atlas, ui->atlas_w, ui->atlas_h, 1, 1, VK_FORMAT_R8G8B8A8_UNORM,
                         VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
            first = true;
        }
        if (g_tex_atlas.img)
            record_image_upload(cmd, f, &g_tex_atlas, ui->atlas, (size_t)ui->atlas_w * ui->atlas_h * 4, first, false, NULL);
        atlas_ver = ui->atlas_version;
    }
    update_descriptors(f);
    if (s->draw_scene) memcpy(f->ubo.map, &s->u, sizeof(SceneUniforms));

    /*--- rendering ---*/
    VkImage swap_img = g_images[idx];
    barrier_image(cmd, swap_img, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    if (g_color_ms.img)
        barrier_image(cmd, g_color_ms.img, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    VkImageAspectFlags daspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    if (g_depth_fmt != VK_FORMAT_D32_SFLOAT && g_depth_fmt != VK_FORMAT_X8_D24_UNORM_PACK32) daspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
    barrier_image(cmd, g_depth.img, daspect, 1, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                  VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, 0,
                  VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

    VkRenderingAttachmentInfo ca = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = g_color_ms.img ? g_color_ms.view : g_views[idx],
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .resolveMode = g_color_ms.img ? VK_RESOLVE_MODE_AVERAGE_BIT : VK_RESOLVE_MODE_NONE,
        .resolveImageView = g_color_ms.img ? g_views[idx] : VK_NULL_HANDLE,
        .resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = g_color_ms.img ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue.color = {{s->clear_color[0], s->clear_color[1], s->clear_color[2], 1.0f}},
    };
    VkRenderingAttachmentInfo da = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = g_depth.view,
        .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .clearValue.depthStencil = {1.0f, 0},
    };
    VkRenderingInfo ri = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
        .renderArea = {{0, 0}, g_extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &ca,
        .pDepthAttachment = &da,
    };
    vkCmdBeginRendering(cmd, &ri);
    VkViewport vp = {0, 0, (float)g_extent.width, (float)g_extent.height, 0.0f, 1.0f};
    VkRect2D sc = {{0, 0}, g_extent};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);

    if (s->draw_scene && g_tex_mat.view) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pl_scene, 0, 1, &f->set_scene, 0, NULL);
        if (g_world_count && g_world.buf) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_p_opaque);
            int32_t pc[4] = {0, 0, 0, 0};
            vkCmdPushConstants(cmd, g_pl_scene, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, pc);
            VkDeviceSize o = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &g_world.buf, &o);
            vkCmdDraw(cmd, g_world_count, 1, 0, 0);
        }
        draw_scene_stream(f, cmd, g_p_opaque, 0, s->dyn, s->dyn_count);
        draw_scene_stream(f, cmd, g_p_trans, 1, s->trans, s->trans_count);
        draw_scene_stream(f, cmd, g_p_add, 2, s->add, s->add_count);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_p_post);
        float aspect = (float)w / (float)(h > 0 ? h : 1);
        float pc[8] = {s->screen_tint[0], s->screen_tint[1], s->screen_tint[2], s->screen_tint[3], s->vignette,
                       aspect > 1 ? 1.0f : aspect, aspect > 1 ? 1.0f / aspect : 1.0f, 0};
        vkCmdPushConstants(cmd, g_pl_post, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, pc);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }
    if (ui && ui->count) {
        long long off = stream_push(f, ui->v, (size_t)ui->count * sizeof(UIVert));
        if (off >= 0) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_p_ui);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pl_ui, 0, 1, &f->set_ui, 0, NULL);
            float pc[4] = {(float)w, (float)h, 0, 0};
            vkCmdPushConstants(cmd, g_pl_ui, VK_SHADER_STAGE_VERTEX_BIT, 0, 16, pc);
            VkDeviceSize o = (VkDeviceSize)off;
            vkCmdBindVertexBuffers(cmd, 0, 1, &f->stream.buf, &o);
            vkCmdDraw(cmd, ui->count, 1, 0, 0);
        }
    }
    vkCmdEndRendering(cmd);

    Buffer shot = {0};
    bool shooting = false;
    if (g_shot_armed) {
        g_shot_armed = false;
        if (buffer_create(&shot, (VkDeviceSize)g_extent.width * g_extent.height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true)) {
            capture_swap_image(cmd, swap_img, &shot);
            shooting = true;
        }
    }
    barrier_image(cmd, swap_img, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0);
    vkEndCommandBuffer(cmd);

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &f->acquired,
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &g_rendered[idx],
    };
    if (vkQueueSubmit(g_queue, 1, &si, f->fence) != VK_SUCCESS) {
        fprintf(stderr, "[VK] queue submit failed\n");
        g_need_recreate = true;
        return;
    }
    VkPresentInfoKHR pi = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &g_rendered[idx],
        .swapchainCount = 1,
        .pSwapchains = &g_swap,
        .pImageIndices = &idx,
    };
    r = vkQueuePresentKHR(g_queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) g_need_recreate = true;

    if (shooting) {
        vkWaitForFences(g_dev, 1, &f->fence, VK_TRUE, UINT64_MAX);
        int W = (int)g_extent.width, H = (int)g_extent.height;
        uint8_t *px = malloc((size_t)W * H * 4);
        if (px) {
            memcpy(px, shot.map, (size_t)W * H * 4);
            if (g_swap_fmt == VK_FORMAT_B8G8R8A8_UNORM || g_swap_fmt == VK_FORMAT_B8G8R8A8_SRGB)
                for (size_t i = 0; i < (size_t)W * H; i++) {
                    uint8_t t = px[i * 4];
                    px[i * 4] = px[i * 4 + 2];
                    px[i * 4 + 2] = t;
                }
            for (size_t i = 0; i < (size_t)W * H; i++) px[i * 4 + 3] = 255;
            free(g_shot);
            g_shot = px;
            g_shot_w = W;
            g_shot_h = H;
        }
        buffer_destroy(&shot);
    }
    g_frame = (g_frame + 1) % FRAMES;
}

static uint8_t *vk_screenshot(int *w, int *h) {
    if (g_shot) {
        uint8_t *r = g_shot;
        g_shot = NULL;
        *w = g_shot_w;
        *h = g_shot_h;
        return r;
    }
    g_shot_armed = true;
    return NULL;
}

static void vk_set_vsync(bool on) {
    if (on == g_vsync) return;
    g_vsync = on;
    g_need_recreate = true;
}

static void vk_shutdown(void) {
    if (!g_dev) {
        if (g_surface) vkDestroySurfaceKHR(g_inst, g_surface, NULL);
        if (g_inst) vkDestroyInstance(g_inst, NULL);
        g_surface = VK_NULL_HANDLE;
        g_inst = VK_NULL_HANDLE;
        return;
    }
    vkDeviceWaitIdle(g_dev);
    for (int i = 0; i < FRAMES; i++) {
        Frame *f = &g_frames[i];
        garbage_flush(f);
        buffer_destroy(&f->stream);
        buffer_destroy(&f->ubo);
        if (f->fence) vkDestroyFence(g_dev, f->fence, NULL);
        if (f->acquired) vkDestroySemaphore(g_dev, f->acquired, NULL);
        memset(f, 0, sizeof(*f));
    }
    buffer_destroy(&g_world);
    image_destroy(&g_tex_mat);
    image_destroy(&g_tex_atlas);
    image_destroy(&g_tex_expl);
    image_destroy(&g_tex_dummy);
    swapchain_destroy();
    if (g_swap) vkDestroySwapchainKHR(g_dev, g_swap, NULL);
    VkPipeline ps[5] = {g_p_opaque, g_p_trans, g_p_add, g_p_post, g_p_ui};
    for (int i = 0; i < 5; i++)
        if (ps[i]) vkDestroyPipeline(g_dev, ps[i], NULL);
    if (g_pl_scene) vkDestroyPipelineLayout(g_dev, g_pl_scene, NULL);
    if (g_pl_ui) vkDestroyPipelineLayout(g_dev, g_pl_ui, NULL);
    if (g_pl_post) vkDestroyPipelineLayout(g_dev, g_pl_post, NULL);
    if (g_dsl_scene) vkDestroyDescriptorSetLayout(g_dev, g_dsl_scene, NULL);
    if (g_dsl_ui) vkDestroyDescriptorSetLayout(g_dev, g_dsl_ui, NULL);
    if (g_dpool) vkDestroyDescriptorPool(g_dev, g_dpool, NULL);
    VkSampler ss[3] = {g_samp_mat, g_samp_linear, g_samp_nearest};
    for (int i = 0; i < 3; i++)
        if (ss[i]) vkDestroySampler(g_dev, ss[i], NULL);
    if (g_pool) vkDestroyCommandPool(g_dev, g_pool, NULL);
    vkDestroyDevice(g_dev, NULL);
    vkDestroySurfaceKHR(g_inst, g_surface, NULL);
    vkDestroyInstance(g_inst, NULL);
    g_dev = VK_NULL_HANDLE;
    g_surface = VK_NULL_HANDLE;
    g_inst = VK_NULL_HANDLE;
    g_swap = VK_NULL_HANDLE;
    g_p_opaque = g_p_trans = g_p_add = g_p_post = g_p_ui = VK_NULL_HANDLE;
    g_pl_scene = g_pl_ui = g_pl_post = VK_NULL_HANDLE;
    g_dsl_scene = g_dsl_ui = VK_NULL_HANDLE;
    g_dpool = VK_NULL_HANDLE;
    g_samp_mat = g_samp_linear = g_samp_nearest = VK_NULL_HANDLE;
    g_pool = VK_NULL_HANDLE;
    world_ver = atlas_ver = expl_ver = 0xFFFFFFFFu;
    g_world_count = 0;
}

static const char *vk_device_name(void) { return g_device; }

const GfxBackend gfx_vk_backend = {
    "Vulkan 1.3",
    vk_hints,
    vk_init,
    vk_upload_materials,
    vk_render,
    vk_screenshot,
    vk_set_vsync,
    vk_shutdown,
    vk_device_name,
};
