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

#include "shaders.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*X(file, symbol, has SPIR-V): keep in sync with SHADER_STAGES and
 * SHADER_INCLUDES in CMakeLists.txt*/
#define SHADER_LIST(X)                       \
    X("common.glsl", common_glsl, 0)         \
    X("scene_ubo.glsl", scene_ubo_glsl, 0)   \
    X("scene.vert", scene_vert, 1)           \
    X("scene.frag", scene_frag, 1)           \
    X("ui.vert", ui_vert, 1)                 \
    X("ui.frag", ui_frag, 1)                 \
    X("fullscreen.vert", fullscreen_vert, 1) \
    X("bloom_down.frag", bloom_down_frag, 1) \
    X("bloom_up.frag", bloom_up_frag, 1)     \
    X("composite.frag", composite_frag, 1)   \
    X("fxaa.frag", fxaa_frag, 1)

#define DECL_GLSL(file, sym, spv)                   \
    extern const unsigned char asset_glsl_##sym[]; \
    extern const size_t asset_glsl_##sym##_size;
SHADER_LIST(DECL_GLSL)
#undef DECL_GLSL
/*only the stages have SPIR-V: declared one by one*/
#define DECL_SPV(sym)                              \
    extern const unsigned char asset_spv_##sym[]; \
    extern const size_t asset_spv_##sym##_size;
DECL_SPV(scene_vert)
DECL_SPV(scene_frag)
DECL_SPV(ui_vert)
DECL_SPV(ui_frag)
DECL_SPV(fullscreen_vert)
DECL_SPV(bloom_down_frag)
DECL_SPV(bloom_up_frag)
DECL_SPV(composite_frag)
DECL_SPV(fxaa_frag)
#undef DECL_SPV

#define SPV_0(sym) NULL, 0
#define SPV_1(sym) asset_spv_##sym, 0
#define ENTRY(file, sym, spv) {file, asset_glsl_##sym, 0, SPV_##spv(sym)},
static ShaderFile g_files[] = {SHADER_LIST(ENTRY)};
#undef ENTRY

/*the sizes are not constant expressions: filled on first use*/
static void fill_sizes(void) {
    static int done;
    if (done) return;
    done = 1;
    size_t i = 0;
#define SIZE_0(sym)
#define SIZE_1(sym) g_files[i].spv_size = asset_spv_##sym##_size;
#define FILL(file, sym, spv)                        \
    g_files[i].glsl_size = asset_glsl_##sym##_size; \
    SIZE_##spv(sym) i++;
    SHADER_LIST(FILL)
#undef FILL
}

const ShaderFile *shader_file(const char *name) {
    fill_sizes();
    for (size_t i = 0; i < sizeof(g_files) / sizeof(g_files[0]); i++)
        if (!strcmp(g_files[i].name, name)) return &g_files[i];
    return NULL;
}

typedef struct {
    char *s;
    size_t n, cap;
} Str;

static int put(Str *b, const char *p, size_t n) {
    if (b->n + n + 1 > b->cap) {
        size_t cap = (b->n + n + 1) * 2;
        char *ns = realloc(b->s, cap);
        if (!ns) return 0;
        b->s = ns;
        b->cap = cap;
    }
    memcpy(b->s + b->n, p, n);
    b->n += n;
    b->s[b->n] = 0;
    return 1;
}

static int expand(Str *b, const char *name, int depth) {
    const ShaderFile *f = shader_file(name);
    if (!f || depth > 4) {
        fprintf(stderr, "[shaders] missing include %s\n", name);
        return 0;
    }
    const char *p = (const char *)f->glsl, *end = p + f->glsl_size;
    while (p < end) {
        const char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t len = nl ? (size_t)(nl - p) + 1 : (size_t)(end - p);
        const char *q = p;
        while (q < p + len && (*q == ' ' || *q == '\t')) q++;
        if (!strncmp(q, "#version", 8) || !strncmp(q, "#extension", 10)) {
            if (!put(b, "\n", 1)) return 0; /*keep the line numbers of error messages*/
        } else if (!strncmp(q, "#include", 8)) {
            const char *a = memchr(q, '"', len - (size_t)(q - p));
            const char *z = a ? memchr(a + 1, '"', (size_t)(p + len - a - 1)) : NULL;
            char inc[64];
            if (!a || !z || (size_t)(z - a - 1) >= sizeof(inc)) return 0;
            memcpy(inc, a + 1, (size_t)(z - a - 1));
            inc[z - a - 1] = 0;
            if (!expand(b, inc, depth + 1) || !put(b, "\n", 1)) return 0;
        } else if (!put(b, p, len)) {
            return 0;
        }
        p += len;
    }
    return 1;
}

char *shader_gl_source(const char *name) {
    Str b = {0};
    if (!put(&b, "#version 330 core\n", 18) || !expand(&b, name, 0)) {
        free(b.s);
        return NULL;
    }
    return b.s;
}
