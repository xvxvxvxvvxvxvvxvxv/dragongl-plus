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

/*OpenGL 3.3 core backend.
 *
 * Every GL entry point (1.1 ones included) is loaded at run time through
 * glfwGetProcAddress, so the executable does not link libGL/opengl32 or
 * GLU at all: GLFW opens the system driver itself. That is what lets the
 * portable Linux/Windows packages run without any -dev package or DLL.*/

#include "gfx.h"
#include "shaders.h"
#include <GLFW/glfw3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

/* ---------------------------- loader ---------------------------- */

typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLboolean;
typedef float GLfloat;
typedef char GLchar;
typedef unsigned char GLubyte;
typedef unsigned int GLbitfield;
typedef ptrdiff_t GLsizeiptr;
typedef ptrdiff_t GLintptr;

#ifndef GLAPIENTRY
#ifdef _WIN32
#define GLAPIENTRY __stdcall
#else
#define GLAPIENTRY
#endif
#endif

#define GL_FALSE 0
#define GL_TRUE 1
#define GL_TRIANGLES 0x0004
#define GL_DEPTH_BUFFER_BIT 0x00000100
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_DEPTH_TEST 0x0B71
#define GL_BLEND 0x0BE2
#define GL_CULL_FACE 0x0B44
#define GL_SCISSOR_TEST 0x0C11
#define GL_LEQUAL 0x0203
#define GL_LESS 0x0201
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_ONE 1
#define GL_ZERO 0
#define GL_FLOAT 0x1406
#define GL_UNSIGNED_BYTE 0x1401
#define GL_RGBA 0x1908
#define GL_RED 0x1903
#define GL_R8 0x8229
#define GL_RGBA8 0x8058
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE_2D_ARRAY 0x8C1A
#define GL_TEXTURE0 0x84C0
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_LINEAR 0x2601
#define GL_NEAREST 0x2600
#define GL_LINEAR_MIPMAP_LINEAR 0x2703
#define GL_REPEAT 0x2901
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_TEXTURE_MAX_ANISOTROPY 0x84FE
#define GL_ARRAY_BUFFER 0x8892
#define GL_UNIFORM_BUFFER 0x8A11
#define GL_STATIC_DRAW 0x88E4
#define GL_STREAM_DRAW 0x88E0
#define GL_DYNAMIC_DRAW 0x88E8
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_INFO_LOG_LENGTH 0x8B84
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_INVALID_INDEX 0xFFFFFFFFu
#define GL_FRAMEBUFFER_SRGB 0x8DB9
#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_RENDERBUFFER 0x8D41
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_ATTACHMENT 0x8D00
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_RGBA16F 0x881A
#define GL_HALF_FLOAT 0x140B
#define GL_DEPTH_COMPONENT24 0x81A6
#define GL_SRGB8_ALPHA8 0x8C43
#define GL_MAX_SAMPLES 0x8D57

