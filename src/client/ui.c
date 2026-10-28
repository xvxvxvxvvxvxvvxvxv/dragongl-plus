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

/*Immediate-mode UI toolkit — see ui.h.
 *
 * Geometry model: every primitive is a CONVEX polygon (with per-vertex
 * colour and UV) that is clipped against the current clip rectangle
 * (Sutherland-Hodgman) and fan-triangulated into the active layer.
 * That single path handles rectangles, gradients, rounded corners,
 * circles, glyphs and icons, and makes scroll areas clip correctly.*/

#include "ui.h"
#include "assets.h"
#include "mathx.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

void ui_icons_rasterize(uint8_t *dst, int stride, int ox, int oy, int cell, int per_row);

/* ------------------------------------------------------------------ */
/* Atlas & fonts                                                       */
/* ------------------------------------------------------------------ */

#define ATLAS_W 2048
#define ATLAS_H 2048
#define ICON_CELL 64
#define ICON_PER_ROW 32
#define ICON_AREA_H (ICON_CELL * 2)           /*bottom strip*/
#define FONT_AREA_H (ATLAS_H - ICON_AREA_H)
#define WHITE_X (ATLAS_W - 4)
#define WHITE_Y (ATLAS_H - 4)

typedef struct {
    float logical;            /*nominal logical size*/
    float px;                 /*baked pixel size*/
    int first, count;
    stbtt_packedchar *chars;
    float ascent, descent, line_gap; /*in baked pixels*/
} BakedSize;

#define MAX_SIZES 8
typedef struct {
    stbtt_fontinfo info;
    const unsigned char *data;
    BakedSize sizes[MAX_SIZES];
    int nsizes;
} UIFont;

static const float FONT_SIZES[FONT_COUNT][MAX_SIZES] = {
    [FONT_BODY]  = {13, 15, 17, 20, 24, 0},
    [FONT_BOLD]  = {13, 15, 17, 20, 24, 30, 0},
    [FONT_TITLE] = {16, 20, 26, 34, 46, 64, 96, 0},
};

static UIFont g_fonts[FONT_COUNT];
static uint8_t *g_atlas;          /*RGBA*/
static uint32_t g_atlas_version;
static float g_baked_scale = -1.0f;

static bool bake_fonts(float scale) {
    uint8_t *alpha = calloc((size_t)ATLAS_W * FONT_AREA_H, 1);
    if (!alpha) return false;
    /*Try the requested scale; if the glyphs do not fit, shrink the bake
     * (quads are then magnified slightly — only at very large UI scales).*/
    float bake = scale;
    for (int attempt = 0; attempt < 6; attempt++) {
        memset(alpha, 0, (size_t)ATLAS_W * FONT_AREA_H);
        stbtt_pack_context pc;
        if (!stbtt_PackBegin(&pc, alpha, ATLAS_W, FONT_AREA_H, 0, 1, NULL)) {
            free(alpha);
            return false;
        }
        bool ok = true;
        for (int f = 0; f < FONT_COUNT && ok; f++) {
            UIFont *F = &g_fonts[f];
            for (int s = 0; s < F->nsizes && ok; s++) {
                BakedSize *B = &F->sizes[s];
                B->px = B->logical * bake;
                if (B->px < 6) B->px = 6;
                int over = B->px < 28.0f ? 2 : 1;
                stbtt_PackSetOversampling(&pc, (unsigned)over, 1);
                free(B->chars);
                B->first = 32;
                B->count = (B->logical >= 40) ? 95 : 224; /*huge titles: ASCII only*/
                B->chars = calloc((size_t)B->count, sizeof(stbtt_packedchar));
                if (!stbtt_PackFontRange(&pc, F->data, 0, B->px, B->first, B->count, B->chars))
                    ok = false;
                float fs = stbtt_ScaleForPixelHeight(&F->info, B->px);
                int a, d, g;
                stbtt_GetFontVMetrics(&F->info, &a, &d, &g);
                B->ascent = a * fs;
                B->descent = d * fs;
                B->line_gap = g * fs;
            }
        }
        stbtt_PackEnd(&pc);
        if (ok) break;
        bake *= 0.8f;
    }
    /*Compose RGBA: white with coverage in alpha.*/
    for (int y = 0; y < FONT_AREA_H; y++)
        for (int x = 0; x < ATLAS_W; x++) {
            uint8_t *p = g_atlas + ((size_t)y * ATLAS_W + x) * 4;
            p[0] = p[1] = p[2] = 255;
            p[3] = alpha[(size_t)y * ATLAS_W + x];
        }
    free(alpha);
    g_baked_scale = scale;
    g_atlas_version++;
    return true;
}

static bool load_font(int f, const unsigned char *data) {
    g_fonts[f].data = data;
    if (!stbtt_InitFont(&g_fonts[f].info, data, stbtt_GetFontOffsetForIndex(data, 0)))
        return false;
    int n = 0;
    while (n < MAX_SIZES && FONT_SIZES[f][n] > 0) {
        g_fonts[f].sizes[n].logical = FONT_SIZES[f][n];
        n++;
    }
    g_fonts[f].nsizes = n;
    return true;
}

/* ------------------------------------------------------------------ */
/* Frame state                                                         */
/* ------------------------------------------------------------------ */

typedef struct { UIVert *v; uint32_t n, cap; } VList;
static VList g_layers[2];
static int g_layer;
static VList g_out;

typedef struct { float x0, y0, x1, y1; } Rect;
static Rect g_clip_stack[16];
static int g_clip_depth;

static float g_scale = 1.0f, g_w = 1920, g_h = 1080;
static int g_fbw, g_fbh;
static UIInput g_in;
static float g_dt;
static double g_time;

/*Blocking rects (mouse capture)*/
static bool g_mouse_captured, g_mouse_captured_next;

/*Focus*/
static uint32_t g_focus_id, g_focus_claim;
static int g_caret;
static bool g_focus_seen;

/*Slider drag*/
static uint32_t g_active_id;

/*Tooltip*/
static char g_tip_title[128], g_tip_body[1024];
static uint32_t g_tip_col;
static bool g_tip;

static uint32_t hash_str(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h ? h : 1;
}

static void vlist_push(VList *l, const UIVert *v, uint32_t n) {
    if (n == 0 || !v) return;
    if (l->n + n > l->cap) {
        uint32_t nc = l->cap ? l->cap * 2 : 65536;
        while (nc < l->n + n) nc *= 2;
        UIVert *nv = realloc(l->v, nc * sizeof(UIVert));
        if (!nv) return;
        l->v = nv;
        l->cap = nc;
    }
    memcpy(l->v + l->n, v, n * sizeof(UIVert));
    l->n += n;
}

uint32_t ui_lerp_color(uint32_t a, uint32_t b, float t) {
    uint32_t r = 0;
    for (int i = 0; i < 4; i++) {
        float ca = (float)((a >> (i * 8)) & 255), cb = (float)((b >> (i * 8)) & 255);
        uint32_t c = (uint32_t)(ca + (cb - ca) * t + 0.5f);
        r |= (c > 255 ? 255 : c) << (i * 8);
    }
    return r;
}

