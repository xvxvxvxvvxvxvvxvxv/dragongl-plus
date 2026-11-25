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
 *   - dynamic rendering (no render pass objects), 2 frames in flight;
 *   - the scene is lit into an HDR target (RGBA16F, optionally 4x MSAA
 *     resolved into it); a bloom chain (downsample / upsample), the
 *     composite pass (tone curve, grading, vignette) and FXAA then write
 *     the swapchain image, where the interface is drawn last;
 *   - the render targets are shared by the frames in flight: every pass
 *     starts with a barrier on what the previous frame read;
 *   - the static world mesh lives in device-local memory (staged copy
 *     recorded in the frame's command buffer); per-frame streams (dynamic
 *     entities, effects, UI) go to a persistently mapped ring per frame;
 *   - resources replaced while a frame may still use them are destroyed
 *     only after that frame's fence has signalled (per-frame garbage).*/

#include "gfx.h"
#include "shaders.h"
#include "mathx.h"
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
    VkDescriptorSet set_scene, set_trans, set_ui; /*set_trans also samples the scene depth*/
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
static VkSampleCountFlagBits g_msaa_max = VK_SAMPLE_COUNT_1_BIT; /*what the device can do*/
static VkSampleCountFlagBits g_samples = VK_SAMPLE_COUNT_1_BIT;   /*what the scene pipelines use*/
static VkFormat g_depth_fmt;
static const VkFormat g_hdr_fmt = VK_FORMAT_R16G16B16A16_SFLOAT;
static bool g_need_recreate;
static RenderSettings g_rs = {GFX_AA_MSAA, true, 1.0f, 2, true};

/*render targets (recreated with the swapchain or the settings)*/
#define BLOOM_LEVELS 6
static Image g_hdr_ms, g_hdr, g_depth, g_ldr, g_bloom[BLOOM_LEVELS];
static int g_nbloom;
static VkExtent2D g_rext; /*3D image size: swapchain size x render scale*/
static VkDescriptorSet g_set_down[BLOOM_LEVELS], g_set_up[BLOOM_LEVELS], g_set_comp, g_set_fxaa;
/*Vulkan-only effects: scene depth for sampling (MSAA resolve or copy),
 * ambient occlusion (half resolution), sun and torch shadow maps*/
static Image g_zbuf, g_ao, g_ao_blur, g_sun_map, g_cube, g_cube_depth;
static VkImageView g_cube_faces[6];
static Image g_dummy_depth, g_dummy_cube;
static VkFormat g_shadow_fmt;
static VkDescriptorSet g_set_ssao, g_set_ssao_blur, g_set_ao_apply;

static VkCommandPool g_pool;
static VkDescriptorPool g_dpool;
static VkDescriptorSetLayout g_dsl_scene, g_dsl_ui, g_dsl_post;
static VkPipelineLayout g_pl_scene, g_pl_ui, g_pl_post, g_pl_ssao;
static VkPipeline g_p_opaque, g_p_trans, g_p_add, g_p_ui, g_p_down, g_p_up, g_p_comp, g_p_fxaa;
static VkPipeline g_p_shadow_sun, g_p_shadow_cube, g_p_ssao, g_p_ssao_blur, g_p_ao_apply;
static VkSampler g_samp_mat, g_samp_linear, g_samp_nearest, g_samp_shadow;

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

/*Cube map (6 layers): a cube view for sampling and one 2D view per face
 * to render into.*/
static bool cube_create(Image *im, VkImageView faces[6], int size, VkFormat fmt, VkImageUsageFlags usage) {
    memset(im, 0, sizeof(*im));
    VkImageCreateInfo ii = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = fmt,
        .extent = {(uint32_t)size, (uint32_t)size, 1},
        .mipLevels = 1,
        .arrayLayers = 6,
        .samples = VK_SAMPLE_COUNT_1_BIT,
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
        .viewType = VK_IMAGE_VIEW_TYPE_CUBE,
        .format = fmt,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6},
    };
    VK_CHECK(vkCreateImageView(g_dev, &vi, NULL, &im->view));
    for (int f = 0; f < 6 && faces; f++) {
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.subresourceRange.baseArrayLayer = (uint32_t)f;
        vi.subresourceRange.layerCount = 1;
        VK_CHECK(vkCreateImageView(g_dev, &vi, NULL, &faces[f]));
    }
    im->w = im->h = size;
    im->layers = 6;
    im->mips = 1;
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

static void targets_destroy(void) {
    image_destroy(&g_hdr_ms);
    image_destroy(&g_hdr);
    image_destroy(&g_depth);
    image_destroy(&g_ldr);
    for (int i = 0; i < BLOOM_LEVELS; i++) image_destroy(&g_bloom[i]);
    g_nbloom = 0;
    image_destroy(&g_zbuf);
    image_destroy(&g_ao);
    image_destroy(&g_ao_blur);
    image_destroy(&g_sun_map);
    for (int f = 0; f < 6; f++) {
        if (g_cube_faces[f]) vkDestroyImageView(g_dev, g_cube_faces[f], NULL);
        g_cube_faces[f] = VK_NULL_HANDLE;
    }
    image_destroy(&g_cube);
    image_destroy(&g_cube_depth);
}