#define GL_FUNCS(X) \
    X(void, glClear, (GLbitfield)) \
    X(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(void, glEnable, (GLenum)) \
    X(void, glDisable, (GLenum)) \
    X(void, glBlendFunc, (GLenum, GLenum)) \
    X(void, glDepthFunc, (GLenum)) \
    X(void, glDepthMask, (GLboolean)) \
    X(void, glViewport, (GLint, GLint, GLsizei, GLsizei)) \
    X(void, glDrawArrays, (GLenum, GLint, GLsizei)) \
    X(void, glGenTextures, (GLsizei, GLuint *)) \
    X(void, glBindTexture, (GLenum, GLuint)) \
    X(void, glTexParameteri, (GLenum, GLenum, GLint)) \
    X(void, glTexParameterf, (GLenum, GLenum, GLfloat)) \
    X(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
    X(void, glTexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *)) \
    X(void, glPixelStorei, (GLenum, GLint)) \
    X(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *)) \
    X(const GLubyte *, glGetString, (GLenum)) \
    X(GLenum, glGetError, (void)) \
    X(void, glTexImage3D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
    X(void, glActiveTexture, (GLenum)) \
    X(void, glGenerateMipmap, (GLenum)) \
    X(void, glGenBuffers, (GLsizei, GLuint *)) \
    X(void, glBindBuffer, (GLenum, GLuint)) \
    X(void, glBufferData, (GLenum, GLsizeiptr, const void *, GLenum)) \
    X(void, glBufferSubData, (GLenum, GLintptr, GLsizeiptr, const void *)) \
    X(void, glBindBufferBase, (GLenum, GLuint, GLuint)) \
    X(void, glGenVertexArrays, (GLsizei, GLuint *)) \
    X(void, glBindVertexArray, (GLuint)) \
    X(void, glEnableVertexAttribArray, (GLuint)) \
    X(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) \
    X(GLuint, glCreateShader, (GLenum)) \
    X(void, glShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *)) \
    X(void, glCompileShader, (GLuint)) \
    X(void, glGetShaderiv, (GLuint, GLenum, GLint *)) \
    X(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(GLuint, glCreateProgram, (void)) \
    X(void, glAttachShader, (GLuint, GLuint)) \
    X(void, glLinkProgram, (GLuint)) \
    X(void, glGetProgramiv, (GLuint, GLenum, GLint *)) \
    X(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
    X(void, glUseProgram, (GLuint)) \
    X(GLint, glGetUniformLocation, (GLuint, const GLchar *)) \
    X(void, glUniform1i, (GLint, GLint)) \
    X(void, glUniform1f, (GLint, GLfloat)) \
    X(void, glUniform2f, (GLint, GLfloat, GLfloat)) \
    X(void, glUniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat)) \
    X(GLuint, glGetUniformBlockIndex, (GLuint, const GLchar *)) \
    X(void, glUniformBlockBinding, (GLuint, GLuint, GLuint)) \
    X(void, glBindAttribLocation, (GLuint, GLuint, const GLchar *)) \
    X(void, glUniform4fv, (GLint, GLsizei, const GLfloat *)) \
    X(void, glGetIntegerv, (GLenum, GLint *)) \
    X(void, glDeleteTextures, (GLsizei, const GLuint *)) \
    X(void, glGenFramebuffers, (GLsizei, GLuint *)) \
    X(void, glDeleteFramebuffers, (GLsizei, const GLuint *)) \
    X(void, glBindFramebuffer, (GLenum, GLuint)) \
    X(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)) \
    X(void, glFramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint)) \
    X(GLenum, glCheckFramebufferStatus, (GLenum)) \
    X(void, glGenRenderbuffers, (GLsizei, GLuint *)) \
    X(void, glDeleteRenderbuffers, (GLsizei, const GLuint *)) \
    X(void, glBindRenderbuffer, (GLenum, GLuint)) \
    X(void, glRenderbufferStorageMultisample, (GLenum, GLsizei, GLenum, GLsizei, GLsizei)) \
    X(void, glBlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum))

#define DECL(ret, name, args) typedef ret(GLAPIENTRY *PFN_##name) args; static PFN_##name name;
GL_FUNCS(DECL)
#undef DECL

