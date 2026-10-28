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

/*CPU mesh builder: growable SVert triangle lists plus the handful of
 * primitives every procedural model is made of.*/
#ifndef MESH_H
#define MESH_H

#include "gfx.h"
#include "mathx.h"
#include <stdint.h>

typedef struct {
    SVert *v;
    uint32_t n, cap;
} Mesh;

/*Material/appearance of a primitive*/
typedef struct {
    int layer;       /*MAT_* (textures.h)*/
    uint32_t col;    /*RGBA packed (UIC order)*/
    float glow;      /*0..1 emissive*/
    int flags;       /*SV_**/
    float uvs;       /*texture repeats per world unit*/
} Mat;

static inline Mat matc(int layer, uint32_t col) { Mat m = {layer, col, 0.0f, 0, 1.0f}; return m; }
static inline Mat matg(int layer, uint32_t col, float glow) { Mat m = {layer, col, glow, 0, 1.0f}; return m; }

#define RGBA8(r, g, b, a) ((uint32_t)(r) | ((uint32_t)(g) << 8) | ((uint32_t)(b) << 16) | ((uint32_t)(a) << 24))
uint32_t col_scale(uint32_t c, float s);  /*rgb * s (alpha kept)*/
uint32_t col_mix(uint32_t a, uint32_t b, float t);
uint32_t col_hsv(float h, float s, float v, uint8_t a);

void mesh_reset(Mesh *m);
void mesh_free(Mesh *m);
SVert *mesh_alloc(Mesh *m, uint32_t n); /*returns n uninitialised vertices*/
void mesh_append(Mesh *dst, const Mesh *src);

void mesh_tri(Mesh *m, v3 a, v3 b, v3 c, float ua, float va, float ub, float vb, float uc, float vc,
              v3 n, const Mat *mt);
/*Quad p0..p3 counter-clockwise seen from the normal side.*/
void mesh_quad(Mesh *m, v3 p0, v3 p1, v3 p2, v3 p3, v3 n, const Mat *mt,
               float u0, float v0, float u1, float v1);
/*Per-vertex colours (ambient occlusion on floors).*/
void mesh_quad_c(Mesh *m, v3 p0, v3 p1, v3 p2, v3 p3, v3 n, const Mat *mt,
                 float u0, float v0, float u1, float v1, const uint32_t cols[4], const float cut[4]);
/*Oriented box: centre c, half extents h, rotation r.*/
void mesh_box(Mesh *m, v3 c, v3 h, m3 r, const Mat *mt);
/*Box hanging from a pivot: the box extends from the pivot along -Y
 * (length len, half sizes hx/hz) after rotation r (limbs).*/
void mesh_limb(Mesh *m, v3 pivot, float hx, float len, float hz, m3 r, const Mat *mt);
/*n-sided prism / pyramid: base centre b, radius rb at the base and rt
 * at the top (rt = 0 gives a cone/spike), height h along r's +Y.*/
void mesh_prism(Mesh *m, v3 b, float rb, float rt, float h, int sides, m3 r, const Mat *mt, bool caps);
/*Camera-facing sprite (particles): right/up are camera axes.*/
void mesh_billboard(Mesh *m, v3 c, float size, v3 right, v3 up, float rot, const Mat *mt);
/*Flat horizontal decal at height y (shadows, runes).*/
void mesh_decal(Mesh *m, float x, float z, float y, float size, float rot, const Mat *mt);

#endif