static void write_post_set(VkDescriptorSet set, const Image *a, const Image *b) {
    VkDescriptorImageInfo ii[2] = {
        {g_samp_linear, a->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {g_samp_linear, b->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
    };
    VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0,
                              .descriptorCount = 2, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                              .pImageInfo = ii};
    vkUpdateDescriptorSets(g_dev, 1, &w, 0, NULL);
}

/*HDR scene (+ MSAA image), depth, bloom chain and the FXAA input, sized
 * from the swapchain and the render scale. The device is idle.*/
static bool targets_create(void) {
    float sc = g_rs.render_scale < 0.5f ? 0.5f : (g_rs.render_scale > 1.0f ? 1.0f : g_rs.render_scale);
    g_rext.width = (uint32_t)((float)g_extent.width * sc + 0.5f);
    g_rext.height = (uint32_t)((float)g_extent.height * sc + 0.5f);
    if (!g_rext.width) g_rext.width = 1;
    if (!g_rext.height) g_rext.height = 1;
    int w = (int)g_rext.width, h = (int)g_rext.height;
    VkImageUsageFlags sampled = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (!image_create(&g_hdr, w, h, 1, 1, g_hdr_fmt, sampled, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT)) return false;
    /*the scene is drawn in two passes (opaque, then effects): the MSAA
     * image is kept in between, not transient*/
    if (g_samples != VK_SAMPLE_COUNT_1_BIT &&
        !image_create(&g_hdr_ms, w, h, 1, 1, g_hdr_fmt, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, g_samples, VK_IMAGE_ASPECT_COLOR_BIT))
        return false;
    if (!image_create(&g_depth, w, h, 1, 1, g_depth_fmt,
                      VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, g_samples,
                      VK_IMAGE_ASPECT_DEPTH_BIT))
        return false;
    /*single-sample copy of the opaque depth: soft particles and SSAO*/
    if (!image_create(&g_zbuf, w, h, 1, 1, g_depth_fmt,
                      VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                      VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_DEPTH_BIT))
        return false;
    if (g_rs.ssao) {
        int aw = (w + 1) / 2, ah = (h + 1) / 2;
        if (!image_create(&g_ao, aw, ah, 1, 1, VK_FORMAT_R8_UNORM, sampled, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT) ||
            !image_create(&g_ao_blur, aw, ah, 1, 1, VK_FORMAT_R8_UNORM, sampled, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT))
            return false;
        write_post_set(g_set_ssao, &g_zbuf, &g_zbuf);
        write_post_set(g_set_ssao_blur, &g_ao, &g_zbuf);
        write_post_set(g_set_ao_apply, &g_ao_blur, &g_ao_blur);
    }
    if (g_rs.shadows > 0) {
        int ss = g_rs.shadows >= 2 ? 2048 : 1024, cs = g_rs.shadows >= 2 ? 512 : 256;
        VkImageUsageFlags du = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        if (!image_create(&g_sun_map, ss, ss, 1, 1, g_shadow_fmt, du, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_DEPTH_BIT) ||
            !cube_create(&g_cube, g_cube_faces, cs, VK_FORMAT_R16_SFLOAT, sampled) ||
            !image_create(&g_cube_depth, cs, cs, 1, 1, g_shadow_fmt, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                          VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_DEPTH_BIT))
            return false;
    }
    if (g_rs.aa == GFX_AA_FXAA &&
        !image_create(&g_ldr, (int)g_extent.width, (int)g_extent.height, 1, 1, g_swap_fmt, sampled, VK_SAMPLE_COUNT_1_BIT,
                      VK_IMAGE_ASPECT_COLOR_BIT))
        return false;
    g_nbloom = 0;
    int bw = w / 2, bh = h / 2;
    while (g_nbloom < BLOOM_LEVELS && bw >= 8 && bh >= 8) {
        if (!image_create(&g_bloom[g_nbloom], bw, bh, 1, 1, g_hdr_fmt, sampled, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT))
            return false;
        g_nbloom++;
        bw /= 2;
        bh /= 2;
    }
    for (int i = 0; i < g_nbloom; i++) {
        const Image *src = i == 0 ? &g_hdr : &g_bloom[i - 1];
        write_post_set(g_set_down[i], src, src);
        if (i + 1 < g_nbloom) write_post_set(g_set_up[i], &g_bloom[i + 1], &g_bloom[i + 1]);
    }
    write_post_set(g_set_comp, &g_hdr, g_nbloom ? &g_bloom[0] : &g_hdr);
    if (g_ldr.view) write_post_set(g_set_fxaa, &g_ldr, &g_ldr);
    return true;
}

static void swapchain_destroy(void) {
    for (uint32_t i = 0; i < g_nimages; i++) {
        if (g_views[i]) vkDestroyImageView(g_dev, g_views[i], NULL);
        if (g_rendered[i]) vkDestroySemaphore(g_dev, g_rendered[i], NULL);
        g_views[i] = VK_NULL_HANDLE;
        g_rendered[i] = VK_NULL_HANDLE;
    }
    targets_destroy();
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
    if (!targets_create()) return false;
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

static VkShaderModule shader(const char *name) {
    const ShaderFile *f = shader_file(name);
    if (!f || !f->spv) {
        fprintf(stderr, "[VK] no SPIR-V for %s\n", name);
        return VK_NULL_HANDLE;
    }
    /*the embedded arrays are not guaranteed 4-byte aligned: copy*/
    uint32_t *words = malloc(f->spv_size + 4);
    if (!words) return VK_NULL_HANDLE;
    memcpy(words, f->spv, f->spv_size);
    VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = f->spv_size, .pCode = words};
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(g_dev, &ci, NULL, &m) != VK_SUCCESS) m = VK_NULL_HANDLE;
    free(words);
    return m;
}

typedef enum { BLEND_NONE, BLEND_ALPHA, BLEND_ADD, BLEND_MUL } BlendMode;

typedef struct {
    VkPipelineLayout layout;
    const char *vs, *fs;
    const VkPipelineVertexInputStateCreateInfo *vin;
    BlendMode blend;
    bool depth_test, depth_write;
    VkFormat color, depth; /*color / depth VK_FORMAT_UNDEFINED: no such attachment*/
    VkSampleCountFlagBits samples;
    bool depth_bias;       /*shadow maps: slope-scaled bias against acne*/
} PipeDesc;

static VkPipeline make_pipeline(const PipeDesc *d) {
    VkShaderModule vs = shader(d->vs), fs = shader(d->fs);
    VkPipeline p = VK_NULL_HANDLE;
    if (!vs || !fs) goto out;
    VkPipelineShaderStageCreateInfo st[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main"},
    };
    VkPipelineVertexInputStateCreateInfo none = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                            .viewportCount = 1, .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo rs = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
                                                 .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f,
                                                 .depthBiasEnable = d->depth_bias, .depthBiasConstantFactor = 1.5f,
                                                 .depthBiasSlopeFactor = 2.0f};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = d->samples};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                .depthTestEnable = d->depth_test, .depthWriteEnable = d->depth_write,
                                                .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL};
    VkPipelineColorBlendAttachmentState ba = {.colorWriteMask = 0xF};
    if (d->blend == BLEND_ALPHA) {
        ba.blendEnable = VK_TRUE;
        ba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        ba.colorBlendOp = VK_BLEND_OP_ADD;
        ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        ba.alphaBlendOp = VK_BLEND_OP_ADD;
    } else if (d->blend == BLEND_MUL) {
        ba.blendEnable = VK_TRUE;
        ba.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        ba.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR;
        ba.colorBlendOp = VK_BLEND_OP_ADD;
        ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        ba.alphaBlendOp = VK_BLEND_OP_ADD;
    } else if (d->blend == BLEND_ADD) {
        ba.blendEnable = VK_TRUE;
        ba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        ba.colorBlendOp = VK_BLEND_OP_ADD;
        ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        ba.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    bool has_color = d->color != VK_FORMAT_UNDEFINED;
    VkPipelineColorBlendStateCreateInfo cb = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = has_color ? 1u : 0u, .pAttachments = &ba};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = 2, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                        .colorAttachmentCount = has_color ? 1u : 0u, .pColorAttachmentFormats = &d->color,
                                        .depthAttachmentFormat = d->depth};
    VkGraphicsPipelineCreateInfo pi = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &ri,
        .stageCount = 2,
        .pStages = st,
        .pVertexInputState = d->vin ? d->vin : &none,
        .pInputAssemblyState = &ia,
        .pViewportState = &vp,
        .pRasterizationState = &rs,
        .pMultisampleState = &ms,
        .pDepthStencilState = &ds,
        .pColorBlendState = &cb,
        .pDynamicState = &dy,
        .layout = d->layout,
    };
    if (vkCreateGraphicsPipelines(g_dev, VK_NULL_HANDLE, 1, &pi, NULL, &p) != VK_SUCCESS) {
        fprintf(stderr, "[VK] pipeline creation failed (%s + %s)\n", d->vs, d->fs);
        p = VK_NULL_HANDLE;
    }