static bool load_gl(void) {
    bool ok = true;
#define LOAD(ret, name, args)                                         \
    name = (PFN_##name)glfwGetProcAddress(#name);                     \
    if (!name) { fprintf(stderr, "[GL] missing %s\n", #name); ok = false; }
    GL_FUNCS(LOAD)
#undef LOAD
    return ok;
}

/* ---------------------------- programs ---------------------------- */

/*A program built from shaders/ (shaders.c pastes the includes) with the
 * locations of its 64-byte parameter block (pc.p0 .. pc.p3).*/
typedef struct {
    GLuint id;
    GLint pc[4];
} Prog;

static GLFWwindow *g_win;
static Prog p_scene, p_ui, p_down, p_up, p_comp, p_fxaa;
static GLuint vao_scene, vao_ui, vao_empty;
static GLuint vbo_world, vbo_dyn, vbo_ui, ubo_scene;
static GLuint tex_mat, tex_atlas, tex_expl;
static uint32_t world_ver = 0xFFFFFFFFu, atlas_ver = 0xFFFFFFFFu, expl_ver = 0xFFFFFFFFu;
static size_t vbo_world_cap, vbo_dyn_cap, vbo_ui_cap;
static char g_device[256];
static RenderSettings g_rs = {GFX_AA_MSAA, true, 1.0f, 0, false};
static int g_max_samples;

static GLuint compile(GLenum type, const char *name) {
    char *src = shader_gl_source(name);
    if (!src) return 0;
    GLuint s = glCreateShader(type);
    const char *cs = src;
    glShaderSource(s, 1, &cs, NULL);
    glCompileShader(s);
    free(src);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "[GL] %s: compile error:\n%s\n", name, log);
        return 0;
    }
    return s;
}

static bool link(Prog *pr, const char *vs, const char *fs) {
    GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) return false;
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(p, sizeof(log), NULL, log);
        fprintf(stderr, "[GL] %s + %s: link error:\n%s\n", vs, fs, log);
        return false;
    }
    pr->id = p;
    for (int i = 0; i < 4; i++) {
        char n[8] = {'p', 'c', '.', 'p', (char)('0' + i), 0};
        pr->pc[i] = glGetUniformLocation(p, n);
    }
    glUseProgram(p);
    /*SAMPLER(binding, ...) in the shaders = texture unit on OpenGL*/
    static const struct { const char *name; int unit; } units[] = {
        {"u_atlas", 0}, {"u_src", 0}, {"u_hdr", 0}, {"u_bloom", 1}, {"u_tex", 1}, {"u_explored", 2},
    };
    for (size_t i = 0; i < sizeof(units) / sizeof(units[0]); i++) {
        GLint loc = glGetUniformLocation(p, units[i].name);
        if (loc >= 0) glUniform1i(loc, units[i].unit);
    }
    GLuint bi = glGetUniformBlockIndex(p, "Scene");
    if (bi != GL_INVALID_INDEX) glUniformBlockBinding(p, bi, 0);
    return true;
}

/*Set the parameter block (members the shader does not use have no
 * location: -1 is ignored by glUniform*).*/
static void push(const Prog *pr, const float p[16]) {
    for (int i = 0; i < 4; i++) glUniform4fv(pr->pc[i], 1, p + i * 4);
}

static void scene_attribs(void) {
    size_t st = sizeof(SVert);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, (GLsizei)st, (void *)offsetof(SVert, pos));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, (GLsizei)st, (void *)offsetof(SVert, nrm));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, (GLsizei)st, (void *)offsetof(SVert, uv));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, (GLsizei)st, (void *)offsetof(SVert, layer));
    glEnableVertexAttribArray(4);
    glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, (GLsizei)st, (void *)offsetof(SVert, glow));
    glEnableVertexAttribArray(5);
    glVertexAttribPointer(5, 1, GL_FLOAT, GL_FALSE, (GLsizei)st, (void *)offsetof(SVert, flags));
    glEnableVertexAttribArray(6);
    glVertexAttribPointer(6, 4, GL_UNSIGNED_BYTE, GL_TRUE, (GLsizei)st, (void *)offsetof(SVert, col));
}

/* ---------------------------- render targets ---------------------------- */

#define BLOOM_LEVELS 6

typedef struct {
    GLuint fbo, tex;
    int w, h;
} Target;

static struct {
    int w, h;            /*3D image size (window size x render scale)*/
    int fw, fh;          /*window size*/
    int samples;         /*0 = no MSAA*/
    GLuint fbo_ms, rb_ms_color, rb_depth;
    Target hdr;          /*resolved scene (RGBA16F), also the target without MSAA*/
    Target ldr;          /*tone-mapped image for FXAA (RGBA8)*/
    Target bloom[BLOOM_LEVELS];
    int nbloom;
} T;