/* ------------------------------------------------------------------ */
/* Polygon pipeline                                                    */
/* ------------------------------------------------------------------ */

typedef struct { float x, y, u, v, c[4]; } PV;
#define MAXPV 96

static void pv_set(PV *p, float x, float y, float u, float v, uint32_t col) {
    p->x = x * g_scale;
    p->y = y * g_scale;
    p->u = u;
    p->v = v;
    for (int i = 0; i < 4; i++) p->c[i] = (float)((col >> (i * 8)) & 255);
}

static PV pv_lerp(const PV *a, const PV *b, float t) {
    PV r;
    r.x = a->x + (b->x - a->x) * t;
    r.y = a->y + (b->y - a->y) * t;
    r.u = a->u + (b->u - a->u) * t;
    r.v = a->v + (b->v - a->v) * t;
    for (int i = 0; i < 4; i++) r.c[i] = a->c[i] + (b->c[i] - a->c[i]) * t;
    return r;
}

/*Clip polygon against one axis-aligned edge.*/
static int clip_edge(const PV *in, int n, PV *out, int axis, float lim, bool keep_greater) {
    int m = 0;
    for (int i = 0; i < n; i++) {
        const PV *a = &in[i], *b = &in[(i + 1) % n];
        float va = axis ? a->y : a->x, vb = axis ? b->y : b->x;
        bool ia = keep_greater ? va >= lim : va <= lim;
        bool ib = keep_greater ? vb >= lim : vb <= lim;
        if (ia && m < MAXPV) out[m++] = *a;
        if (ia != ib && m < MAXPV) {
            float t = (lim - va) / (vb - va);
            out[m++] = pv_lerp(a, b, t);
        }
    }
    return m;
}

static void emit_poly(PV *p, int n) {
    if (n < 3) return;
    PV a[MAXPV], b[MAXPV];
    const Rect *c = &g_clip_stack[g_clip_depth];
    /*Fast path: fully inside / fully outside*/
    float minx = p[0].x, maxx = p[0].x, miny = p[0].y, maxy = p[0].y;
    for (int i = 1; i < n; i++) {
        if (p[i].x < minx) minx = p[i].x;
        if (p[i].x > maxx) maxx = p[i].x;
        if (p[i].y < miny) miny = p[i].y;
        if (p[i].y > maxy) maxy = p[i].y;
    }
    if (maxx < c->x0 || minx > c->x1 || maxy < c->y0 || miny > c->y1) return;
    PV *poly = p;
    if (minx < c->x0 || maxx > c->x1 || miny < c->y0 || maxy > c->y1) {
        n = clip_edge(p, n, a, 0, c->x0, true);
        n = clip_edge(a, n, b, 0, c->x1, false);
        n = clip_edge(b, n, a, 1, c->y0, true);
        n = clip_edge(a, n, b, 1, c->y1, false);
        poly = b;
        if (n < 3) return;
    }
    UIVert tri[3 * MAXPV];
    int k = 0;
    for (int i = 1; i + 1 < n; i++) {
        const PV *q[3] = {&poly[0], &poly[i], &poly[i + 1]};
        for (int j = 0; j < 3; j++) {
            UIVert *v = &tri[k++];
            v->pos[0] = q[j]->x;
            v->pos[1] = q[j]->y;
            v->uv[0] = q[j]->u;
            v->uv[1] = q[j]->v;
            for (int ci = 0; ci < 4; ci++) {
                float cc = q[j]->c[ci];
                v->col[ci] = (uint8_t)(cc < 0 ? 0 : (cc > 255 ? 255 : cc + 0.5f));
            }
        }
    }
    vlist_push(&g_layers[g_layer], tri, (uint32_t)k);
}

static float WU, WV; /*UV of the white texel*/

static void quad_uv(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
                    uint32_t c00, uint32_t c10, uint32_t c11, uint32_t c01) {
    PV p[4];
    pv_set(&p[0], x0, y0, u0, v0, c00);
    pv_set(&p[1], x1, y0, u1, v0, c10);
    pv_set(&p[2], x1, y1, u1, v1, c11);
    pv_set(&p[3], x0, y1, u0, v1, c01);
    emit_poly(p, 4);
}

void ui_rect(float x, float y, float w, float h, uint32_t col) {
    if ((col >> 24) == 0) return;
    quad_uv(x, y, x + w, y + h, WU, WV, WU, WV, col, col, col, col);
}
void ui_rect_grad(float x, float y, float w, float h, uint32_t top, uint32_t bottom) {
    quad_uv(x, y, x + w, y + h, WU, WV, WU, WV, top, top, bottom, bottom);
}
void ui_rect_hgrad(float x, float y, float w, float h, uint32_t left, uint32_t right) {
    quad_uv(x, y, x + w, y + h, WU, WV, WU, WV, left, right, right, left);
}

static int rrect_points(float x, float y, float w, float h, float r, float *px, float *py) {
    if (r > w * 0.5f) r = w * 0.5f;
    if (r > h * 0.5f) r = h * 0.5f;
    if (r < 0.5f) {
        px[0] = x; py[0] = y; px[1] = x + w; py[1] = y;
        px[2] = x + w; py[2] = y + h; px[3] = x; py[3] = y + h;
        return 4;
    }
    int seg = r * g_scale > 8 ? 6 : 3;
    int n = 0;
    float cx[4] = {x + w - r, x + w - r, x + r, x + r};
    float cy[4] = {y + r, y + h - r, y + h - r, y + r};
    static const float start[4] = {-1.5707963f, 0.0f, 1.5707963f, 3.1415926f};
    for (int c = 0; c < 4; c++)
        for (int i = 0; i <= seg; i++) {
            float a = start[c] + 1.5707963f * (float)i / (float)seg;
            px[n] = cx[c] + cosf(a) * r;
            py[n] = cy[c] + sinf(a) * r;
            n++;
        }
    return n;
}

void ui_rrect_grad(float x, float y, float w, float h, float r, uint32_t top, uint32_t bottom) {
    float px[40], py[40];
    int n = rrect_points(x, y, w, h, r, px, py);
    PV p[40];
    for (int i = 0; i < n; i++) {
        float t = h > 0 ? (py[i] - y) / h : 0;
        pv_set(&p[i], px[i], py[i], WU, WV, ui_lerp_color(top, bottom, t));
    }
    emit_poly(p, n);
}
void ui_rrect(float x, float y, float w, float h, float r, uint32_t col) {
    if ((col >> 24) == 0) return;
    ui_rrect_grad(x, y, w, h, r, col, col);
}

