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
    X(void, glBindAttribLocation, (GLuint, GLuint, const GLchar *))

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

/* ---------------------------- shaders ---------------------------- */

static const char *SCENE_VS =
    "#version 330 core\n"
    "layout(std140) uniform Scene {\n"
    "  mat4 viewproj; vec4 cam_pos; vec4 player_pos; vec4 ambient; vec4 fog;\n"
    "  vec4 cut_dir; vec4 misc; vec4 lights[64];\n"
    "};\n"
    "layout(location=0) in vec3 a_pos;\n"
    "layout(location=1) in vec3 a_nrm;\n"
    "layout(location=2) in vec2 a_uv;\n"
    "layout(location=3) in float a_layer;\n"
    "layout(location=4) in float a_glow;\n"
    "layout(location=5) in float a_flags;\n"
    "layout(location=6) in vec4 a_col;\n"
    "out vec3 v_wpos; out vec3 v_nrm; out vec2 v_uv; flat out float v_layer;\n"
    "out float v_glow; flat out int v_flags; out vec4 v_col;\n"
    "void main() {\n"
    "  vec3 p = a_pos; int fl = int(a_flags + 0.5); float t = cam_pos.w;\n"
    "  if ((fl & 1) != 0 && cut_dir.w > 0.5) {\n"
    "    vec2 d = p.xz - player_pos.xz; vec2 f = cut_dir.xy;\n"
    "    float along = dot(d, -f); float lat = abs(d.x * f.y - d.y * f.x);\n"
    "    float k = smoothstep(0.3, 1.3, along) * (1.0 - smoothstep(2.0 + along * 0.35, 3.5 + along * 0.45, lat)) * (1.0 - smoothstep(11.0, 14.0, along));\n"
    "    p.y = mix(p.y, min(p.y, 0.16), k);\n"
    "  }\n"
    "  if ((fl & 2) != 0) p.y += sin(t * 1.6 + p.x * 1.3 + p.z * 0.9) * 0.025;\n"
    "  if ((fl & 8) != 0) p.x += sin(t * 2.0 + a_pos.x * 2.0 + a_pos.z) * 0.18 * a_pos.y;\n"
    "  vec2 uv = a_uv;\n"
    "  if ((fl & 2) != 0) uv += vec2(t * 0.03, t * 0.02);\n"
    "  if ((fl & 4) != 0) uv += vec2(t * 0.02, -t * 0.015);\n"
    "  v_wpos = p; v_nrm = a_nrm; v_uv = uv; v_layer = a_layer; v_glow = a_glow; v_flags = fl; v_col = a_col;\n"
    "  gl_Position = viewproj * vec4(p, 1.0);\n"
    "}\n";

static const char *SCENE_FS =
    "#version 330 core\n"
    "layout(std140) uniform Scene {\n"
    "  mat4 viewproj; vec4 cam_pos; vec4 player_pos; vec4 ambient; vec4 fog;\n"
    "  vec4 cut_dir; vec4 misc; vec4 lights[64];\n"
    "};\n"
    "uniform sampler2DArray u_tex; uniform sampler2D u_explored; uniform int u_pass;\n"
    "in vec3 v_wpos; in vec3 v_nrm; in vec2 v_uv; flat in float v_layer;\n"
    "in float v_glow; flat in int v_flags; in vec4 v_col;\n"
    "out vec4 o_col;\n"
    "void main() {\n"
    "  vec4 tex = texture(u_tex, vec3(v_uv, v_layer));\n"
    "  vec4 base = tex * v_col;\n"
    "  float expl = texture(u_explored, (v_wpos.xz + 0.5) / misc.y).r;\n"
    "  float pd = length(v_wpos.xz - player_pos.xz);\n"
    "  float vis = 1.0 - smoothstep(player_pos.w - 1.8, player_pos.w + 0.4, pd);\n"
    "  if ((v_flags & 16) != 0) { expl = 1.0; vis = 1.0; }\n"
    "  if (u_pass == 2) {\n"
    "    if (expl < 0.5) discard;\n"
    "    float a = base.a * max(vis, 0.0);\n"
    "    o_col = vec4(base.rgb * a, 1.0); return;\n"
    "  }\n"
    "  if (expl < 0.5) discard;\n"
    "  if (u_pass == 0 && base.a < 0.35) discard;\n"
    "  vec3 n = normalize(v_nrm);\n"
    "  float hemi = 0.55 + 0.45 * n.y;\n"
    "  vec3 light = ambient.rgb * hemi;\n"
    "  int nl = int(misc.x);\n"
    "  for (int i = 0; i < nl; i++) {\n"
    "    vec4 lp = lights[i * 2]; vec4 lc = lights[i * 2 + 1];\n"
    "    vec3 L = lp.xyz - v_wpos; float d = length(L);\n"
    "    float att = clamp(1.0 - d / lp.w, 0.0, 1.0); att *= att;\n"
    "    float ndl = max(dot(n, L / max(d, 1e-3)), 0.0) * 0.8 + 0.2;\n"
    "    light += lc.rgb * lc.a * att * ndl;\n"
    "  }\n"
    "  vec3 lit = base.rgb * light + base.rgb * v_glow * 1.4;\n"
    "  if ((v_flags & 2) != 0) {\n"
    "    vec3 vd = normalize(cam_pos.xyz - v_wpos);\n"
    "    float fres = pow(1.0 - max(vd.y, 0.0), 3.0);\n"
    "    lit += vec3(0.10, 0.16, 0.24) * (0.4 + fres) * (0.4 + light);\n"
    "  }\n"
    "  float lum = dot(base.rgb, vec3(0.3, 0.59, 0.11));\n"
    "  vec3 mem = vec3(lum) * vec3(0.62, 0.70, 0.95) * ambient.a;\n"
    "  vec3 col = mix(mem, lit, vis);\n"
    "  float cd = length(v_wpos - cam_pos.xyz);\n"
    "  col = mix(col, fog.rgb, 1.0 - exp(-fog.a * max(cd - 14.0, 0.0)));\n"
    "  col = col * misc.w; col = col / (1.0 + max(col - 1.0, 0.0) * 0.6);\n"
    "  o_col = vec4(col, u_pass == 1 ? base.a : 1.0);\n"
    "}\n";