static void target_free(Target *t) {
    if (t->fbo) glDeleteFramebuffers(1, &t->fbo);
    if (t->tex) glDeleteTextures(1, &t->tex);
    memset(t, 0, sizeof(*t));
}

static bool target_make(Target *t, int w, int h, GLenum ifmt, GLenum fmt, GLenum type) {
    t->w = w;
    t->h = h;
    glGenTextures(1, &t->tex);
    glBindTexture(GL_TEXTURE_2D, t->tex);
    glTexImage2D(GL_TEXTURE_2D, 0, (GLint)ifmt, w, h, 0, fmt, type, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &t->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, t->fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->tex, 0);
    return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

static void targets_free(void) {
    if (T.fbo_ms) glDeleteFramebuffers(1, &T.fbo_ms);
    if (T.rb_ms_color) glDeleteRenderbuffers(1, &T.rb_ms_color);
    if (T.rb_depth) glDeleteRenderbuffers(1, &T.rb_depth);
    T.fbo_ms = T.rb_ms_color = T.rb_depth = 0;
    target_free(&T.hdr);
    target_free(&T.ldr);
    for (int i = 0; i < BLOOM_LEVELS; i++) target_free(&T.bloom[i]);
    T.w = T.h = T.fw = T.fh = 0;
}

static bool targets_ensure(int fw, int fh) {
    float sc = g_rs.render_scale < 0.5f ? 0.5f : (g_rs.render_scale > 1.0f ? 1.0f : g_rs.render_scale);
    int w = (int)((float)fw * sc + 0.5f), h = (int)((float)fh * sc + 0.5f);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    int samples = g_rs.aa == GFX_AA_MSAA ? (g_max_samples >= 4 ? 4 : (g_max_samples >= 2 ? 2 : 0)) : 0;
    bool want_ldr = g_rs.aa == GFX_AA_FXAA;
    if (w == T.w && h == T.h && fw == T.fw && fh == T.fh && samples == T.samples && want_ldr == (T.ldr.tex != 0))
        return T.hdr.fbo != 0;
    targets_free();
    T.w = w;
    T.h = h;
    T.fw = fw;
    T.fh = fh;
    T.samples = samples;
    bool ok = target_make(&T.hdr, w, h, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT);
    glGenRenderbuffers(1, &T.rb_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, T.rb_depth);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH_COMPONENT24, w, h);
    if (samples) {
        glGenRenderbuffers(1, &T.rb_ms_color);
        glBindRenderbuffer(GL_RENDERBUFFER, T.rb_ms_color);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA16F, w, h);
        glGenFramebuffers(1, &T.fbo_ms);
        glBindFramebuffer(GL_FRAMEBUFFER, T.fbo_ms);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, T.rb_ms_color);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, T.rb_depth);
        ok = ok && glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, T.hdr.fbo);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, T.rb_depth);
        ok = ok && glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    }
    if (want_ldr) ok = ok && target_make(&T.ldr, fw, fh, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
    T.nbloom = 0;
    int bw = w / 2, bh = h / 2;
    while (T.nbloom < BLOOM_LEVELS && bw >= 8 && bh >= 8) {
        ok = ok && target_make(&T.bloom[T.nbloom], bw, bh, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT);
        T.nbloom++;
        bw /= 2;
        bh /= 2;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (!ok) fprintf(stderr, "[GL] could not create the render targets (%dx%d)\n", w, h);
    return ok;
}

/* ---------------------------- init ---------------------------- */

static void gl_hints(void) {
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    glfwWindowHint(GLFW_SAMPLES, 0); /*the 3D image has its own (multisampled) target*/
    glfwWindowHint(GLFW_DEPTH_BITS, 0);
}

static bool gl_init(GLFWwindow *win, bool vsync) {
    g_win = win;
    glfwMakeContextCurrent(win);
    if (!load_gl()) return false;
    glfwSwapInterval(vsync ? 1 : 0);
    /*short form for the UI: "OpenGL 4.6 - GeForce RTX 3060" (driver details dropped)*/
    const char *ver = (const char *)glGetString(GL_VERSION), *ren = (const char *)glGetString(GL_RENDERER);
    int vlen = 0, rlen = 0;
    while (ver && ver[vlen] && ver[vlen] != ' ' && vlen < 8) vlen++;
    while (ren && ren[rlen] && ren[rlen] != '(' && ren[rlen] != '/' && rlen < 60) rlen++;
    while (rlen > 0 && ren[rlen - 1] == ' ') rlen--;
    snprintf(g_device, sizeof(g_device), "OpenGL %.*s - %.*s", vlen, ver ? ver : "?", rlen, ren ? ren : "?");
    fprintf(stderr, "[GL] %s / %s\n", ver ? ver : "?", ren ? ren : "?");
    glGetIntegerv(GL_MAX_SAMPLES, &g_max_samples);

    if (!link(&p_scene, "scene.vert", "scene.frag") || !link(&p_ui, "ui.vert", "ui.frag") ||
        !link(&p_down, "fullscreen.vert", "bloom_down.frag") || !link(&p_up, "fullscreen.vert", "bloom_up.frag") ||
        !link(&p_comp, "fullscreen.vert", "composite.frag") || !link(&p_fxaa, "fullscreen.vert", "fxaa.frag"))
        return false;

    glGenBuffers(1, &ubo_scene);
    glBindBuffer(GL_UNIFORM_BUFFER, ubo_scene);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(SceneUniforms), NULL, GL_DYNAMIC_DRAW);

    glGenVertexArrays(1, &vao_scene);
    glBindVertexArray(vao_scene);
    glGenBuffers(1, &vbo_world);
    glGenBuffers(1, &vbo_dyn);
    glGenVertexArrays(1, &vao_ui);
    glBindVertexArray(vao_ui);
    glGenBuffers(1, &vbo_ui);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_ui);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(UIVert), (void *)offsetof(UIVert, pos));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(UIVert), (void *)offsetof(UIVert, uv));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(UIVert), (void *)offsetof(UIVert, col));
    glGenVertexArrays(1, &vao_empty);
    glBindVertexArray(0);

    glGenTextures(1, &tex_mat);
    glGenTextures(1, &tex_atlas);
    glGenTextures(1, &tex_expl);
    glBindTexture(GL_TEXTURE_2D, tex_expl);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return glGetError() == 0;
}