out:
    if (vs) vkDestroyShaderModule(g_dev, vs, NULL);
    if (fs) vkDestroyShaderModule(g_dev, fs, NULL);
    return p;
}

/*Descriptor set and pipeline layouts. Every pipeline has the same 64-byte
 * block of push constants (PUSH_BLOCK in shaders/common.glsl).*/
static bool layouts_create(void) {
    /*0 uniforms, 1 materials, 2 explored mask, 3 scene depth, 4 sun shadow, 5 torch shadow*/
    VkDescriptorSetLayoutBinding sb[6] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
    };
    VkDescriptorSetLayoutCreateInfo dl = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 6,
                                          .pBindings = sb};
    VK_CHECK(vkCreateDescriptorSetLayout(g_dev, &dl, NULL, &g_dsl_scene));
    VkDescriptorSetLayoutBinding ub = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    dl.bindingCount = 1;
    dl.pBindings = &ub;
    VK_CHECK(vkCreateDescriptorSetLayout(g_dev, &dl, NULL, &g_dsl_ui));
    VkDescriptorSetLayoutBinding pb[2] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
    };
    dl.bindingCount = 2;
    dl.pBindings = pb;
    VK_CHECK(vkCreateDescriptorSetLayout(g_dev, &dl, NULL, &g_dsl_post));

    VkPushConstantRange pr = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &g_dsl_scene, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    VK_CHECK(vkCreatePipelineLayout(g_dev, &li, NULL, &g_pl_scene));
    li.pSetLayouts = &g_dsl_ui;
    VK_CHECK(vkCreatePipelineLayout(g_dev, &li, NULL, &g_pl_ui));
    li.pSetLayouts = &g_dsl_post;
    VK_CHECK(vkCreatePipelineLayout(g_dev, &li, NULL, &g_pl_post));
    VkPushConstantRange pr_ao = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 128};
    li.pPushConstantRanges = &pr_ao;
    VK_CHECK(vkCreatePipelineLayout(g_dev, &li, NULL, &g_pl_ssao));
    return true;
}

static void pipelines_destroy(void) {
    VkPipeline *ps[] = {&g_p_opaque, &g_p_trans, &g_p_add, &g_p_ui, &g_p_down, &g_p_up, &g_p_comp, &g_p_fxaa,
                        &g_p_shadow_sun, &g_p_shadow_cube, &g_p_ssao, &g_p_ssao_blur, &g_p_ao_apply};
    for (size_t i = 0; i < sizeof(ps) / sizeof(ps[0]); i++) {
        if (*ps[i]) vkDestroyPipeline(g_dev, *ps[i], NULL);
        *ps[i] = VK_NULL_HANDLE;
    }
}

/*All pipelines, for the current swapchain format and MSAA sample count.*/
static bool pipelines_create(void) {
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
    PipeDesc d = {g_pl_scene, "scene.vert", "scene.frag", &svin, BLEND_NONE, true, true, g_hdr_fmt, g_depth_fmt, g_samples};
    g_p_opaque = make_pipeline(&d);
    d.blend = BLEND_ALPHA;
    d.depth_write = false;
    g_p_trans = make_pipeline(&d);
    d.blend = BLEND_ADD;
    g_p_add = make_pipeline(&d);
    PipeDesc post = {g_pl_post, "fullscreen.vert", "bloom_down.frag", NULL, BLEND_NONE, false, false, g_hdr_fmt,
                     VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT};
    g_p_down = make_pipeline(&post);
    post.fs = "bloom_up.frag";
    post.blend = BLEND_ADD;
    g_p_up = make_pipeline(&post);
    post.fs = "composite.frag";
    post.blend = BLEND_NONE;
    post.color = g_swap_fmt;
    g_p_comp = make_pipeline(&post);
    post.fs = "fxaa.frag";
    g_p_fxaa = make_pipeline(&post);
    PipeDesc ui = {g_pl_ui, "ui.vert", "ui.frag", &uvin, BLEND_ALPHA, false, false, g_swap_fmt, VK_FORMAT_UNDEFINED,
                   VK_SAMPLE_COUNT_1_BIT};
    g_p_ui = make_pipeline(&ui);
    /*Vulkan-only effects*/
    PipeDesc sh = {g_pl_scene, "shadow.vert", "shadow_sun.frag", &svin, BLEND_NONE, true, true, VK_FORMAT_UNDEFINED,
                   g_shadow_fmt, VK_SAMPLE_COUNT_1_BIT, true};
    g_p_shadow_sun = make_pipeline(&sh);
    sh.fs = "shadow_cube.frag";
    sh.color = VK_FORMAT_R16_SFLOAT;
    sh.depth_bias = false;
    g_p_shadow_cube = make_pipeline(&sh);
    PipeDesc ao = {g_pl_ssao, "fullscreen.vert", "ssao.frag", NULL, BLEND_NONE, false, false, VK_FORMAT_R8_UNORM,
                   VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT, false};
    g_p_ssao = make_pipeline(&ao);
    ao.layout = g_pl_post;
    ao.fs = "ssao_blur.frag";
    g_p_ssao_blur = make_pipeline(&ao);
    /*drawn inside the scene's transparent pass: its attachments*/
    PipeDesc ap = {g_pl_post, "fullscreen.vert", "ao_apply.frag", NULL, BLEND_MUL, false, false, g_hdr_fmt, g_depth_fmt,
                   g_samples, false};
    g_p_ao_apply = make_pipeline(&ap);
    return g_p_opaque && g_p_trans && g_p_add && g_p_down && g_p_up && g_p_comp && g_p_fxaa && g_p_ui &&
           g_p_shadow_sun && g_p_shadow_cube && g_p_ssao && g_p_ssao_blur && g_p_ao_apply;
}

