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

/*Renderer-independent frame description.
 *
 * DragonGL+ splits the client in two halves:
 *   - scene.c / ui.c build, every frame, plain vertex arrays (world,
 *     entities, transparent and additive effects, 2D interface) plus a
 *     block of uniforms (camera, lights, fog, time);
 *   - a backend (gfx_gl.c = OpenGL 3.3, gfx_vk.c = Vulkan 1.3) only
 *     uploads and draws them with the SAME shading model.
 * Both backends therefore look identical and every visual feature is
 * written once.*/
#ifndef GFX_H
#define GFX_H

#include <stdbool.h>
#include <stdint.h>

#define GFX_MAX_LIGHTS 32

/*World vertex (48 bytes).
 *  layer : material layer in the texture array (textures.h, MAT_*)
 *  glow  : 0..1 emissive amount (lava, crystals, eyes...)
 *  flags : bitfield as float (exact for small ints):
 *          1 = cut-away vertex (top of a wall: lowered when it hides
 *              the player from the camera)
 *          2 = water surface (vertex wave + scrolling UV)
 *          4 = lava / flowing emissive (scrolling UV)
 *          8 = foliage (wind sway)
 *         16 = unlit (ignore lights, e.g. UI-like markers)
 *  col   : RGBA8 tint; alpha is used by the transparent pass*/
typedef struct {
    float pos[3];
    float nrm[3];
    float uv[2];
    float layer;
    float glow;
    float flags;
    uint8_t col[4];
} SVert;

#define SV_CUT 1
#define SV_WATER 2
#define SV_LAVA 4
#define SV_FOLIAGE 8
#define SV_UNLIT 16

/*2D interface vertex (20 bytes). Pixel coordinates, origin top-left.
 * The colour multiplies the UI atlas sample (font coverage in alpha,
 * a white texel for solid fills, RGBA for icons).*/
typedef struct {
    float pos[2];
    float uv[2];
    uint8_t col[4];
} UIVert;

typedef struct {
    float pos[4]; /*xyz, w = radius*/
    float col[4]; /*rgb, a = intensity*/
} GfxLight;

/*Uniform block shared by every scene shader (std140-compatible:
 * vec4-aligned members only).*/
typedef struct {
    float viewproj[16];
    float cam_pos[4];      /*xyz, w = time (seconds)*/
    float player_pos[4];   /*xyz, w = vision radius*/
    float ambient[4];      /*rgb, a = memory brightness (explored, unseen)*/
    float fog_color[4];    /*rgb, a = fog density*/
    float cut_dir[4];      /*xz = camera forward on the ground, z unused, w = cut-away on/off*/
    float misc[4];         /*x = light count, y = explored map size, z = saturation, w = exposure*/
    GfxLight lights[GFX_MAX_LIGHTS];
} SceneUniforms;

/*Explored-tiles mask (R8, MAP_WIDTH x MAP_HEIGHT): fragments on tiles
 * never seen are not drawn, tiles seen but out of view are drawn as a
 * dim "memory". Uploaded only when it changes (version).*/
typedef struct {
    const uint8_t *data;
    int w, h;
    uint32_t version;
} GfxExplored;

typedef struct {
    bool draw_scene;
    const SVert *world; uint32_t world_count; uint32_t world_version;
    const SVert *dyn;   uint32_t dyn_count;   /*opaque, rebuilt each frame*/
    const SVert *trans; uint32_t trans_count; /*alpha blended, no depth write*/
    const SVert *add;   uint32_t add_count;   /*additive, no depth write*/
    SceneUniforms u;
    GfxExplored explored;
    float clear_color[4];
    float screen_tint[4];  /*rgb, a = full-screen overlay (damage flash, fades)*/
    float vignette;        /*0..1 darkening of the screen corners*/
} SceneFrame;

typedef struct {
    const UIVert *v;
    uint32_t count;        /*triangle list*/
    uint32_t atlas_version;/*re-upload the atlas when it changes*/
    const uint8_t *atlas;  /*RGBA8*/
    int atlas_w, atlas_h;
} UIFrame;

typedef struct GLFWwindow GLFWwindow;

typedef struct {
    const char *name;
    /*GLFW window hints for this API (called before glfwCreateWindow).*/
    void (*window_hints)(void);
    bool (*init)(GLFWwindow *win, bool vsync);
    /*Material texture array: layers x (size x size) RGBA8, mipmapped.*/
    void (*upload_materials)(const uint8_t *rgba, int size, int layers);
    void (*render)(const SceneFrame *scene, const UIFrame *ui, int fb_w, int fb_h);
    /*Read back the last presented frame as RGBA8 (top-left origin);
     * returns a malloc'ed buffer or NULL.*/
    uint8_t *(*screenshot)(int *w, int *h);
    void (*set_vsync)(bool on);
    void (*shutdown)(void);
    /*Human-readable GPU/driver line for the settings screen.*/
    const char *(*device_name)(void);
} GfxBackend;

extern const GfxBackend gfx_gl_backend;
extern const GfxBackend gfx_vk_backend;
/*Is a Vulkan driver present at all? (volk can load the loader)*/
bool gfx_vk_available(void);

#endif