static void gl_upload_materials(const uint8_t *rgba, int size, int layers) {
    glBindTexture(GL_TEXTURE_2D_ARRAY, tex_mat);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_SRGB8_ALPHA8, size, size, layers, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameterf(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_ANISOTROPY, 8.0f);
    glGetError(); /*anisotropy may be unsupported: ignore*/
}

static void upload_stream(GLuint vbo, size_t *cap, const void *data, size_t bytes) {
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    if (bytes > *cap) *cap = bytes + bytes / 2;
    /*orphan: let the driver hand us fresh storage*/
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)*cap, NULL, GL_STREAM_DRAW);
    if (bytes) glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)bytes, data);
}

static void draw_stream(const SVert *v, uint32_t n) {
    if (!n) return;
    upload_stream(vbo_dyn, &vbo_dyn_cap, v, (size_t)n * sizeof(SVert));
    scene_attribs();
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)n);
}

/*Screenshot protocol (see gfx.h): the first call arms a capture of the
 * next rendered frame (read back BEFORE the buffer swap, when the back
 * buffer is still defined) and returns NULL; a later call returns it.*/
static bool g_shot_armed;
static uint8_t *g_shot;
static int g_shot_w, g_shot_h;

static void capture_backbuffer(int W, int H) {
    uint8_t *buf = malloc((size_t)W * H * 4);
    uint8_t *flip = malloc((size_t)W * H * 4);
    if (!buf || !flip) { free(buf); free(flip); return; }
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, buf);
    for (int y = 0; y < H; y++) memcpy(flip + (size_t)y * W * 4, buf + (size_t)(H - 1 - y) * W * 4, (size_t)W * 4);
    for (size_t i = 0; i < (size_t)W * H; i++) flip[i * 4 + 3] = 255;
    free(buf);
    free(g_shot);
    g_shot = flip;
    g_shot_w = W;
    g_shot_h = H;
}