static const char *UI_VS =
    "#version 330 core\n"
    "uniform vec2 u_screen;\n"
    "layout(location=0) in vec2 a_pos; layout(location=1) in vec2 a_uv; layout(location=2) in vec4 a_col;\n"
    "out vec2 v_uv; out vec4 v_col;\n"
    "void main() { v_uv = a_uv; v_col = a_col;\n"
    "  gl_Position = vec4(a_pos.x / u_screen.x * 2.0 - 1.0, 1.0 - a_pos.y / u_screen.y * 2.0, 0.0, 1.0); }\n";

static const char *UI_FS =
    "#version 330 core\n"
    "uniform sampler2D u_atlas; in vec2 v_uv; in vec4 v_col; out vec4 o_col;\n"
    "void main() { o_col = texture(u_atlas, v_uv) * v_col; }\n";

static const char *POST_VS =
    "#version 330 core\n"
    "out vec2 v_p;\n"
    "void main() { vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2); v_p = p * 2.0 - 1.0;\n"
    "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }\n";

static const char *POST_FS =
    "#version 330 core\n"
    "uniform vec4 u_tint; uniform float u_vignette; uniform vec2 u_aspect;\n"
    "in vec2 v_p; out vec4 o_col;\n"
    "void main() { vec2 q = v_p * u_aspect; float r = dot(q, q);\n"
    "  float vig = smoothstep(0.55, 1.6, r) * u_vignette;\n"
    "  vec3 c = mix(vec3(0.0), u_tint.rgb, u_tint.a / max(u_tint.a + vig, 1e-4));\n"
    "  o_col = vec4(c, clamp(vig + u_tint.a, 0.0, 1.0)); }\n";

/* ---------------------------- state ---------------------------- */

static GLFWwindow *g_win;
static GLuint p_scene, p_ui, p_post;
static GLint u_scene_pass, u_scene_tex, u_scene_expl;
static GLint u_ui_screen, u_ui_atlas;
static GLint u_post_tint, u_post_vig, u_post_aspect;
static GLuint vao_scene, vao_ui, vao_post;
static GLuint vbo_world, vbo_dyn, vbo_ui, ubo_scene;
static GLuint tex_mat, tex_atlas, tex_expl;
static uint32_t world_ver = 0xFFFFFFFFu, atlas_ver = 0xFFFFFFFFu, expl_ver = 0xFFFFFFFFu;
static size_t vbo_world_cap, vbo_dyn_cap, vbo_ui_cap;
static char g_device[256];
static int g_last_w, g_last_h;

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "[GL] shader compile error:\n%s\n", log);
        return 0;
    }
    return s;
}

static GLuint link(const char *vs, const char *fs) {
    GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(p, sizeof(log), NULL, log);
        fprintf(stderr, "[GL] program link error:\n%s\n", log);
        return 0;
    }
    return p;
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

