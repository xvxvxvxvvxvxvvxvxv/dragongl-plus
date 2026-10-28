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

#include "mesh.h"
#include <stdlib.h>
#include <string.h>

uint32_t col_scale(uint32_t c, float s) {
    uint32_t r = 0;
    for (int i = 0; i < 3; i++) {
        float v = (float)((c >> (i * 8)) & 255) * s;
        uint32_t u = (uint32_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
        r |= u << (i * 8);
    }
    return r | (c & 0xFF000000u);
}

uint32_t col_mix(uint32_t a, uint32_t b, float t) {
    uint32_t r = 0;
    for (int i = 0; i < 4; i++) {
        float ca = (float)((a >> (i * 8)) & 255), cb = (float)((b >> (i * 8)) & 255);
        float v = ca + (cb - ca) * t;
        uint32_t u = (uint32_t)(v < 0 ? 0 : (v > 255 ? 255 : v + 0.5f));
        r |= u << (i * 8);
    }
    return r;
}

uint32_t col_hsv(float h, float s, float v, uint8_t a) {
    h = h - floorf(h);
    float r, g, b;
    int i = (int)(h * 6.0f);
    float f = h * 6.0f - (float)i;
    float p = v * (1 - s), q = v * (1 - f * s), t = v * (1 - (1 - f) * s);
    switch (i % 6) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
    }
    return RGBA8((int)(r * 255), (int)(g * 255), (int)(b * 255), a);
}

void mesh_reset(Mesh *m) { m->n = 0; }

void mesh_free(Mesh *m) {
    free(m->v);
    m->v = NULL;
    m->n = m->cap = 0;
}

SVert *mesh_alloc(Mesh *m, uint32_t n) {
    if (m->n + n > m->cap) {
        uint32_t nc = m->cap ? m->cap : 4096;
        while (nc < m->n + n) nc *= 2;
        SVert *nv = realloc(m->v, (size_t)nc * sizeof(SVert));
        if (!nv) return NULL;
        m->v = nv;
        m->cap = nc;
    }
    SVert *r = m->v + m->n;
    m->n += n;
    return r;
}

void mesh_append(Mesh *dst, const Mesh *src) {
    if (!src->n) return;
    SVert *d = mesh_alloc(dst, src->n);
    if (d) memcpy(d, src->v, (size_t)src->n * sizeof(SVert));
}

static inline void sv_set(SVert *s, v3 p, v3 n, float u, float v, const Mat *mt, uint32_t col, float cut) {
    s->pos[0] = p.x; s->pos[1] = p.y; s->pos[2] = p.z;
    s->nrm[0] = n.x; s->nrm[1] = n.y; s->nrm[2] = n.z;
    s->uv[0] = u; s->uv[1] = v;
    s->layer = (float)mt->layer;
    s->glow = mt->glow;
    s->flags = (float)(mt->flags | (cut > 0.5f ? SV_CUT : 0));
    memcpy(s->col, &col, 4);
}

void mesh_tri(Mesh *m, v3 a, v3 b, v3 c, float ua, float va, float ub, float vb, float uc, float vc,
              v3 n, const Mat *mt) {
    SVert *s = mesh_alloc(m, 3);
    if (!s) return;
    sv_set(&s[0], a, n, ua, va, mt, mt->col, 0);
    sv_set(&s[1], b, n, ub, vb, mt, mt->col, 0);
    sv_set(&s[2], c, n, uc, vc, mt, mt->col, 0);
}

void mesh_quad_c(Mesh *m, v3 p0, v3 p1, v3 p2, v3 p3, v3 n, const Mat *mt,
                 float u0, float v0, float u1, float v1, const uint32_t cols[4], const float cut[4]) {
    SVert *s = mesh_alloc(m, 6);
    if (!s) return;
    static const float zc[4] = {0, 0, 0, 0};
    if (!cut) cut = zc;
    uint32_t c[4];
    for (int i = 0; i < 4; i++) c[i] = cols ? cols[i] : mt->col;
    sv_set(&s[0], p0, n, u0, v0, mt, c[0], cut[0]);
    sv_set(&s[1], p1, n, u1, v0, mt, c[1], cut[1]);
    sv_set(&s[2], p2, n, u1, v1, mt, c[2], cut[2]);
    sv_set(&s[3], p0, n, u0, v0, mt, c[0], cut[0]);
    sv_set(&s[4], p2, n, u1, v1, mt, c[2], cut[2]);
    sv_set(&s[5], p3, n, u0, v1, mt, c[3], cut[3]);
}

void mesh_quad(Mesh *m, v3 p0, v3 p1, v3 p2, v3 p3, v3 n, const Mat *mt,
               float u0, float v0, float u1, float v1) {
    mesh_quad_c(m, p0, p1, p2, p3, n, mt, u0, v0, u1, v1, NULL, NULL);
}