/* ---------------------------- frame ---------------------------- */

static void fullscreen(GLuint fbo, int w, int h) {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, w, h);
    glBindVertexArray(vao_empty);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

static void draw_scene(const SceneFrame *s) {
    glBindFramebuffer(GL_FRAMEBUFFER, T.samples ? T.fbo_ms : T.hdr.fbo);
    glViewport(0, 0, T.w, T.h);
    glDepthMask(GL_TRUE);
    /*the clear colour is the fog colour (sRGB): the target is linear*/
    float cl[3];
    for (int i = 0; i < 3; i++) {
        float c = s->clear_color[i];
        cl[i] = c * (c * (c * 0.305306011f + 0.682171111f) + 0.012522878f);
    }
    glClearColor(cl[0], cl[1], cl[2], 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glUseProgram(p_scene.id);
    glBindBuffer(GL_UNIFORM_BUFFER, ubo_scene);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(SceneUniforms), &s->u);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, ubo_scene);
    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D_ARRAY, tex_mat);
    glActiveTexture(GL_TEXTURE0 + 2);
    glBindTexture(GL_TEXTURE_2D, tex_expl);
    if (s->explored.data && s->explored.version != expl_ver) {
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, s->explored.w, s->explored.h, 0, GL_RED, GL_UNSIGNED_BYTE, s->explored.data);
        expl_ver = s->explored.version;
    }
    glActiveTexture(GL_TEXTURE0);

    glBindVertexArray(vao_scene);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDisable(GL_BLEND);
    float pc[16] = {0};
    push(&p_scene, pc);
    if (s->world_version != world_ver) {
        glBindBuffer(GL_ARRAY_BUFFER, vbo_world);
        size_t bytes = (size_t)s->world_count * sizeof(SVert);
        if (bytes > vbo_world_cap) vbo_world_cap = bytes + bytes / 4;
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)vbo_world_cap, NULL, GL_STATIC_DRAW);
        if (bytes) glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr)bytes, s->world);
        world_ver = s->world_version;
    }
    if (s->world_count) {
        glBindBuffer(GL_ARRAY_BUFFER, vbo_world);
        scene_attribs();
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)s->world_count);
    }
    draw_stream(s->dyn, s->dyn_count);

    glEnable(GL_BLEND);
    glDepthMask(GL_FALSE);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    pc[0] = 1;
    push(&p_scene, pc);
    draw_stream(s->trans, s->trans_count);
    glBlendFunc(GL_ONE, GL_ONE);
    pc[0] = 2;
    push(&p_scene, pc);
    draw_stream(s->add, s->add_count);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);

    if (T.samples) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, T.fbo_ms);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, T.hdr.fbo);
        glBlitFramebuffer(0, 0, T.w, T.h, 0, 0, T.w, T.h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
}

static void draw_bloom(void) {
    float pc[16] = {0};
    glActiveTexture(GL_TEXTURE0);
    glUseProgram(p_down.id);
    for (int i = 0; i < T.nbloom; i++) {
        const Target *src = i == 0 ? &T.hdr : &T.bloom[i - 1];
        glBindTexture(GL_TEXTURE_2D, src->tex);
        pc[0] = 1.0f / (float)src->w;
        pc[1] = 1.0f / (float)src->h;
        pc[2] = i == 0 ? 1.0f : 0.0f;
        pc[3] = 1.0f;  /*threshold*/
        pc[4] = 0.6f;  /*knee*/
        push(&p_down, pc);
        fullscreen(T.bloom[i].fbo, T.bloom[i].w, T.bloom[i].h);
    }
    glUseProgram(p_up.id);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);
    for (int i = T.nbloom - 2; i >= 0; i--) {
        const Target *src = &T.bloom[i + 1];
        glBindTexture(GL_TEXTURE_2D, src->tex);
        memset(pc, 0, sizeof(pc));
        pc[0] = 1.0f / (float)src->w;
        pc[1] = 1.0f / (float)src->h;
        pc[2] = 1.0f;
        pc[3] = 1.0f;
        push(&p_up, pc);
        fullscreen(T.bloom[i].fbo, T.bloom[i].w, T.bloom[i].h);
    }
    glDisable(GL_BLEND);
}