void ui_line(float x0, float y0, float x1, float y1, float t, uint32_t col) {
    float dx = x1 - x0, dy = y1 - y0;
    float l = sqrtf(dx * dx + dy * dy);
    if (l < 1e-4f) return;
    float nx = -dy / l * t * 0.5f, ny = dx / l * t * 0.5f;
    PV p[4];
    pv_set(&p[0], x0 + nx, y0 + ny, WU, WV, col);
    pv_set(&p[1], x1 + nx, y1 + ny, WU, WV, col);
    pv_set(&p[2], x1 - nx, y1 - ny, WU, WV, col);
    pv_set(&p[3], x0 - nx, y0 - ny, WU, WV, col);
    emit_poly(p, 4);
}

void ui_rrect_line(float x, float y, float w, float h, float r, float t, uint32_t col) {
    float px[40], py[40];
    int n = rrect_points(x, y, w, h, r, px, py);
    /*Outline: quads between consecutive points (inner offset toward centre)*/
    float cx = x + w * 0.5f, cy = y + h * 0.5f;
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        float ix0 = px[i], iy0 = py[i], ix1 = px[j], iy1 = py[j];
        float d0 = sqrtf((cx - ix0) * (cx - ix0) + (cy - iy0) * (cy - iy0));
        float d1 = sqrtf((cx - ix1) * (cx - ix1) + (cy - iy1) * (cy - iy1));
        /*inset along the edge normal*/
        float ex = ix1 - ix0, ey = iy1 - iy0;
        float el = sqrtf(ex * ex + ey * ey);
        if (el < 1e-5f) continue;
        float nx = ey / el, ny = -ex / el; /*points outward for clockwise*/
        (void)d0; (void)d1;
        PV p[4];
        pv_set(&p[0], ix0, iy0, WU, WV, col);
        pv_set(&p[1], ix1, iy1, WU, WV, col);
        pv_set(&p[2], ix1 - nx * t, iy1 - ny * t, WU, WV, col);
        pv_set(&p[3], ix0 - nx * t, iy0 - ny * t, WU, WV, col);
        emit_poly(p, 4);
    }
}

void ui_circle(float cx, float cy, float r, uint32_t col) {
    int seg = (int)fminf(48, fmaxf(10, r * g_scale * 0.6f));
    PV p[MAXPV];
    if (seg > MAXPV - 1) seg = MAXPV - 1;
    for (int i = 0; i < seg; i++) {
        float a = MX_TAU * (float)i / (float)seg;
        pv_set(&p[i], cx + cosf(a) * r, cy + sinf(a) * r, WU, WV, col);
    }
    emit_poly(p, seg);
}

void ui_arc(float cx, float cy, float r, float t, float a0, float a1, uint32_t col) {
    int seg = (int)fmaxf(4, fabsf(a1 - a0) * r * g_scale * 0.15f);
    if (seg > 128) seg = 128;
    for (int i = 0; i < seg; i++) {
        float t0 = a0 + (a1 - a0) * (float)i / (float)seg;
        float t1 = a0 + (a1 - a0) * (float)(i + 1) / (float)seg;
        PV p[4];
        pv_set(&p[0], cx + cosf(t0) * r, cy + sinf(t0) * r, WU, WV, col);
        pv_set(&p[1], cx + cosf(t1) * r, cy + sinf(t1) * r, WU, WV, col);
        pv_set(&p[2], cx + cosf(t1) * (r - t), cy + sinf(t1) * (r - t), WU, WV, col);
        pv_set(&p[3], cx + cosf(t0) * (r - t), cy + sinf(t0) * (r - t), WU, WV, col);
        emit_poly(p, 4);
    }
}

void ui_ring(float cx, float cy, float r, float t, uint32_t col) {
    ui_arc(cx, cy, r, t, 0, MX_TAU, col);
}

void ui_tri(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t col) {
    PV p[3];
    pv_set(&p[0], x0, y0, WU, WV, col);
    pv_set(&p[1], x1, y1, WU, WV, col);
    pv_set(&p[2], x2, y2, WU, WV, col);
    emit_poly(p, 3);
}

void ui_diamond(float cx, float cy, float r, uint32_t col) {
    PV p[4];
    pv_set(&p[0], cx, cy - r, WU, WV, col);
    pv_set(&p[1], cx + r, cy, WU, WV, col);
    pv_set(&p[2], cx, cy + r, WU, WV, col);
    pv_set(&p[3], cx - r, cy, WU, WV, col);
    emit_poly(p, 4);
}

void ui_shadow(float x, float y, float w, float h, float r, float spread, float alpha) {
    for (int i = 6; i >= 1; i--) {
        float s = spread * (float)i / 6.0f;
        ui_rrect(x - s, y - s + spread * 0.3f, w + 2 * s, h + 2 * s, r + s,
                 UIC(0, 0, 0, (int)(alpha * 255.0f / 7.0f)));
    }
}

void ui_icon(int icon, float x, float y, float size, uint32_t tint) {
    if (icon <= 0 || icon >= ICON_COUNT) return;
    float u0 = (float)((icon % ICON_PER_ROW) * ICON_CELL) / ATLAS_W;
    float v0 = (float)(FONT_AREA_H + (icon / ICON_PER_ROW) * ICON_CELL) / ATLAS_H;
    float u1 = u0 + (float)ICON_CELL / ATLAS_W, v1 = v0 + (float)ICON_CELL / ATLAS_H;
    /*half-texel inset against bleeding*/
    float eps = 0.5f / ATLAS_W;
    quad_uv(x, y, x + size, y + size, u0 + eps, v0 + eps, u1 - eps, v1 - eps, tint, tint, tint, tint);
}

/* ------------------------------------------------------------------ */
/* Text                                                                */
/* ------------------------------------------------------------------ */

static BakedSize *pick_size(int font, float size) {
    UIFont *F = &g_fonts[font < 0 || font >= FONT_COUNT ? 0 : font];
    BakedSize *best = &F->sizes[0];
    float bd = 1e9f;
    for (int i = 0; i < F->nsizes; i++) {
        /*prefer the closest size at or above the request*/
        float d = F->sizes[i].logical - size;
        float cost = d >= 0 ? d : -d * 2.0f;
        if (cost < bd) { bd = cost; best = &F->sizes[i]; }
    }
    return best;
}