static void gl_hints(void) {
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_API);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
    glfwWindowHint(GLFW_SAMPLES, 4);
    glfwWindowHint(GLFW_DEPTH_BITS, 24);
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

    p_scene = link(SCENE_VS, SCENE_FS);
    p_ui = link(UI_VS, UI_FS);
    p_post = link(POST_VS, POST_FS);
    if (!p_scene || !p_ui || !p_post) return false;
    u_scene_pass = glGetUniformLocation(p_scene, "u_pass");
    u_scene_tex = glGetUniformLocation(p_scene, "u_tex");
    u_scene_expl = glGetUniformLocation(p_scene, "u_explored");
    GLuint bi = glGetUniformBlockIndex(p_scene, "Scene");
    if (bi != GL_INVALID_INDEX) glUniformBlockBinding(p_scene, bi, 0);
    u_ui_screen = glGetUniformLocation(p_ui, "u_screen");
    u_ui_atlas = glGetUniformLocation(p_ui, "u_atlas");
    u_post_tint = glGetUniformLocation(p_post, "u_tint");
    u_post_vig = glGetUniformLocation(p_post, "u_vignette");
    u_post_aspect = glGetUniformLocation(p_post, "u_aspect");

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
    glGenVertexArrays(1, &vao_post);
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
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, size, size, layers, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
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
    if (bytes > *cap) {
        *cap = bytes + bytes / 2;
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)*cap, NULL, GL_STREAM_DRAW);
    } else {
        /*orphan: let the driver hand us fresh storage*/
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)*cap, NULL, GL_STREAM_DRAW);
    }
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
    free(buf);
    free(g_shot);
    g_shot = flip;
    g_shot_w = W;
    g_shot_h = H;
}

static void gl_render(const SceneFrame *s, const UIFrame *ui, int w, int h) {
    g_last_w = w;
    g_last_h = h;
    glViewport(0, 0, w, h);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_TRUE);
    glClearColor(s->clear_color[0], s->clear_color[1], s->clear_color[2], 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (s->draw_scene) {
        glUseProgram(p_scene);
        glBindBuffer(GL_UNIFORM_BUFFER, ubo_scene);
        glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(SceneUniforms), &s->u);
        glBindBufferBase(GL_UNIFORM_BUFFER, 0, ubo_scene);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D_ARRAY, tex_mat);
        glUniform1i(u_scene_tex, 0);
        glActiveTexture(GL_TEXTURE0 + 1);
        glBindTexture(GL_TEXTURE_2D, tex_expl);
        if (s->explored.data && s->explored.version != expl_ver) {
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, s->explored.w, s->explored.h, 0, GL_RED, GL_UNSIGNED_BYTE, s->explored.data);
            expl_ver = s->explored.version;
        }
        glUniform1i(u_scene_expl, 1);
        glActiveTexture(GL_TEXTURE0);

        glBindVertexArray(vao_scene);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glDisable(GL_BLEND);
        glUniform1i(u_scene_pass, 0);
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
        glUniform1i(u_scene_pass, 1);
        draw_stream(s->trans, s->trans_count);
        glBlendFunc(GL_ONE, GL_ONE);
        glUniform1i(u_scene_pass, 2);
        draw_stream(s->add, s->add_count);
        glDepthMask(GL_TRUE);

        /*post overlay: vignette + tint*/
        glDisable(GL_DEPTH_TEST);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(p_post);
        glUniform4f(u_post_tint, s->screen_tint[0], s->screen_tint[1], s->screen_tint[2], s->screen_tint[3]);
        glUniform1f(u_post_vig, s->vignette);
        float aspect = (float)w / (float)(h > 0 ? h : 1);
        glUniform2f(u_post_aspect, aspect > 1 ? 1.0f : aspect, aspect > 1 ? 1.0f / aspect : 1.0f);
        glBindVertexArray(vao_post);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    /*UI*/
    if (ui && ui->count) {
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(p_ui);
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
        glUniform1i(u_ui_atlas, 0);
        glUniform2f(u_ui_screen, (float)w, (float)h);
        glBindVertexArray(vao_ui);
        upload_stream(vbo_ui, &vbo_ui_cap, ui->v, (size_t)ui->count * sizeof(UIVert));
        glDrawArrays(GL_TRIANGLES, 0, (GLsizei)ui->count);
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
static void gl_shutdown(void) {}
static const char *gl_device_name(void) { return g_device; }

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
};