static void composite(const SceneFrame *s, GLuint fbo, int w, int h, bool bloom) {
    glUseProgram(p_comp.id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, T.hdr.tex);
    glActiveTexture(GL_TEXTURE0 + 1);
    glBindTexture(GL_TEXTURE_2D, bloom ? T.bloom[0].tex : T.hdr.tex);
    glActiveTexture(GL_TEXTURE0);
    float aspect = (float)w / (float)(h > 0 ? h : 1);
    const GfxPost *po = &s->post;
    float pc[16] = {
        s->screen_tint[0], s->screen_tint[1], s->screen_tint[2], s->screen_tint[3],
        s->vignette, aspect > 1 ? 1.0f : aspect, aspect > 1 ? 1.0f / aspect : 1.0f, po->exposure,
        po->bloom, po->saturation, po->contrast, bloom ? 1.0f : 0.0f,
        po->grade[0], po->grade[1], po->grade[2], 0,
    };
    push(&p_comp, pc);
    fullscreen(fbo, w, h);
}

static void gl_render(const SceneFrame *s, const UIFrame *ui, int w, int h) {
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    if (s->draw_scene && w > 0 && h > 0 && targets_ensure(w, h)) {
        draw_scene(s);
        bool bloom = g_rs.bloom && T.nbloom > 0 && s->post.bloom > 0;
        if (bloom) draw_bloom();
        if (T.ldr.fbo) {
            composite(s, T.ldr.fbo, w, h, bloom);
            glUseProgram(p_fxaa.id);
            glBindTexture(GL_TEXTURE_2D, T.ldr.tex);
            float pc[16] = {1.0f / (float)w, 1.0f / (float)h};
            push(&p_fxaa, pc);
            fullscreen(0, w, h);
        } else {
            composite(s, 0, w, h, bloom);
        }
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, w, h);
        glClearColor(s->clear_color[0], s->clear_color[1], s->clear_color[2], 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    /*UI*/
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, w, h);
    if (ui && ui->count) {
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(p_ui.id);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex_atlas);
        if (ui->atlas_version != atlas_ver) {
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, ui->atlas_w, ui->atlas_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, ui->atlas);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            atlas_ver = ui->atlas_version;
        }
        float pc[16] = {(float)w, (float)h};
        push(&p_ui, pc);
        glBindVertexArray(vao_ui);
        upload_stream(vbo_ui, &vbo_ui_cap, ui->v, (size_t)ui->count * sizeof(UIVert));
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)ui->count);
        glDisable(GL_BLEND);
    }
    glBindVertexArray(0);
    if (g_shot_armed) {
        g_shot_armed = false;
        capture_backbuffer(w, h);
    }
    glfwSwapBuffers(g_win);
}

static uint8_t *gl_screenshot(int *w, int *h) {
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

static void gl_set_vsync(bool on) { glfwSwapInterval(on ? 1 : 0); }
static void gl_shutdown(void) { targets_free(); }
static const char *gl_device_name(void) { return g_device; }
static void gl_configure(const RenderSettings *rs) {
    g_rs = *rs;
    if (g_rs.aa == GFX_AA_MSAA && g_max_samples < 2) g_rs.aa = GFX_AA_FXAA;
}

const GfxBackend gfx_gl_backend = {
    "OpenGL 3.3",
    gl_hints,
    gl_init,
    gl_upload_materials,
    gl_render,
    gl_screenshot,
    gl_set_vsync,
    gl_shutdown,
    gl_device_name,
    gl_configure,
    GFX_CAP_MSAA,
};