/*Decode one UTF-8 codepoint (Latin-1 range used by the font).*/
static uint32_t utf8_next(const char **ps) {
    const unsigned char *s = (const unsigned char *)*ps;
    uint32_t c = s[0];
    if (c < 0x80) { *ps += 1; return c; }
    if ((c & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
        *ps += 2;
        return ((c & 0x1F) << 6) | (s[1] & 0x3F);
    }
    if ((c & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        *ps += 3;
        return ((c & 0x0F) << 12) | ((uint32_t)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
    }
    if ((c & 0xF8) == 0xF0) { *ps += 4; return '?'; }
    *ps += 1;
    return c; /*stray Latin-1 byte*/
}

int ui_utf8_encode(uint32_t cp, char out[4]) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
}

static float glyph_advance(BakedSize *B, uint32_t cp, float k) {
    if (cp < (uint32_t)B->first || cp >= (uint32_t)(B->first + B->count)) cp = '?';
    return B->chars[cp - B->first].xadvance * k;
}

/*k: logical units per baked pixel*/
static float size_factor(BakedSize *B, float size) {
    return size / B->px;
}

float ui_text_width_n(int font, float size, const char *s, int n) {
    if (!s) return 0;
    BakedSize *B = pick_size(font, size);
    float k = size_factor(B, size);
    float w = 0;
    const char *end = n >= 0 ? s + n : NULL;
    while (*s && (!end || s < end)) {
        if (s[0] == '^' && s[1] >= '0' && s[1] <= '9') { s += 2; continue; }
        w += glyph_advance(B, utf8_next(&s), k);
    }
    return w;
}

float ui_text_width(int font, float size, const char *s) { return ui_text_width_n(font, size, s, -1); }

static uint32_t rich_color(char code, uint32_t base) {
    switch (code) {
    case '1': return COL_RED;
    case '2': return COL_GREEN;
    case '3': return UIC(240, 220, 90, 255);
    case '4': return COL_BLUE;
    case '5': return COL_PURPLE;
    case '6': return UIC(110, 220, 240, 255);
    case '7': return COL_WHITE;
    case '8': return COL_GOLD;
    case '9': return COL_TEXT_FAINT;
    default: return base;
    }
}

static float draw_text_impl(int font, float size, float x, float y, const char *s, uint32_t col,
                            int align, bool rich) {
    if (!s || !*s) return 0;
    BakedSize *B = pick_size(font, size);
    float k = size_factor(B, size);
    float w = ui_text_width(font, size, s);
    if ((align & 3) == ALIGN_CENTER) x -= w * 0.5f;
    else if ((align & 3) == ALIGN_RIGHT) x -= w;
    float asc = B->ascent * k, desc = B->descent * k;
    if (align & ALIGN_MIDDLE) y -= (asc - desc) * 0.5f;
    float baseline = y + asc;
    /*Snap the pen to whole pixels: crisper small text.*/
    float pen = floorf(x * g_scale + 0.5f) / g_scale;
    baseline = floorf(baseline * g_scale + 0.5f) / g_scale;
    uint32_t cur = col;
    uint32_t alpha_mul = col >> 24;
    while (*s) {
        if (rich && s[0] == '^' && s[1] >= '0' && s[1] <= '9') {
            cur = (rich_color(s[1], col) & 0x00FFFFFFu) | (alpha_mul << 24);
            s += 2;
            continue;
        }
        uint32_t cp = utf8_next(&s);
        if (cp < (uint32_t)B->first || cp >= (uint32_t)(B->first + B->count)) cp = '?';
        stbtt_packedchar *pc = &B->chars[cp - B->first];
        if (cp != ' ') {
            float x0 = pen + pc->xoff * k, y0 = baseline + pc->yoff * k;
            float x1 = pen + pc->xoff2 * k, y1 = baseline + pc->yoff2 * k;
            quad_uv(x0, y0, x1, y1, pc->x0 / (float)ATLAS_W, pc->y0 / (float)ATLAS_H,
                    pc->x1 / (float)ATLAS_W, pc->y1 / (float)ATLAS_H, cur, cur, cur, cur);
        }
        pen += pc->xadvance * k;
    }
    return w;
}

float ui_text(int font, float size, float x, float y, const char *s, uint32_t col, int align) {
    return draw_text_impl(font, size, x, y, s, col, align, false);
}

float ui_text_rich(int font, float size, float x, float y, const char *s, uint32_t base, int align) {
    return draw_text_impl(font, size, x, y, s, base, align, true);
}

float ui_text_shadow(int font, float size, float x, float y, const char *s, uint32_t col, int align) {
    float o = fmaxf(1.0f, size * 0.07f);
    uint32_t sh = UIC(0, 0, 0, (int)(0.75f * (float)(col >> 24)));
    draw_text_impl(font, size, x + o, y + o, s, sh, align, false);
    return draw_text_impl(font, size, x, y, s, col, align, false);
}

float ui_text_outline(int font, float size, float x, float y, const char *s, uint32_t col, uint32_t outline, int align) {
    float o = fmaxf(1.0f, size * 0.06f);
    static const float d[8][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
    for (int i = 0; i < 8; i++)
        draw_text_impl(font, size, x + d[i][0] * o, y + d[i][1] * o, s, outline, align, false);
    return draw_text_impl(font, size, x, y, s, col, align, false);
}

/*Shared word-wrap walker: calls emit for each line.*/
typedef void (*line_fn)(const char *s, int n, int line, void *ud);
static int wrap_lines(int font, float size, float w, const char *s, line_fn fn, void *ud) {
    int line = 0;
    while (*s) {
        const char *ls = s;
        const char *last_break = NULL;
        float lw = 0;
        const char *p = s;
        while (*p && *p != '\n') {
            const char *q = p;
            if (q[0] == '^' && q[1] >= '0' && q[1] <= '9') { p += 2; continue; }
            uint32_t cp = utf8_next(&q);
            float gw = ui_text_width_n(font, size, p, (int)(q - p));
            if (cp == ' ') last_break = p;
            if (lw + gw > w && p > ls) {
                if (last_break && last_break > ls) p = last_break;
                break;
            }
            lw += gw;
            p = q;
        }
        if (fn) fn(ls, (int)(p - ls), line, ud);
        line++;
        if (*p == '\n' || *p == ' ') p++;
        s = p;
    }
    return line;
}

typedef struct { int font; float size, x, y, line_h; uint32_t col; } WrapCtx;
static void wrap_emit(const char *s, int n, int line, void *ud) {
    WrapCtx *c = (WrapCtx *)ud;
    char buf[1024];
    if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
    memcpy(buf, s, (size_t)n);
    buf[n] = '\0';
    draw_text_impl(c->font, c->size, c->x, c->y + (float)line * c->line_h, buf, c->col, ALIGN_LEFT, true);
}

float ui_text_wrap(int font, float size, float x, float y, float w, const char *s, uint32_t col, float line_h) {
    if (!s) return 0;
    if (line_h <= 0) line_h = size * 1.3f;
    WrapCtx c = {font, size, x, y, line_h, col};
    int n = wrap_lines(font, size, w, s, wrap_emit, &c);
    return (float)n * line_h;
}

float ui_text_wrap_height(int font, float size, float w, const char *s, float line_h) {
    if (!s) return 0;
    if (line_h <= 0) line_h = size * 1.3f;
    return (float)wrap_lines(font, size, w, s, NULL, NULL) * line_h;
}

/* ------------------------------------------------------------------ */
/* Life cycle                                                          */
/* ------------------------------------------------------------------ */

bool ui_init(void) {
    g_atlas = calloc((size_t)ATLAS_W * ATLAS_H, 4);
    if (!g_atlas) return false;
    if (!load_font(FONT_BODY, asset_font_body) ||
        !load_font(FONT_BOLD, asset_font_bold) ||
        !load_font(FONT_TITLE, asset_font_title)) {
        fprintf(stderr, "[UI] cannot load embedded fonts\n");
        return false;
    }
    /*Icons strip + white texel*/
    ui_icons_rasterize(g_atlas, ATLAS_W, 0, FONT_AREA_H, ICON_CELL, ICON_PER_ROW);
    for (int y = WHITE_Y; y < ATLAS_H; y++)
        for (int x = WHITE_X; x < ATLAS_W; x++) {
            uint8_t *p = g_atlas + ((size_t)y * ATLAS_W + x) * 4;
            p[0] = p[1] = p[2] = p[3] = 255;
        }
    WU = (WHITE_X + 2.0f) / ATLAS_W;
    WV = (WHITE_Y + 2.0f) / ATLAS_H;
    return bake_fonts(1.0f);
}

void ui_shutdown(void) {
    for (int f = 0; f < FONT_COUNT; f++)
        for (int s = 0; s < g_fonts[f].nsizes; s++) free(g_fonts[f].sizes[s].chars);
    free(g_atlas);
    free(g_layers[0].v);
    free(g_layers[1].v);
    free(g_out.v);
}

const uint8_t *ui_atlas(int *w, int *h, uint32_t *version) {
    *w = ATLAS_W;
    *h = ATLAS_H;
    *version = g_atlas_version;
    return g_atlas;
}

void ui_begin_frame(int fb_w, int fb_h, float scale, const UIInput *in, float dt, double time) {
    if (scale < 0.5f) scale = 0.5f;
    if (fabsf(scale - g_baked_scale) > 0.01f) bake_fonts(scale);
    g_scale = scale;
    g_fbw = fb_w;
    g_fbh = fb_h;
    g_w = (float)fb_w / scale;
    g_h = (float)fb_h / scale;
    g_in = *in;
    g_dt = dt;
    g_time = time;
    g_layers[0].n = g_layers[1].n = 0;
    g_layer = 0;
    g_clip_depth = 0;
    g_clip_stack[0].x0 = 0;
    g_clip_stack[0].y0 = 0;
    g_clip_stack[0].x1 = (float)fb_w;
    g_clip_stack[0].y1 = (float)fb_h;
    g_mouse_captured = g_mouse_captured_next;
    g_mouse_captured_next = false;
    g_tip = false;
    g_focus_claim = 0;
    g_focus_seen = false;
}

static void draw_tooltip(void);

void ui_end_frame(UIFrame *out) {
    if (g_tip) draw_tooltip();
    /*Focus loss: a click that no text field claimed drops the focus.*/
    if (g_in.pressed[0] && g_focus_claim == 0) g_focus_id = 0;
    if (!g_focus_seen) g_focus_id = 0;
    g_out.n = 0;
    vlist_push(&g_out, g_layers[0].v, g_layers[0].n);
    vlist_push(&g_out, g_layers[1].v, g_layers[1].n);
    out->v = g_out.v;
    out->count = g_out.n;
    out->atlas = ui_atlas(&out->atlas_w, &out->atlas_h, &out->atlas_version);
}

float ui_width(void) { return g_w; }
float ui_height(void) { return g_h; }
float ui_scale(void) { return g_scale; }
double ui_time(void) { return g_time; }
const UIInput *ui_input(void) { return &g_in; }

void ui_layer(int layer) { g_layer = layer ? 1 : 0; }

void ui_push_clip(float x, float y, float w, float h) {
    if (g_clip_depth >= 15) return;
    Rect *p = &g_clip_stack[g_clip_depth];
    Rect r = {x * g_scale, y * g_scale, (x + w) * g_scale, (y + h) * g_scale};
    if (r.x0 < p->x0) r.x0 = p->x0;
    if (r.y0 < p->y0) r.y0 = p->y0;
    if (r.x1 > p->x1) r.x1 = p->x1;
    if (r.y1 > p->y1) r.y1 = p->y1;
    g_clip_stack[++g_clip_depth] = r;
}

void ui_pop_clip(void) {
    if (g_clip_depth > 0) g_clip_depth--;
}

/* ------------------------------------------------------------------ */
/* Interaction                                                         */
/* ------------------------------------------------------------------ */

static bool in_clip(float mx, float my) {
    const Rect *c = &g_clip_stack[g_clip_depth];
    float px = mx * g_scale, py = my * g_scale;
    return px >= c->x0 && px <= c->x1 && py >= c->y0 && py <= c->y1;
}

bool ui_hover(float x, float y, float w, float h) {
    return g_in.mx >= x && g_in.mx < x + w && g_in.my >= y && g_in.my < y + h && in_clip(g_in.mx, g_in.my);
}

bool ui_clicked(float x, float y, float w, float h, int button) {
    return g_in.pressed[button] && ui_hover(x, y, w, h);
}

void ui_block(float x, float y, float w, float h) {
    if (g_in.mx >= x && g_in.mx < x + w && g_in.my >= y && g_in.my < y + h) {
        g_mouse_captured_next = true;
        g_mouse_captured = true;
    }
}

bool ui_mouse_captured(void) { return g_mouse_captured || g_active_id != 0; }
bool ui_keyboard_captured(void) { return g_focus_id != 0; }
void ui_release_focus(void) { g_focus_id = 0; }

/* ------------------------------------------------------------------ */
/* Widgets                                                             */
/* ------------------------------------------------------------------ */

bool ui_button(const char *label, float x, float y, float w, float h, int style) {
    bool hov = ui_hover(x, y, w, h);
    bool down = hov && g_in.down[0];
    ui_block(x, y, w, h);
    float r = h * 0.18f;
    uint32_t top, bot, border, text = COL_TEXT;
    switch (style) {
    case BTN_PRIMARY:
        top = UIC(150, 104, 40, 255); bot = UIC(96, 62, 22, 255); border = COL_GOLD; text = UIC(255, 246, 220, 255);
        break;
    case BTN_DANGER:
        top = UIC(140, 40, 36, 255); bot = UIC(86, 22, 20, 255); border = UIC(220, 110, 90, 255);
        break;
    case BTN_GHOST:
        top = UIC(0, 0, 0, 0); bot = UIC(0, 0, 0, 0); border = UIC(0, 0, 0, 0); text = COL_TEXT_DIM;
        break;
    case BTN_TAB:
        top = UIC(30, 26, 36, 220); bot = UIC(22, 19, 27, 220); border = UIC(80, 66, 50, 255); text = COL_TEXT_DIM;
        break;
    case BTN_TAB_ACTIVE:
        top = UIC(70, 54, 34, 255); bot = UIC(40, 32, 24, 255); border = COL_GOLD; text = COL_GOLD;
        break;
    default:
        top = UIC(58, 50, 64, 255); bot = UIC(34, 29, 40, 255); border = UIC(120, 98, 64, 255);
        break;
    }
    if (hov) {
        top = ui_lerp_color(top, UIC(255, 220, 150, 255), 0.18f);
        bot = ui_lerp_color(bot, UIC(255, 220, 150, 255), 0.10f);
        border = ui_lerp_color(border, COL_GOLD, 0.6f);
        if (style == BTN_GHOST) { top = bot = UIC(255, 220, 150, 30); text = COL_TEXT; }
    }
    if (down) { uint32_t t = top; top = bot; bot = t; }
    if ((top >> 24) || (bot >> 24)) {
        ui_rrect_grad(x, y, w, h, r, top, bot);
        /*inner highlight*/
        ui_rect(x + r, y + 1, w - 2 * r, 1, UIC(255, 240, 210, hov ? 60 : 30));
    }
    if (border >> 24) ui_rrect_line(x, y, w, h, r, 1.2f, border);
    float fs = h * 0.46f;
    if (fs > 22) fs = 22;
    ui_text_shadow(FONT_BOLD, fs, x + w * 0.5f, y + h * 0.5f + (down ? 1 : 0), label, text, ALIGN_CENTER | ALIGN_MIDDLE);
    return hov && g_in.pressed[0];
}

bool ui_button_icon(int icon, const char *tip, float x, float y, float size, bool active) {
    bool hov = ui_hover(x, y, size, size);
    ui_block(x, y, size, size);
    uint32_t bg = active ? UIC(88, 66, 36, 240) : UIC(28, 24, 32, 220);
    if (hov) bg = ui_lerp_color(bg, UIC(255, 220, 150, 255), 0.2f);
    ui_rrect(x, y, size, size, size * 0.18f, bg);
    ui_rrect_line(x, y, size, size, size * 0.18f, 1.2f, active || hov ? COL_GOLD : UIC(100, 84, 60, 255));
    float pad = size * 0.16f;
    ui_icon(icon, x + pad, y + pad, size - 2 * pad, COL_WHITE);
    if (hov && tip) ui_tooltip(tip, NULL);
    return hov && g_in.pressed[0];
}

bool ui_checkbox(const char *label, float x, float y, bool *value) {
    float s = 20;
    float tw = ui_text_width(FONT_BODY, 17, label);
    bool hov = ui_hover(x, y, s + 10 + tw, s);
    ui_block(x, y, s + 10 + tw, s);
    ui_rrect(x, y, s, s, 4, hov ? UIC(60, 50, 62, 255) : UIC(36, 30, 40, 255));
    ui_rrect_line(x, y, s, s, 4, 1.2f, hov ? COL_GOLD : UIC(120, 98, 64, 255));
    if (*value) {
        ui_line(x + 4, y + 10, x + 8, y + 15, 2.5f, COL_GOLD);
        ui_line(x + 8, y + 15, x + 16, y + 5, 2.5f, COL_GOLD);
    }
    ui_text(FONT_BODY, 17, x + s + 10, y + s * 0.5f, label, COL_TEXT, ALIGN_MIDDLE);
    if (hov && g_in.pressed[0]) {
        *value = !*value;
        return true;
    }
    return false;
}

bool ui_slider(const char *id, float x, float y, float w, float *value, float minv, float maxv) {
    uint32_t hid = hash_str(id);
    float h = 18;
    bool hov = ui_hover(x - 8, y - 4, w + 16, h + 8);
    ui_block(x - 8, y - 4, w + 16, h + 8);
    if (hov && g_in.pressed[0]) g_active_id = hid;
    if (g_active_id == hid && !g_in.down[0]) g_active_id = 0;
    bool changed = false;
    if (g_active_id == hid) {
        float t = clampf((g_in.mx - x) / w, 0, 1);
        float nv = minv + (maxv - minv) * t;
        if (nv != *value) { *value = nv; changed = true; }
    }
    if (hov && g_in.wheel != 0) {
        *value = clampf(*value + (maxv - minv) * 0.05f * g_in.wheel, minv, maxv);
        changed = true;
    }
    float t = (maxv > minv) ? clampf((*value - minv) / (maxv - minv), 0, 1) : 0;
    ui_rrect(x, y + h * 0.5f - 3, w, 6, 3, UIC(20, 16, 22, 255));
    ui_rrect_grad(x, y + h * 0.5f - 3, w * t, 6, 3, COL_GOLD, COL_GOLD_DARK);
    float kx = x + w * t;
    ui_circle(kx, y + h * 0.5f, 9, (hov || g_active_id == hid) ? UIC(255, 230, 170, 255) : COL_GOLD);
    ui_circle(kx, y + h * 0.5f, 5, UIC(60, 44, 24, 255));
    return changed;
}

bool ui_text_field_focused(const char *id) { return g_focus_id == hash_str(id); }

void ui_focus(const char *id) {
    g_focus_id = hash_str(id);
    g_caret = 1 << 30;
}

static int utf8_prev(const char *s, int pos) {
    if (pos <= 0) return 0;
    pos--;
    while (pos > 0 && ((unsigned char)s[pos] & 0xC0) == 0x80) pos--;
    return pos;
}
static int utf8_nextpos(const char *s, int pos) {
    int n = (int)strlen(s);
    if (pos >= n) return n;
    pos++;
    while (pos < n && ((unsigned char)s[pos] & 0xC0) == 0x80) pos++;
    return pos;
}

bool ui_text_field(const char *id, float x, float y, float w, float h, char *buf, int cap, int flags,
                   const char *placeholder) {
    uint32_t hid = hash_str(id);
    bool hov = ui_hover(x, y, w, h);
    ui_block(x, y, w, h);
    if (flags & TF_FOCUS) { if (g_focus_id != hid) { g_focus_id = hid; g_caret = 1 << 30; } }
    if (hov && g_in.pressed[0]) {
        g_focus_id = hid;
        g_caret = 1 << 30;
    }
    bool focused = g_focus_id == hid;
    if (focused) { g_focus_claim = hid; g_focus_seen = true; }
    if (hov && g_in.pressed[0]) g_focus_claim = hid;
    bool submit = false;
    int len = (int)strlen(buf);
    /*the caret is shared by all fields: only the focused one may clamp it*/
    if (focused && g_caret > len) g_caret = len;
    if (focused) {
        for (int i = 0; i < g_in.nchars; i++) {
            uint32_t cp = g_in.chars[i];
            if (cp < 32 || cp == 127) continue;
            if ((flags & TF_DIGITS) && (cp < '0' || cp > '9')) continue;
            if (cp > 255) cp = '?';
            char enc[4];
            int el = ui_utf8_encode(cp, enc);
            if (len + el >= cap) break;
            memmove(buf + g_caret + el, buf + g_caret, (size_t)(len - g_caret + 1));
            memcpy(buf + g_caret, enc, (size_t)el);
            g_caret += el;
            len += el;
        }
        g_in.nchars = 0; /*consumed: a field focused later this frame must not get them too*/
        if (g_in.keys[UIK_V_PASTE] && g_in.clipboard) {
            for (const char *p = g_in.clipboard; *p && len < cap - 1; p++) {
                if ((unsigned char)*p < 32) continue;
                if ((flags & TF_DIGITS) && (*p < '0' || *p > '9')) continue;
                memmove(buf + g_caret + 1, buf + g_caret, (size_t)(len - g_caret + 1));
                buf[g_caret++] = *p;
                len++;
            }
        }
        for (int k = 0; k < g_in.keys[UIK_BACKSPACE] && g_caret > 0; k++) {
            int p = utf8_prev(buf, g_caret);
            memmove(buf + p, buf + g_caret, (size_t)(len - g_caret + 1));
            len -= g_caret - p;
            g_caret = p;
        }
        for (int k = 0; k < g_in.keys[UIK_DELETE] && g_caret < len; k++) {
            int p = utf8_nextpos(buf, g_caret);
            memmove(buf + g_caret, buf + p, (size_t)(len - p + 1));
            len -= p - g_caret;
        }
        for (int k = 0; k < g_in.keys[UIK_LEFT]; k++) g_caret = utf8_prev(buf, g_caret);
        for (int k = 0; k < g_in.keys[UIK_RIGHT]; k++) g_caret = utf8_nextpos(buf, g_caret);
        if (g_in.keys[UIK_HOME]) g_caret = 0;
        if (g_in.keys[UIK_END]) g_caret = len;
        if (g_in.keys[UIK_ENTER]) submit = true;
    }
    float r = 5;
    ui_rrect_grad(x, y, w, h, r, UIC(10, 8, 12, 235), UIC(24, 20, 28, 235));
    ui_rrect_line(x, y, w, h, r, focused ? 1.6f : 1.1f,
                  focused ? COL_GOLD : (hov ? UIC(150, 120, 80, 255) : UIC(90, 76, 58, 255)));
    float fs = h * 0.5f;
    if (fs > 20) fs = 20;
    float tx = x + 10, tw = w - 20;
    char shown[512];
    if (flags & TF_PASSWORD) {
        int n = 0;
        for (const char *p = buf; *p && n < (int)sizeof(shown) - 1; p++)
            if (((unsigned char)*p & 0xC0) != 0x80) shown[n++] = '*';
        shown[n] = '\0';
    } else {
        snprintf(shown, sizeof(shown), "%s", buf);
    }
    /*caret position in the shown string*/
    int caret_pos = focused ? (g_caret < len ? g_caret : len) : len;
    int caret_shown = caret_pos;
    if (flags & TF_PASSWORD) {
        int n = 0;
        for (int i = 0; i < caret_pos; i++)
            if (((unsigned char)buf[i] & 0xC0) != 0x80) n++;
        caret_shown = n;
    }
    float cw = ui_text_width_n(FONT_BODY, fs, shown, caret_shown);
    float scroll = 0;
    if (cw > tw) scroll = cw - tw;
    ui_push_clip(x + 4, y, w - 8, h);
    if (!buf[0] && placeholder && !focused)
        ui_text(FONT_BODY, fs, tx, y + h * 0.5f, placeholder, COL_TEXT_FAINT, ALIGN_MIDDLE);
    ui_text(FONT_BODY, fs, tx - scroll, y + h * 0.5f, shown, COL_TEXT, ALIGN_MIDDLE);
    if (focused && fmod(g_time, 1.0) < 0.6)
        ui_rect(tx - scroll + cw, y + h * 0.2f, 1.6f, h * 0.6f, COL_GOLD);
    ui_pop_clip();
    return submit;
}

void ui_frame_ornament(float x, float y, float w, float h, uint32_t col) {
    ui_rrect_line(x, y, w, h, 6, 1.4f, col);
    ui_rrect_line(x + 4, y + 4, w - 8, h - 8, 4, 1.0f, ui_alpha(col, 0.35f));
    float d = 5;
    ui_diamond(x, y, d, col);
    ui_diamond(x + w, y, d, col);
    ui_diamond(x, y + h, d, col);
    ui_diamond(x + w, y + h, d, col);
    ui_diamond(x + w * 0.5f, y, d * 0.8f, col);
    ui_diamond(x + w * 0.5f, y + h, d * 0.8f, col);
}

void ui_panel(float x, float y, float w, float h) {
    ui_block(x, y, w, h);
    ui_shadow(x, y, w, h, 8, 14, 0.55f);
    ui_rrect_grad(x, y, w, h, 7, UIC(30, 25, 36, 238), UIC(14, 12, 18, 242));
    ui_frame_ornament(x, y, w, h, UIC(176, 138, 70, 255));
}

bool ui_window(const char *title, float x, float y, float w, float h, bool closable) {
    ui_panel(x, y, w, h);
    float th = 40;
    ui_rect_grad(x + 6, y + 6, w - 12, th - 6, UIC(70, 52, 32, 200), UIC(30, 24, 20, 120));
    ui_line(x + 12, y + th, x + w - 12, y + th, 1.2f, UIC(176, 138, 70, 200));
    ui_text_shadow(FONT_TITLE, 22, x + w * 0.5f, y + th * 0.5f + 3, title, COL_GOLD, ALIGN_CENTER | ALIGN_MIDDLE);
    bool open = true;
    if (closable) {
        float bs = 26, bx = x + w - bs - 10, by = y + 9;
        bool hov = ui_hover(bx, by, bs, bs);
        ui_rrect(bx, by, bs, bs, 5, hov ? UIC(150, 50, 40, 255) : UIC(50, 40, 46, 220));
        ui_line(bx + 8, by + 8, bx + bs - 8, by + bs - 8, 2.2f, COL_TEXT);
        ui_line(bx + bs - 8, by + 8, bx + 8, by + bs - 8, 2.2f, COL_TEXT);
        if (hov) ui_tooltip("Close", "Esc also closes the window.");
        if (hov && g_in.pressed[0]) open = false;
    }
    return open;
}

void ui_bar(float x, float y, float w, float h, float frac, uint32_t col_top, uint32_t col_bot, const char *label) {
    frac = clampf(frac, 0, 1);
    float r = h * 0.3f;
    ui_rrect(x - 1, y - 1, w + 2, h + 2, r + 1, UIC(0, 0, 0, 200));
    ui_rrect_grad(x, y, w, h, r, UIC(26, 20, 24, 255), UIC(12, 10, 12, 255));
    if (frac > 0.002f) {
        ui_rrect_grad(x, y, fmaxf(w * frac, r * 2), h, r, col_top, col_bot);
        ui_rect(x + r, y + 1, fmaxf(0, w * frac - 2 * r), h * 0.35f, UIC(255, 255, 255, 40));
    }
    ui_rrect_line(x - 1, y - 1, w + 2, h + 2, r + 1, 1.0f, UIC(120, 98, 64, 220));
    if (label) {
        float fs = h * 0.72f;
        if (fs > 18) fs = 18;
        ui_text_outline(FONT_BOLD, fs, x + w * 0.5f, y + h * 0.5f, label, COL_WHITE, UIC(0, 0, 0, 200), ALIGN_CENTER | ALIGN_MIDDLE);
    }
}

void ui_header(const char *text, float x, float y, float w) {
    ui_text(FONT_TITLE, 17, x, y, text, COL_GOLD, ALIGN_LEFT);
    float tw = ui_text_width(FONT_TITLE, 17, text);
    ui_rect_hgrad(x + tw + 10, y + 11, w - tw - 10, 1.2f, UIC(176, 138, 70, 200), UIC(176, 138, 70, 0));
}

void ui_tooltip_color(const char *title, uint32_t title_col, const char *body) {
    snprintf(g_tip_title, sizeof(g_tip_title), "%s", title ? title : "");
    snprintf(g_tip_body, sizeof(g_tip_body), "%s", body ? body : "");
    g_tip_col = title_col;
    g_tip = true;
}

void ui_tooltip(const char *title, const char *body) { ui_tooltip_color(title, COL_GOLD, body); }

static void draw_tooltip(void) {
    int saved = g_layer;
    int saved_clip = g_clip_depth;
    g_layer = 1;
    g_clip_depth = 0;
    float w = 320;
    float tw = ui_text_width(FONT_BOLD, 17, g_tip_title) + 24;
    bool has_body = g_tip_body[0] != '\0';
    if (!has_body) w = tw;
    if (tw > w) w = tw;
    float bh = has_body ? ui_text_wrap_height(FONT_BODY, 15, w - 24, g_tip_body, 19) : 0;
    float h = 12 + 22 + (has_body ? bh + 6 : -4) + 8;
    float x = g_in.mx + 18, y = g_in.my + 20;
    if (x + w > g_w - 4) x = g_in.mx - w - 12;
    if (y + h > g_h - 4) y = g_h - h - 4;
    if (x < 4) x = 4;
    ui_shadow(x, y, w, h, 6, 10, 0.6f);
    ui_rrect_grad(x, y, w, h, 6, UIC(34, 28, 40, 248), UIC(18, 15, 22, 250));
    ui_rrect_line(x, y, w, h, 6, 1.2f, UIC(176, 138, 70, 255));
    ui_text(FONT_BOLD, 17, x + 12, y + 9, g_tip_title, g_tip_col, ALIGN_LEFT);
    if (has_body) ui_text_wrap(FONT_BODY, 15, x + 12, y + 34, w - 24, g_tip_body, COL_TEXT, 19);
    g_layer = saved;
    g_clip_depth = saved_clip;
}

/* ------------------------------------------------------------------ */
/* Scroll areas                                                        */
/* ------------------------------------------------------------------ */

typedef struct { uint32_t id; float off; } ScrollState;
static ScrollState g_scrolls[64];
static int g_nscrolls;
static struct { ScrollState *s; float x, y, w, h, content; } g_scroll_cur[4];
static int g_scroll_depth;

float ui_scroll_begin(const char *id, float x, float y, float w, float h, float content_h) {
    uint32_t hid = hash_str(id);
    ScrollState *s = NULL;
    for (int i = 0; i < g_nscrolls; i++)
        if (g_scrolls[i].id == hid) { s = &g_scrolls[i]; break; }
    if (!s) {
        if (g_nscrolls < 64) {
            s = &g_scrolls[g_nscrolls++];
        } else {
            s = &g_scrolls[hid % 64];
        }
        s->id = hid;
        s->off = 0;
    }
    float maxoff = fmaxf(0, content_h - h);
    if (ui_hover(x, y, w, h) && g_in.wheel != 0) s->off -= g_in.wheel * 48.0f;
    /*scrollbar drag*/
    if (maxoff > 0) {
        float bx = x + w - 8, bh = h * h / content_h;
        float by = y + (h - bh) * (s->off / maxoff);
        bool hov = ui_hover(bx - 4, y, 12, h);
        if (hov && g_in.pressed[0]) g_active_id = hid;
        if (g_active_id == hid) {
            if (!g_in.down[0]) g_active_id = 0;
            else s->off = (g_in.my - y - bh * 0.5f) / fmaxf(1, h - bh) * maxoff;
        }
        if (s->off < 0) s->off = 0;
        if (s->off > maxoff) s->off = maxoff;
        by = y + (h - bh) * (s->off / maxoff);
        ui_rrect(bx, y, 6, h, 3, UIC(0, 0, 0, 120));
        ui_rrect(bx, by, 6, bh, 3, hov || g_active_id == hid ? COL_GOLD : UIC(140, 112, 70, 220));
    } else {
        s->off = 0;
    }
    if (g_scroll_depth < 4) {
        g_scroll_cur[g_scroll_depth].s = s;
        g_scroll_cur[g_scroll_depth].x = x;
        g_scroll_cur[g_scroll_depth].y = y;
        g_scroll_cur[g_scroll_depth].w = w;
        g_scroll_cur[g_scroll_depth].h = h;
        g_scroll_cur[g_scroll_depth].content = content_h;
        g_scroll_depth++;
    }
    ui_push_clip(x, y, w - 10, h);
    return -s->off;
}

void ui_scroll_end(void) {
    ui_pop_clip();
    if (g_scroll_depth > 0) g_scroll_depth--;
}

/* ------------------------------------------------------------------ */
/* Toasts                                                              */
/* ------------------------------------------------------------------ */

typedef struct { char text[160]; uint32_t col; double t0; } Toast;
static Toast g_toasts[6];
static int g_ntoasts;

void ui_toast(const char *text, uint32_t col) {
    if (g_ntoasts == 6) {
        memmove(&g_toasts[0], &g_toasts[1], sizeof(Toast) * 5);
        g_ntoasts = 5;
    }
    Toast *t = &g_toasts[g_ntoasts++];
    snprintf(t->text, sizeof(t->text), "%s", text);
    t->col = col;
    t->t0 = g_time;
}

void ui_draw_toasts(void) {
    int saved = g_layer;
    g_layer = 1;
    float y = g_h * 0.20f;
    for (int i = 0; i < g_ntoasts; i++) {
        Toast *t = &g_toasts[i];
        float age = (float)(g_time - t->t0);
        if (age > 4.0f) {
            memmove(&g_toasts[i], &g_toasts[i + 1], sizeof(Toast) * (size_t)(g_ntoasts - i - 1));
            g_ntoasts--;
            i--;
            continue;
        }
        float a = clampf(age * 4.0f, 0, 1) * clampf((4.0f - age) * 1.5f, 0, 1);
        float tw = ui_text_width(FONT_BOLD, 20, t->text) + 40;
        float x = g_w * 0.5f - tw * 0.5f;
        float yy = y - (1.0f - clampf(age * 4, 0, 1)) * 14.0f;
        ui_rrect(x, yy, tw, 36, 8, UIC(12, 10, 14, (int)(200 * a)));
        ui_rrect_line(x, yy, tw, 36, 8, 1.2f, ui_alpha(t->col, a * 0.8f));
        ui_text_shadow(FONT_BOLD, 20, g_w * 0.5f, yy + 18, t->text, ui_alpha(t->col, a), ALIGN_CENTER | ALIGN_MIDDLE);
        y += 44;
    }
    g_layer = saved;
}