/* ------------------------------------------------------------------ */
/* Init                                                                */
/* ------------------------------------------------------------------ */

static int g_vk_loaded = -1;

bool gfx_vk_cached(void) { return g_vk_loaded == 1; }

bool gfx_vk_available(void) {
    if (getenv("DRAGONGL_NO_VULKAN")) g_vk_loaded = 0; /*escape hatch for broken drivers*/
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
    g_msaa_max = (sc & VK_SAMPLE_COUNT_4_BIT) ? VK_SAMPLE_COUNT_4_BIT : ((sc & VK_SAMPLE_COUNT_2_BIT) ? VK_SAMPLE_COUNT_2_BIT : VK_SAMPLE_COUNT_1_BIT);
    g_samples = g_rs.aa == GFX_AA_MSAA ? g_msaa_max : VK_SAMPLE_COUNT_1_BIT;
    const VkFormat dfs[3] = {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_X8_D24_UNORM_PACK32};
    g_depth_fmt = VK_FORMAT_UNDEFINED;
    /*the scene depth is also copied and sampled (soft particles, SSAO)*/
    const VkFormatFeatureFlags dneed = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                       VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    for (int i = 0; i < 3; i++) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(g_phys, dfs[i], &fp);
        if ((fp.optimalTilingFeatures & dneed) == dneed) { g_depth_fmt = dfs[i]; break; }
    }
    if (g_depth_fmt == VK_FORMAT_UNDEFINED) return false;
    {
        /*shadow maps: sampled with comparison (D16 is always possible)*/
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(g_phys, VK_FORMAT_D32_SFLOAT, &fp);
        VkFormatFeatureFlags need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        g_shadow_fmt = (fp.optimalTilingFeatures & need) == need ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_D16_UNORM;
    }

    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = g_qfam};
    VK_CHECK(vkCreateCommandPool(g_dev, &pci, NULL, &g_pool));

    if (!create_sampler(&g_samp_mat, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT, 16.0f, true)) return false;
    if (!create_sampler(&g_samp_linear, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 0.0f, false)) return false;
    if (!create_sampler(&g_samp_nearest, VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, 0.0f, false)) return false;
    {
        VkSamplerCreateInfo si = {
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_LINEAR,
            .minFilter = VK_FILTER_LINEAR,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .compareEnable = VK_TRUE,
            .compareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
        };
        VK_CHECK(vkCreateSampler(g_dev, &si, NULL, &g_samp_shadow));
    }
    if (!layouts_create()) return false;

    /*per frame: scene (opaque, effects) + UI sets; shared: one set per
     * post-processing pass*/
    enum { POST_SETS = BLOOM_LEVELS * 2 + 2 + 3 };
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, FRAMES * 2},
                                  {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, FRAMES * 11 + POST_SETS * 2}};
    VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = FRAMES * 3 + POST_SETS,
                                      .poolSizeCount = 2, .pPoolSizes = ps};
    VK_CHECK(vkCreateDescriptorPool(g_dev, &dpi, NULL, &g_dpool));
    {
        VkDescriptorSetLayout ls[POST_SETS];
        VkDescriptorSet sets[POST_SETS];
        for (int i = 0; i < POST_SETS; i++) ls[i] = g_dsl_post;
        VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = g_dpool,
                                           .descriptorSetCount = POST_SETS, .pSetLayouts = ls};
        VK_CHECK(vkAllocateDescriptorSets(g_dev, &dai, sets));
        for (int i = 0; i < BLOOM_LEVELS; i++) {
            g_set_down[i] = sets[i];
            g_set_up[i] = sets[BLOOM_LEVELS + i];
        }
        g_set_comp = sets[BLOOM_LEVELS * 2];
        g_set_fxaa = sets[BLOOM_LEVELS * 2 + 1];
        g_set_ssao = sets[BLOOM_LEVELS * 2 + 2];
        g_set_ssao_blur = sets[BLOOM_LEVELS * 2 + 3];
        g_set_ao_apply = sets[BLOOM_LEVELS * 2 + 4];
    }

    if (!swapchain_create()) {
        int w = 0, h = 0;
        glfwGetFramebufferSize(win, &w, &h);
        if (w > 0 && h > 0) return false; /*a real failure, not a minimised window*/
        g_need_recreate = true;
    }
    if (!pipelines_create()) return false;

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
        VkDescriptorSetLayout ls[3] = {g_dsl_scene, g_dsl_scene, g_dsl_ui};
        VkDescriptorSet sets[3];
        VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = g_dpool,
                                           .descriptorSetCount = 3, .pSetLayouts = ls};
        VK_CHECK(vkAllocateDescriptorSets(g_dev, &dai, sets));
        f->set_scene = sets[0];
        f->set_trans = sets[1];
        f->set_ui = sets[2];
    }

    /*1x1 placeholder so every descriptor is valid before the first upload*/
    if (!image_create(&g_tex_dummy, 1, 1, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                      VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT))
        return false;
    VkCommandBuffer cmd = one_shot_begin();
    const uint8_t white[4] = {255, 255, 255, 255};
    Buffer st;
    record_image_upload(cmd, NULL, &g_tex_dummy, white, 4, true, false, &st);
    /*placeholders for the shadow maps and the scene depth when those
     * effects are off (never read: the uniforms turn them off)*/
    if (!image_create(&g_dummy_depth, 1, 1, 1, 1, g_shadow_fmt, VK_IMAGE_USAGE_SAMPLED_BIT, VK_SAMPLE_COUNT_1_BIT,
                      VK_IMAGE_ASPECT_DEPTH_BIT) ||
        !cube_create(&g_dummy_cube, NULL, 1, VK_FORMAT_R16_SFLOAT, VK_IMAGE_USAGE_SAMPLED_BIT))
        return false;
    barrier_image(cmd, g_dummy_depth.img, VK_IMAGE_ASPECT_DEPTH_BIT, 1, 1, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    barrier_image(cmd, g_dummy_cube.img, VK_IMAGE_ASPECT_COLOR_BIT, 1, 6, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    one_shot_end(cmd);
    buffer_destroy(&st);
    return true;
}

static void vk_upload_materials(const uint8_t *rgba, int size, int layers) {
    vkDeviceWaitIdle(g_dev);
    image_destroy(&g_tex_mat);
    int mips = 1;
    for (int s = size; s > 1; s >>= 1) mips++;
    if (!image_create(&g_tex_mat, size, size, layers, mips, VK_FORMAT_R8G8B8A8_SRGB,
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
    float pc[16] = {(float)pass};
    vkCmdPushConstants(cmd, g_pl_scene, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64, pc);
    VkDeviceSize o = (VkDeviceSize)off;
    vkCmdBindVertexBuffers(cmd, 0, 1, &f->stream.buf, &o);
    vkCmdDraw(cmd, n, 1, 0, 0);
}

static bool sun_shadows_on(const SceneFrame *s) { return g_sun_map.img && s->u.sun_dir[3] > 0.0f; }
static bool torch_shadows_on(const SceneFrame *s) {
    return g_cube.img && s->u.shadow[2] > 0.5f && s->u.misc[0] >= 1.0f && s->u.lights[0].pos[3] > 0.5f;
}

static void update_descriptors(Frame *f, const SceneFrame *s) {
    VkDescriptorBufferInfo bi = {f->ubo.buf, 0, sizeof(SceneUniforms)};
    VkDescriptorImageInfo mat = {g_samp_mat, g_tex_mat.view ? g_tex_mat.view : VK_NULL_HANDLE, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo expl = {g_samp_nearest, g_tex_expl.view ? g_tex_expl.view : g_tex_dummy.view,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo atl = {g_samp_linear, g_tex_atlas.view ? g_tex_atlas.view : g_tex_dummy.view,
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    /*the opaque pass writes the scene depth: it reads the placeholder*/
    VkDescriptorImageInfo nodepth = {g_samp_nearest, g_dummy_depth.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo depth = {g_samp_nearest, g_zbuf.view ? g_zbuf.view : g_dummy_depth.view,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo sun = {g_samp_shadow, sun_shadows_on(s) ? g_sun_map.view : g_dummy_depth.view,
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo cube = {g_samp_linear, torch_shadows_on(s) ? g_cube.view : g_dummy_cube.view,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    if (!g_tex_mat.view) {
        VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = f->set_ui, .dstBinding = 0,
                                  .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                  .pImageInfo = &atl};
        vkUpdateDescriptorSets(g_dev, 1, &w, 0, NULL);
        return;
    }
    VkWriteDescriptorSet w[13];
    int n = 0;
    VkDescriptorSet sets[2] = {f->set_scene, f->set_trans};
    for (int k = 0; k < 2; k++) {
        const VkDescriptorImageInfo *imgs[5] = {&mat, &expl, k == 0 ? &nodepth : &depth, &sun, &cube};
        w[n++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[k], .dstBinding = 0,
                                        .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                        .pBufferInfo = &bi};
        for (int i = 0; i < 5; i++)
            w[n++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = sets[k],
                                            .dstBinding = (uint32_t)(i + 1), .descriptorCount = 1,
                                            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = imgs[i]};
    }
    w[n++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = f->set_ui, .dstBinding = 0,
                                    .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                    .pImageInfo = &atl};
    vkUpdateDescriptorSets(g_dev, (uint32_t)n, w, 0, NULL);
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

/* ---- passes ---- */

#define STAGE_RW (VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT)

/*Previous content not needed (fully overwritten): the barrier waits for
 * the previous frame's reads and writes of the image.*/
static void to_attachment(VkCommandBuffer cmd, const Image *im) {
    barrier_image(cmd, im->img, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, STAGE_RW, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
}

static void to_sampled(VkCommandBuffer cmd, const Image *im) {
    barrier_image(cmd, im->img, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
}

static void begin_color(VkCommandBuffer cmd, VkImageView view, VkExtent2D ext, VkAttachmentLoadOp load, const float *clear) {
    VkRenderingAttachmentInfo ca = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = view,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = load,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
    };
    if (clear) memcpy(ca.clearValue.color.float32, clear, sizeof(float) * 4);
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, ext}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cmd, &ri);
    VkViewport vp = {0, 0, (float)ext.width, (float)ext.height, 0.0f, 1.0f};
    VkRect2D sc = {{0, 0}, ext};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
}

static VkExtent2D image_ext(const Image *im) { return (VkExtent2D){(uint32_t)im->w, (uint32_t)im->h}; }

static void post_draw(VkCommandBuffer cmd, VkPipeline p, VkDescriptorSet set, const float pc[16]) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pl_post, 0, 1, &set, 0, NULL);
    vkCmdPushConstants(cmd, g_pl_post, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64, pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

static VkImageAspectFlags depth_aspect(VkFormat f) {
    return (f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D16_UNORM_S8_UINT)
               ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
               : VK_IMAGE_ASPECT_DEPTH_BIT;
}

#define STAGE_DEPTH (VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT)

static void depth_to_attachment(VkCommandBuffer cmd, const Image *im, uint32_t layers) {
    barrier_image(cmd, im->img, depth_aspect(im->fmt), 1, layers, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                  STAGE_DEPTH | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, STAGE_DEPTH,
                  VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
}

static void depth_to_sampled(VkCommandBuffer cmd, const Image *im, VkImageLayout from, VkPipelineStageFlags stage,
                             VkAccessFlags access) {
    barrier_image(cmd, im->img, depth_aspect(im->fmt), 1, 1, from, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, stage, access,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
}

static void begin_depth_only(VkCommandBuffer cmd, const Image *depth, VkImageView color, VkExtent2D ext) {
    VkRenderingAttachmentInfo ca = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = color,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue.color = {{1.0f, 1.0f, 1.0f, 1.0f}},
    };
    VkRenderingAttachmentInfo da = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = depth->view,
        .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = color ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue.depthStencil = {1.0f, 0},
    };
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, ext}, .layerCount = 1,
                          .colorAttachmentCount = color ? 1u : 0u, .pColorAttachments = &ca, .pDepthAttachment = &da};
    vkCmdBeginRendering(cmd, &ri);
    VkViewport vp = {0, 0, (float)ext.width, (float)ext.height, 0.0f, 1.0f};
    VkRect2D sc = {{0, 0}, ext};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
}

/*World chunks whose bounds touch the square (cx, cz) +- r, then the
 * dynamic geometry (entities). The vertex buffers: world, then stream.*/
static void draw_casters(Frame *f, VkCommandBuffer cmd, const SceneFrame *s, float cx, float cz, float r,
                         VkDeviceSize dyn_off, bool have_dyn) {
    if (g_world_count && g_world.buf) {
        VkDeviceSize o = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &g_world.buf, &o);
        for (uint32_t i = 0; i < s->world_nranges; i++) {
            const GfxRange *rg = &s->world_ranges[i];
            if (rg->x1 < cx - r || rg->x0 > cx + r || rg->z1 < cz - r || rg->z0 > cz + r) continue;
            if (rg->first + rg->count > g_world_count) continue;
            vkCmdDraw(cmd, rg->count, 1, rg->first, 0);
        }
    }
    if (have_dyn) {
        vkCmdBindVertexBuffers(cmd, 0, 1, &f->stream.buf, &dyn_off);
        vkCmdDraw(cmd, s->dyn_count, 1, 0, 0);
    }
}

static void push_matrix(VkCommandBuffer cmd, const m4 *m) {
    vkCmdPushConstants(cmd, g_pl_scene, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64, m->m);
}

/*Sun (orthographic) and torch (cube of 6 faces) shadow maps.*/
static void pass_shadows(Frame *f, VkCommandBuffer cmd, const SceneFrame *s) {
    bool sun = sun_shadows_on(s), torch = torch_shadows_on(s);
    if (!sun && !torch) return;
    /*the entities, once, in this frame's stream*/
    long long dyn = s->dyn_count ? stream_push(f, s->dyn, (size_t)s->dyn_count * sizeof(SVert)) : -1;
    if (sun) {
        depth_to_attachment(cmd, &g_sun_map, 1);
        begin_depth_only(cmd, &g_sun_map, VK_NULL_HANDLE, image_ext(&g_sun_map));
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_p_shadow_sun);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pl_scene, 0, 1, &f->set_scene, 0, NULL);
        m4 vp;
        memcpy(vp.m, s->u.sun_vp, sizeof(vp.m));
        push_matrix(cmd, &vp);
        draw_casters(f, cmd, s, s->u.player_pos[0], s->u.player_pos[2], 40.0f, dyn < 0 ? 0 : (VkDeviceSize)dyn, dyn >= 0);
        vkCmdEndRendering(cmd);
        depth_to_sampled(cmd, &g_sun_map, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, STAGE_DEPTH,
                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    }
    if (torch) {
        const GfxLight *L = &s->u.lights[0];
        v3 p = v3m(L->pos[0], L->pos[1], L->pos[2]);
        float r = L->pos[3];
        /*face order and up vectors of the cube map convention (+X -X +Y -Y +Z -Z)*/
        static const float dirs[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
        static const float ups[6][3] = {{0, -1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}, {0, -1, 0}, {0, -1, 0}};
        m4 proj = m4perspective(1.5707963f, 1.0f, 0.05f, r);
        barrier_image(cmd, g_cube.img, VK_IMAGE_ASPECT_COLOR_BIT, 1, 6, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, STAGE_RW, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        for (int face = 0; face < 6; face++) {
            depth_to_attachment(cmd, &g_cube_depth, 1);
            begin_depth_only(cmd, &g_cube_depth, g_cube_faces[face], image_ext(&g_cube_depth));
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_p_shadow_cube);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pl_scene, 0, 1, &f->set_scene, 0, NULL);
            v3 d = v3m(dirs[face][0], dirs[face][1], dirs[face][2]);
            m4 vp = m4mul(proj, m4lookat(p, v3add(p, d), v3m(ups[face][0], ups[face][1], ups[face][2])));
            push_matrix(cmd, &vp);
            draw_casters(f, cmd, s, p.x, p.z, r, dyn < 0 ? 0 : (VkDeviceSize)dyn, dyn >= 0);
            vkCmdEndRendering(cmd);
        }
        barrier_image(cmd, g_cube.img, VK_IMAGE_ASPECT_COLOR_BIT, 1, 6, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    }
}

static void begin_scene(VkCommandBuffer cmd, bool first, bool resolve, const SceneFrame *s) {
    VkClearValue cv = {0};
    /*the clear colour is the fog colour (sRGB): the target is linear*/
    for (int i = 0; i < 3; i++) {
        float c = s->clear_color[i];
        cv.color.float32[i] = c * (c * (c * 0.305306011f + 0.682171111f) + 0.012522878f);
    }
    cv.color.float32[3] = 1.0f;
    bool ms = g_hdr_ms.img != VK_NULL_HANDLE;
    VkRenderingAttachmentInfo ca = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = ms ? g_hdr_ms.view : g_hdr.view,
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .resolveMode = ms && resolve ? VK_RESOLVE_MODE_AVERAGE_BIT : VK_RESOLVE_MODE_NONE,
        .resolveImageView = ms && resolve ? g_hdr.view : VK_NULL_HANDLE,
        .resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .loadOp = first ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp = ms && resolve ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE,
        .clearValue = cv,
    };
    /*first pass: with MSAA the depth is resolved (sample 0) into the
     * single-sample copy that the effects read*/
    VkRenderingAttachmentInfo da = {
        .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
        .imageView = g_depth.view,
        .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        .resolveMode = ms && first ? VK_RESOLVE_MODE_SAMPLE_ZERO_BIT : VK_RESOLVE_MODE_NONE,
        .resolveImageView = ms && first ? g_zbuf.view : VK_NULL_HANDLE,
        .resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        .loadOp = first ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp = first ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .clearValue.depthStencil = {1.0f, 0},
    };
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, g_rext}, .layerCount = 1,
                          .colorAttachmentCount = 1, .pColorAttachments = &ca, .pDepthAttachment = &da};
    vkCmdBeginRendering(cmd, &ri);
    VkViewport vp = {0, 0, (float)g_rext.width, (float)g_rext.height, 0.0f, 1.0f};
    VkRect2D sc = {{0, 0}, g_rext};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
}

/*Opaque world and entities; leaves the scene depth sampled in g_zbuf.*/
static void pass_opaque(Frame *f, VkCommandBuffer cmd, const SceneFrame *s) {
    bool ms = g_hdr_ms.img != VK_NULL_HANDLE;
    to_attachment(cmd, ms ? &g_hdr_ms : &g_hdr);
    depth_to_attachment(cmd, &g_depth, 1);
    if (ms) /*depth resolves are done in the colour attachment output stage*/
        barrier_image(cmd, g_zbuf.img, depth_aspect(g_zbuf.fmt), 1, 1, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    begin_scene(cmd, true, false, s);
    if (g_tex_mat.view) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pl_scene, 0, 1, &f->set_scene, 0, NULL);
        if (g_world_count && g_world.buf) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_p_opaque);
            float pc[16] = {0};
            vkCmdPushConstants(cmd, g_pl_scene, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64, pc);
            VkDeviceSize o = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &g_world.buf, &o);
            vkCmdDraw(cmd, g_world_count, 1, 0, 0);
        }
        draw_scene_stream(f, cmd, g_p_opaque, 0, s->dyn, s->dyn_count);
    }
    vkCmdEndRendering(cmd);
    VkImageAspectFlags da = depth_aspect(g_depth_fmt);
    if (ms) {
        depth_to_sampled(cmd, &g_zbuf, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | STAGE_DEPTH,
                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    } else {
        /*no MSAA: copy the depth (the effects keep it as their depth test)*/
        barrier_image(cmd, g_depth.img, da, 1, 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, STAGE_DEPTH, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        barrier_image(cmd, g_zbuf.img, da, 1, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkImageCopy rg = {.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1},
                          .dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1},
                          .extent = {g_rext.width, g_rext.height, 1}};
        vkCmdCopyImage(cmd, g_depth.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_zbuf.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &rg);
        depth_to_sampled(cmd, &g_zbuf, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_ACCESS_TRANSFER_WRITE_BIT);
        barrier_image(cmd, g_depth.img, da, 1, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, STAGE_DEPTH,
                      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
    }
}

/*Ambient occlusion at half resolution, then its edge-aware blur.*/
static void pass_ssao(VkCommandBuffer cmd, const SceneFrame *s) {
    m4 vp, inv;
    memcpy(vp.m, s->u.viewproj, sizeof(vp.m));
    if (!m4invert(&vp, &inv)) return;
    to_attachment(cmd, &g_ao);
    begin_color(cmd, g_ao.view, image_ext(&g_ao), VK_ATTACHMENT_LOAD_OP_DONT_CARE, NULL);
    struct {
        float inv_vp[16];
        float cam[4];
        float params[4];
    } pc;
    memcpy(pc.inv_vp, inv.m, sizeof(pc.inv_vp));
    pc.cam[0] = s->u.cam_pos[0];
    pc.cam[1] = s->u.cam_pos[1];
    pc.cam[2] = s->u.cam_pos[2];
    /*pixels of the AO image per world unit at distance 1 (vertical fov 0.70)*/
    pc.cam[3] = (float)g_ao.h * 0.5f / tanf(0.35f);
    pc.params[0] = 1.0f / (float)g_zbuf.w;
    pc.params[1] = 1.0f / (float)g_zbuf.h;
    pc.params[2] = 0.55f; /*radius in tiles*/
    pc.params[3] = 1.1f;  /*intensity*/
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_p_ssao);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pl_ssao, 0, 1, &g_set_ssao, 0, NULL);
    vkCmdPushConstants(cmd, g_pl_ssao, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    to_sampled(cmd, &g_ao);
    to_attachment(cmd, &g_ao_blur);
    begin_color(cmd, g_ao_blur.view, image_ext(&g_ao_blur), VK_ATTACHMENT_LOAD_OP_DONT_CARE, NULL);
    float bp[16] = {1.0f / (float)g_ao.w, 1.0f / (float)g_ao.h, s->u.proj[0], s->u.proj[1]};
    post_draw(cmd, g_p_ssao_blur, g_set_ssao_blur, bp);
    vkCmdEndRendering(cmd);
    to_sampled(cmd, &g_ao_blur);
}