void mesh_box(Mesh *m, v3 c, v3 h, m3 r, const Mat *mt) {
    /*face: normal, then the 4 corners (CCW from outside)*/
    static const float F[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    static const float C[6][4][3] = {
        {{1, -1, 1}, {1, -1, -1}, {1, 1, -1}, {1, 1, 1}},
        {{-1, -1, -1}, {-1, -1, 1}, {-1, 1, 1}, {-1, 1, -1}},
        {{-1, 1, 1}, {1, 1, 1}, {1, 1, -1}, {-1, 1, -1}},
        {{-1, -1, -1}, {1, -1, -1}, {1, -1, 1}, {-1, -1, 1}},
        {{-1, -1, 1}, {1, -1, 1}, {1, 1, 1}, {-1, 1, 1}},
        {{1, -1, -1}, {-1, -1, -1}, {-1, 1, -1}, {1, 1, -1}},
    };
    float us = mt->uvs;
    for (int f = 0; f < 6; f++) {
        v3 p[4];
        for (int k = 0; k < 4; k++) {
            v3 lp = v3m(C[f][k][0] * h.x, C[f][k][1] * h.y, C[f][k][2] * h.z);
            p[k] = v3add(c, m3apply(r, lp));
        }
        v3 n = m3apply(r, v3m(F[f][0], F[f][1], F[f][2]));
        /*UV extent from the face size (texel density)*/
        float sw, sh;
        if (f < 2) { sw = h.z; sh = h.y; }
        else if (f < 4) { sw = h.x; sh = h.z; }
        else { sw = h.x; sh = h.y; }
        mesh_quad(m, p[0], p[1], p[2], p[3], n, mt, 0, 0, 2 * sw * us, 2 * sh * us);
    }
}

void mesh_limb(Mesh *m, v3 pivot, float hx, float len, float hz, m3 r, const Mat *mt) {
    v3 off = m3apply(r, v3m(0, -len * 0.5f, 0));
    mesh_box(m, v3add(pivot, off), v3m(hx, len * 0.5f, hz), r, mt);
}

void mesh_prism(Mesh *m, v3 b, float rb, float rt, float h, int sides, m3 r, const Mat *mt, bool caps) {
    if (sides < 3) sides = 3;
    if (sides > 16) sides = 16;
    v3 top = v3add(b, m3apply(r, v3m(0, h, 0)));
    for (int i = 0; i < sides; i++) {
        float a0 = MX_TAU * (float)i / (float)sides, a1 = MX_TAU * (float)(i + 1) / (float)sides;
        v3 d0 = v3m(cosf(a0), 0, sinf(a0)), d1 = v3m(cosf(a1), 0, sinf(a1));
        v3 b0 = v3add(b, m3apply(r, v3scale(d0, rb))), b1 = v3add(b, m3apply(r, v3scale(d1, rb)));
        float am = (a0 + a1) * 0.5f;
        float slope = (rb - rt) / (h > 1e-4f ? h : 1e-4f);
        v3 n = v3norm(m3apply(r, v3m(cosf(am), slope, sinf(am))));
        float u0 = (float)i / (float)sides, u1 = (float)(i + 1) / (float)sides;
        if (rt <= 1e-4f) {
            mesh_tri(m, b1, b0, top, u1, 0, u0, 0, (u0 + u1) * 0.5f, h * mt->uvs, n, mt);
        } else {
            v3 t0 = v3add(top, m3apply(r, v3scale(d0, rt))), t1 = v3add(top, m3apply(r, v3scale(d1, rt)));
            mesh_quad(m, b1, b0, t0, t1, n, mt, u1, 0, u0, h * mt->uvs);
        }
        if (caps) {
            v3 dn = m3apply(r, v3m(0, -1, 0));
            mesh_tri(m, b0, b1, b, 0, 0, 1, 0, 0.5f, 0.5f, dn, mt);
            if (rt > 1e-4f) {
                v3 t0 = v3add(top, m3apply(r, v3scale(d0, rt))), t1 = v3add(top, m3apply(r, v3scale(d1, rt)));
                v3 up = m3apply(r, v3m(0, 1, 0));
                mesh_tri(m, t1, t0, top, 0, 0, 1, 0, 0.5f, 0.5f, up, mt);
            }
        }
    }
}

void mesh_billboard(Mesh *m, v3 c, float size, v3 right, v3 up, float rot, const Mat *mt) {
    float cs = cosf(rot) * size, sn = sinf(rot) * size;
    v3 rr = v3add(v3scale(right, cs), v3scale(up, sn));
    v3 uu = v3add(v3scale(up, cs), v3scale(right, -sn));
    v3 p0 = v3sub(v3sub(c, rr), uu), p1 = v3sub(v3add(c, rr), uu);
    v3 p2 = v3add(v3add(c, rr), uu), p3 = v3add(v3sub(c, rr), uu);
    v3 n = v3norm(v3cross(right, up));
    mesh_quad(m, p0, p1, p2, p3, n, mt, 0, 1, 1, 0);
}

void mesh_decal(Mesh *m, float x, float z, float y, float size, float rot, const Mat *mt) {
    float cs = cosf(rot) * size, sn = sinf(rot) * size;
    v3 a = v3m(x - cs + sn, y, z - sn - cs);
    v3 b = v3m(x + cs + sn, y, z + sn - cs);
    v3 c = v3m(x + cs - sn, y, z + sn + cs);
    v3 d = v3m(x - cs - sn, y, z - sn + cs);
    mesh_quad(m, d, c, b, a, v3m(0, 1, 0), mt, 0, 0, 1, 1);
}
