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

/*Small header-only vector/matrix library shared by the scene builder
 * and both renderers. Matrices are column-major (OpenGL convention);
 * the Vulkan backend flips Y/depth in its projection.*/
#ifndef MATHX_H
#define MATHX_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define MX_PI 3.14159265358979f
#define MX_TAU 6.28318530717958f

typedef struct { float x, y, z; } v3;

static inline v3 v3m(float x, float y, float z) { v3 r = {x, y, z}; return r; }
static inline v3 v3add(v3 a, v3 b) { return v3m(a.x + b.x, a.y + b.y, a.z + b.z); }
static inline v3 v3sub(v3 a, v3 b) { return v3m(a.x - b.x, a.y - b.y, a.z - b.z); }
static inline v3 v3scale(v3 a, float s) { return v3m(a.x * s, a.y * s, a.z * s); }
static inline float v3dot(v3 a, v3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline v3 v3cross(v3 a, v3 b) {
    return v3m(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}
static inline float v3len(v3 a) { return sqrtf(v3dot(a, a)); }
static inline v3 v3norm(v3 a) {
    float l = v3len(a);
    return l > 1e-8f ? v3scale(a, 1.0f / l) : v3m(0, 1, 0);
}
static inline v3 v3lerp(v3 a, v3 b, float t) {
    return v3m(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t);
}

static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
static inline float smoothstepf(float e0, float e1, float x) {
    float t = clampf((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
/*Frame-rate independent exponential approach: rate = 1/s.*/
static inline float damp(float cur, float target, float rate, float dt) {
    return lerpf(cur, target, 1.0f - expf(-rate * dt));
}
/*Shortest signed angle difference in radians.*/
static inline float angle_diff(float a, float b) {
    float d = fmodf(b - a + MX_PI, MX_TAU);
    if (d < 0) d += MX_TAU;
    return d - MX_PI;
}

/*Deterministic hashes (procedural textures, props, colour variation).*/
static inline uint32_t hash_u32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
static inline uint32_t hash2i(int x, int y) {
    return hash_u32((uint32_t)x * 73856093u ^ hash_u32((uint32_t)y * 19349663u + 0x9e3779b9u));
}
static inline float hash2f(int x, int y) { return (float)(hash2i(x, y) & 0xFFFFFF) / 16777215.0f; }
static inline float hash1f(uint32_t s) { return (float)(hash_u32(s) & 0xFFFFFF) / 16777215.0f; }

/* ---------------------------- 4x4 matrices ---------------------------- */
typedef struct { float m[16]; } m4;

static inline m4 m4identity(void) {
    m4 r;
    memset(&r, 0, sizeof(r));
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
    return r;
}
static inline m4 m4mul(m4 a, m4 b) {
    m4 r;
    for (int c = 0; c < 4; c++)
        for (int rr = 0; rr < 4; rr++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a.m[k * 4 + rr] * b.m[c * 4 + k];
            r.m[c * 4 + rr] = s;
        }
    return r;
}
static inline m4 m4perspective(float fovy, float aspect, float zn, float zf) {
    m4 r;
    memset(&r, 0, sizeof(r));
    float f = 1.0f / tanf(fovy * 0.5f);
    r.m[0] = f / aspect;
    r.m[5] = f;
    r.m[10] = (zf + zn) / (zn - zf);
    r.m[11] = -1.0f;
    r.m[14] = (2.0f * zf * zn) / (zn - zf);
    return r;
}
static inline m4 m4lookat(v3 eye, v3 at, v3 up) {
    v3 f = v3norm(v3sub(at, eye));
    v3 s = v3norm(v3cross(f, up));
    v3 u = v3cross(s, f);
    m4 r = m4identity();
    r.m[0] = s.x; r.m[4] = s.y; r.m[8] = s.z;
    r.m[1] = u.x; r.m[5] = u.y; r.m[9] = u.z;
    r.m[2] = -f.x; r.m[6] = -f.y; r.m[10] = -f.z;
    r.m[12] = -v3dot(s, eye);
    r.m[13] = -v3dot(u, eye);
    r.m[14] = v3dot(f, eye);
    return r;
}
static inline void m4xform4(const m4 *a, const float in[4], float out[4]) {
    for (int r = 0; r < 4; r++)
        out[r] = a->m[r] * in[0] + a->m[4 + r] * in[1] + a->m[8 + r] * in[2] + a->m[12 + r] * in[3];
}
/*General 4x4 inverse (for mouse picking).*/
static inline int m4invert(const m4 *a, m4 *out) {
    const float *m = a->m;
    float inv[16];
    inv[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
    inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    inv[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
    float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (fabsf(det) < 1e-12f) return 0;
    det = 1.0f / det;
    for (int i = 0; i < 16; i++) out->m[i] = inv[i] * det;
    return 1;
}

/*3x3 rotation helpers for model building (row-major 3x3 in float[9]).*/
typedef struct { float m[9]; } m3;
static inline m3 m3identity(void) { m3 r = {{1, 0, 0, 0, 1, 0, 0, 0, 1}}; return r; }
static inline m3 m3mul(m3 a, m3 b) {
    m3 r;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            r.m[i * 3 + j] = a.m[i * 3 + 0] * b.m[0 * 3 + j] + a.m[i * 3 + 1] * b.m[1 * 3 + j] + a.m[i * 3 + 2] * b.m[2 * 3 + j];
    return r;
}
static inline m3 m3rotx(float a) { float c = cosf(a), s = sinf(a); m3 r = {{1, 0, 0, 0, c, -s, 0, s, c}}; return r; }
static inline m3 m3roty(float a) { float c = cosf(a), s = sinf(a); m3 r = {{c, 0, s, 0, 1, 0, -s, 0, c}}; return r; }
static inline m3 m3rotz(float a) { float c = cosf(a), s = sinf(a); m3 r = {{c, -s, 0, s, c, 0, 0, 0, 1}}; return r; }
static inline v3 m3apply(m3 r, v3 v) {
    return v3m(r.m[0] * v.x + r.m[1] * v.y + r.m[2] * v.z,
               r.m[3] * v.x + r.m[4] * v.y + r.m[5] * v.z,
               r.m[6] * v.x + r.m[7] * v.y + r.m[8] * v.z);
}

#endif