/*Ambient occlusion on the opaque scene, then transparent and additive
 * effects (soft against the depth); resolves the MSAA image.*/
static void pass_effects(Frame *f, VkCommandBuffer cmd, const SceneFrame *s, bool ao) {
    bool ms = g_hdr_ms.img != VK_NULL_HANDLE;
    VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                          .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                          .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | STAGE_DEPTH,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | STAGE_DEPTH, 0, 1, &mb, 0, NULL, 0, NULL);
    if (ms) to_attachment(cmd, &g_hdr);
    begin_scene(cmd, false, true, s);
    if (ao) {
        float pc[16] = {0};
        post_draw(cmd, g_p_ao_apply, g_set_ao_apply, pc);
    }
    if (g_tex_mat.view) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pl_scene, 0, 1, &f->set_trans, 0, NULL);
        draw_scene_stream(f, cmd, g_p_trans, 1, s->trans, s->trans_count);
        draw_scene_stream(f, cmd, g_p_add, 2, s->add, s->add_count);
    }
    vkCmdEndRendering(cmd);
    to_sampled(cmd, &g_hdr);
}

static void pass_bloom(VkCommandBuffer cmd) {
    for (int i = 0; i < g_nbloom; i++) {
        const Image *src = i == 0 ? &g_hdr : &g_bloom[i - 1];
        to_attachment(cmd, &g_bloom[i]);
        begin_color(cmd, g_bloom[i].view, image_ext(&g_bloom[i]), VK_ATTACHMENT_LOAD_OP_DONT_CARE, NULL);
        float pc[16] = {1.0f / (float)src->w, 1.0f / (float)src->h, i == 0 ? 1.0f : 0.0f, 1.0f, 0.6f};
        post_draw(cmd, g_p_down, g_set_down[i], pc);
        vkCmdEndRendering(cmd);
        to_sampled(cmd, &g_bloom[i]);
    }
    for (int i = g_nbloom - 2; i >= 0; i--) {
        const Image *src = &g_bloom[i + 1];
        /*keep the downsampled content: the upsample is added onto it*/
        barrier_image(cmd, g_bloom[i].img, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                      VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
        begin_color(cmd, g_bloom[i].view, image_ext(&g_bloom[i]), VK_ATTACHMENT_LOAD_OP_LOAD, NULL);
        float pc[16] = {1.0f / (float)src->w, 1.0f / (float)src->h, 1.0f, 1.0f};
        post_draw(cmd, g_p_up, g_set_up[i], pc);
        vkCmdEndRendering(cmd);
        to_sampled(cmd, &g_bloom[i]);
    }
}

static void composite_pc(const SceneFrame *s, int w, int h, bool bloom, float pc[16]) {
    float aspect = (float)w / (float)(h > 0 ? h : 1);
    const GfxPost *po = &s->post;
    float v[16] = {
        s->screen_tint[0], s->screen_tint[1], s->screen_tint[2], s->screen_tint[3],
        s->vignette, aspect > 1 ? 1.0f : aspect, aspect > 1 ? 1.0f / aspect : 1.0f, po->exposure,
        po->bloom, po->saturation, po->contrast, bloom ? 1.0f : 0.0f,
        po->grade[0], po->grade[1], po->grade[2], 0,
    };
    memcpy(pc, v, sizeof(v));
}

static void vk_render(const SceneFrame *s, const UIFrame *ui, int w, int h) {
    if (w <= 0 || h <= 0) return;
    VkSampleCountFlagBits want = g_rs.aa == GFX_AA_MSAA ? g_msaa_max : VK_SAMPLE_COUNT_1_BIT;
    if (want != g_samples) {
        /*the sample count is baked into the scene pipelines*/
        vkDeviceWaitIdle(g_dev);
        g_samples = want;
        pipelines_destroy();
        if (!pipelines_create()) fprintf(stderr, "[VK] could not rebuild the pipelines\n");
        g_need_recreate = true;
    }
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
    update_descriptors(f, s);
    if (s->draw_scene) {
        SceneUniforms *u = f->ubo.map;
        memcpy(u, &s->u, sizeof(SceneUniforms));
        /*what this renderer turns on: shadow maps, soft particles (with SSAO)*/
        u->shadow[0] = sun_shadows_on(s) ? 1.0f : 0.0f;
        u->shadow[1] = torch_shadows_on(s) ? 1.0f : 0.0f;
        u->proj[2] = g_rs.ssao ? (float)g_rext.width : 0.0f;
        u->proj[3] = (float)g_rext.height;
    }

    /*--- rendering ---*/
    VkImage swap_img = g_images[idx];
    bool scene = s->draw_scene && g_hdr.img;
    bool bloom = scene && g_rs.bloom && g_nbloom > 0 && s->post.bloom > 0;
    bool fxaa = scene && g_ldr.img;
    if (scene) {
        pass_shadows(f, cmd, s);
        pass_opaque(f, cmd, s);
        bool ao = g_rs.ssao && g_ao.img;
        if (ao) pass_ssao(cmd, s);
        pass_effects(f, cmd, s, ao);
        if (bloom) pass_bloom(cmd);
        else if (g_nbloom) {
            /*the composite set samples bloom[0]: give it a valid layout*/
            to_attachment(cmd, &g_bloom[0]);
            float zero[4] = {0, 0, 0, 0};
            begin_color(cmd, g_bloom[0].view, image_ext(&g_bloom[0]), VK_ATTACHMENT_LOAD_OP_CLEAR, zero);
            vkCmdEndRendering(cmd);
            to_sampled(cmd, &g_bloom[0]);
        }
    }
    float pc[16];
    if (fxaa) {
        to_attachment(cmd, &g_ldr);
        begin_color(cmd, g_ldr.view, g_extent, VK_ATTACHMENT_LOAD_OP_DONT_CARE, NULL);
        composite_pc(s, w, h, bloom, pc);
        post_draw(cmd, g_p_comp, g_set_comp, pc);
        vkCmdEndRendering(cmd);
        to_sampled(cmd, &g_ldr);
    }
    barrier_image(cmd, swap_img, VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    float clear[4] = {s->clear_color[0], s->clear_color[1], s->clear_color[2], 1.0f};
    begin_color(cmd, g_views[idx], g_extent, scene ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_CLEAR, clear);
    if (fxaa) {
        float fp[16] = {1.0f / (float)g_extent.width, 1.0f / (float)g_extent.height};
        post_draw(cmd, g_p_fxaa, g_set_fxaa, fp);
    } else if (scene) {
        composite_pc(s, w, h, bloom, pc);
        post_draw(cmd, g_p_comp, g_set_comp, pc);
    }
    if (ui && ui->count) {
        long long off = stream_push(f, ui->v, (size_t)ui->count * sizeof(UIVert));
        if (off >= 0) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_p_ui);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pl_ui, 0, 1, &f->set_ui, 0, NULL);
            float upc[16] = {(float)w, (float)h};
            vkCmdPushConstants(cmd, g_pl_ui, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64, upc);
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
    image_destroy(&g_dummy_depth);
    image_destroy(&g_dummy_cube);
    swapchain_destroy();
    if (g_swap) vkDestroySwapchainKHR(g_dev, g_swap, NULL);
    pipelines_destroy();
    if (g_pl_scene) vkDestroyPipelineLayout(g_dev, g_pl_scene, NULL);
    if (g_pl_ui) vkDestroyPipelineLayout(g_dev, g_pl_ui, NULL);
    if (g_pl_post) vkDestroyPipelineLayout(g_dev, g_pl_post, NULL);
    if (g_pl_ssao) vkDestroyPipelineLayout(g_dev, g_pl_ssao, NULL);
    if (g_dsl_scene) vkDestroyDescriptorSetLayout(g_dev, g_dsl_scene, NULL);
    if (g_dsl_ui) vkDestroyDescriptorSetLayout(g_dev, g_dsl_ui, NULL);
    if (g_dsl_post) vkDestroyDescriptorSetLayout(g_dev, g_dsl_post, NULL);
    if (g_dpool) vkDestroyDescriptorPool(g_dev, g_dpool, NULL);
    VkSampler ss[4] = {g_samp_mat, g_samp_linear, g_samp_nearest, g_samp_shadow};
    for (int i = 0; i < 4; i++)
        if (ss[i]) vkDestroySampler(g_dev, ss[i], NULL);
    if (g_pool) vkDestroyCommandPool(g_dev, g_pool, NULL);
    vkDestroyDevice(g_dev, NULL);
    vkDestroySurfaceKHR(g_inst, g_surface, NULL);
    vkDestroyInstance(g_inst, NULL);
    g_dev = VK_NULL_HANDLE;
    g_surface = VK_NULL_HANDLE;
    g_inst = VK_NULL_HANDLE;
    g_swap = VK_NULL_HANDLE;
    g_pl_scene = g_pl_ui = g_pl_post = g_pl_ssao = VK_NULL_HANDLE;
    g_dsl_scene = g_dsl_ui = g_dsl_post = VK_NULL_HANDLE;
    g_dpool = VK_NULL_HANDLE;
    g_samp_mat = g_samp_linear = g_samp_nearest = g_samp_shadow = VK_NULL_HANDLE;
    g_pool = VK_NULL_HANDLE;
    world_ver = atlas_ver = expl_ver = 0xFFFFFFFFu;
    g_world_count = 0;
}

static const char *vk_device_name(void) { return g_device; }

/*Render scale and FXAA change the targets; MSAA also the scene pipelines
 * (handled at the start of the next frame).*/
static void vk_configure(const RenderSettings *rs) {
    if (rs->aa != g_rs.aa || rs->render_scale != g_rs.render_scale || rs->shadows != g_rs.shadows || rs->ssao != g_rs.ssao)
        g_need_recreate = true;
    g_rs = *rs;
}

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
    vk_configure,
    GFX_CAP_MSAA | GFX_CAP_SHADOWS | GFX_CAP_SSAO,
};
