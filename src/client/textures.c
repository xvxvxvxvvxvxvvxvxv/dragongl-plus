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

/*Procedural material generator (see textures.h).
 *
 * All patterns are PERIODIC over the unit square so every layer tiles
 * seamlessly (GL_REPEAT). Building blocks: periodic value noise + fBm,
 * periodic Voronoi (F1/F2 + cell id), and a few hand-written layouts
 * (bricks, planks, flagstones). Colour textures are authored in their
 * final colours; "body" materials (skin, cloth, metal, scales...) are
 * near-white so the vertex colour can tint them per creature.*/

#include "textures.h"
#include "mathx.h"
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define TS MAT_TEX_SIZE

typedef struct { float r, g, b, a; } rgba;

static rgba C(float r, float g, float b) { rgba c = {r, g, b, 1.0f}; return c; }
static rgba mixc(rgba a, rgba b, float t) {
    rgba c = {lerpf(a.r, b.r, t), lerpf(a.g, b.g, t), lerpf(a.b, b.b, t), lerpf(a.a, b.a, t)};
    return c;
}
static rgba mulc(rgba a, float s) { rgba c = {a.r * s, a.g * s, a.b * s, a.a}; return c; }

/* ------------------------------ noise -------------------------------- */

static float lattice(int x, int y, int period, uint32_t seed) {
    x = ((x % period) + period) % period;
    y = ((y % period) + period) % period;
    return hash1f(hash2i(x, y) ^ seed);
}

static float fade(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }

/*Periodic value noise; (u,v) in [0,1), period = lattice cells.*/
static float vnoise(float u, float v, int period, uint32_t seed) {
    float x = u * (float)period, y = v * (float)period;
    int xi = (int)floorf(x), yi = (int)floorf(y);
    float fx = fade(x - (float)xi), fy = fade(y - (float)yi);
    float a = lattice(xi, yi, period, seed), b = lattice(xi + 1, yi, period, seed);
    float c = lattice(xi, yi + 1, period, seed), d = lattice(xi + 1, yi + 1, period, seed);
    return lerpf(lerpf(a, b, fx), lerpf(c, d, fx), fy);
}

static float fbm(float u, float v, int period, int oct, uint32_t seed) {
    float s = 0, amp = 0.5f, norm = 0;
    for (int o = 0; o < oct; o++) {
        s += vnoise(u, v, period, seed + (uint32_t)o * 977u) * amp;
        norm += amp;
        amp *= 0.5f;
        period *= 2;
    }
    return s / norm;
}

/*Periodic Voronoi: returns F1, F2 distances (in cell units) and the id
 * of the nearest cell.*/
static void voronoi(float u, float v, int cells, uint32_t seed, float *f1, float *f2, uint32_t *id) {
    float x = u * (float)cells, y = v * (float)cells;
    int xi = (int)floorf(x), yi = (int)floorf(y);
    float d1 = 1e9f, d2 = 1e9f;
    uint32_t best = 0;
    for (int j = -1; j <= 1; j++)
        for (int i = -1; i <= 1; i++) {
            int cx = xi + i, cy = yi + j;
            int wx = ((cx % cells) + cells) % cells, wy = ((cy % cells) + cells) % cells;
            uint32_t h = hash2i(wx, wy) ^ seed;
            float px = (float)cx + 0.15f + 0.7f * hash1f(h);
            float py = (float)cy + 0.15f + 0.7f * hash1f(h * 7u + 3u);
            float dx = px - x, dy = py - y;
            float d = sqrtf(dx * dx + dy * dy);
            if (d < d1) { d2 = d1; d1 = d; best = h; }
            else if (d < d2) d2 = d;
        }
    if (f1) *f1 = d1;
    if (f2) *f2 = d2;
    if (id) *id = best;
}

/* --------------------------- layer helpers ---------------------------- */

static uint8_t *g_buf;

static void put(int layer, int x, int y, rgba c) {
    uint8_t *p = g_buf + ((size_t)layer * TS * TS + (size_t)y * TS + (size_t)x) * 4;
    p[0] = (uint8_t)(clampf(c.r, 0, 1) * 255.0f + 0.5f);
    p[1] = (uint8_t)(clampf(c.g, 0, 1) * 255.0f + 0.5f);
    p[2] = (uint8_t)(clampf(c.b, 0, 1) * 255.0f + 0.5f);
    p[3] = (uint8_t)(clampf(c.a, 0, 1) * 255.0f + 0.5f);
}

#define FOR_PIXELS for (int y = 0; y < TS; y++) for (int x = 0; x < TS; x++)
#define UV float u = ((float)x + 0.5f) / TS, v = ((float)y + 0.5f) / TS; (void)u; (void)v

/*Rectangular block layout (bricks/flagstones): returns distance to the
 * nearest mortar line (in pixels-ish units of the block) and block id.*/
static float block_edge(float u, float v, int cols, int rows, bool stagger, uint32_t *bid) {
    float fy = v * (float)rows;
    int row = (int)floorf(fy);
    float off = (stagger && (row & 1)) ? 0.5f : 0.0f;
    float fx = u * (float)cols + off;
    int col = (int)floorf(fx);
    float lx = fx - (float)col, ly = fy - (float)row;
    int wc = ((col % cols) + cols) % cols;
    if (bid) *bid = hash2i(wc, row) ;
    float ex = fminf(lx, 1.0f - lx) / (float)cols * (float)TS;
    float ey = fminf(ly, 1.0f - ly) / (float)rows * (float)TS;
    return fminf(ex, ey);
}

/* ------------------------------ materials ----------------------------- */

static void gen_white(int L) { FOR_PIXELS put(L, x, y, C(1, 1, 1)); }

static void gen_flagstone(int L) {
    FOR_PIXELS {
        UV;
        uint32_t id;
        /*2x2 slabs per tile, alternate rows shifted by a quarter*/
        float e = block_edge(u, v, 2, 2, true, &id);
        float base = 0.30f + 0.08f * hash1f(id);
        float n = fbm(u, v, 8, 4, 11u);
        float grain = vnoise(u, v, 64, 5u);
        float bevel = smoothstepf(0.0f, 4.0f, e);
        float c = base * (0.75f + 0.45f * n) * (0.9f + 0.2f * grain);
        c *= 0.35f + 0.65f * bevel;
        /*hairline cracks*/
        float cr = fabsf(fbm(u + 0.37f, v, 6, 3, 99u) - 0.5f);
        if (cr < 0.012f) c *= 0.7f;
        rgba col = C(c * 1.02f, c * 0.98f, c * 0.93f);
        put(L, x, y, col);
    }
}

static void gen_brick(int L) {
    FOR_PIXELS {
        UV;
        uint32_t id;
        float e = block_edge(u, v, 2, 4, true, &id);
        float hv = hash1f(id);
        float n = fbm(u, v, 16, 3, 21u);
        float mortar = smoothstepf(1.0f, 3.0f, e);
        rgba stone = C(0.42f + 0.10f * hv, 0.40f + 0.08f * hv, 0.38f + 0.06f * hv);
        stone = mulc(stone, 0.75f + 0.5f * n);
        /*chipped corners: darken near the mortar edges*/
        stone = mulc(stone, 0.6f + 0.4f * smoothstepf(1.0f, 6.0f, e));
        rgba mort = C(0.16f, 0.15f, 0.14f);
        mort = mulc(mort, 0.8f + 0.4f * vnoise(u, v, 32, 7u));
        rgba c = mixc(mort, stone, mortar);
        /*moss/damp at random*/
        float moss = smoothstepf(0.62f, 0.75f, fbm(u, v, 4, 3, 300u));
        c = mixc(c, mulc(C(0.20f, 0.30f, 0.14f), 0.8f + 0.4f * n), moss * 0.55f);
        put(L, x, y, c);
    }
}

static void gen_wall_top(int L) {
    FOR_PIXELS {
        UV;
        float n = fbm(u, v, 8, 4, 31u);
        float c = 0.22f + 0.18f * n;
        put(L, x, y, C(c, c * 0.97f, c * 0.92f));
    }
}

static void gen_rock(int L) {
    FOR_PIXELS {
        UV;
        float f1, f2;
        voronoi(u, v, 5, 41u, &f1, &f2, NULL);
        float n = fbm(u, v, 8, 5, 42u);
        float ridge = smoothstepf(0.0f, 0.25f, f2 - f1);
        float c = (0.18f + 0.22f * n) * (0.55f + 0.45f * ridge);
        put(L, x, y, C(c * 1.0f, c * 0.95f, c * 0.9f));
    }
}

static void gen_cobble(int L) {
    FOR_PIXELS {
        UV;
        float f1, f2;
        uint32_t id;
        voronoi(u, v, 6, 51u, &f1, &f2, &id);
        float edge = f2 - f1;
        float hv = hash1f(id);
        float dome = 1.0f - clampf(f1 * 1.3f, 0, 1);
        float n = vnoise(u, v, 48, 52u);
        rgba stone = C(0.40f + 0.12f * hv, 0.38f + 0.10f * hv, 0.36f + 0.09f * hv);
        stone = mulc(stone, 0.65f + 0.45f * dome + 0.15f * n);
        rgba gap = C(0.12f, 0.11f, 0.09f);
        rgba c = mixc(gap, stone, smoothstepf(0.04f, 0.12f, edge));
        put(L, x, y, c);
    }
}

static void gen_grass(int L) {
    FOR_PIXELS {
        UV;
        float n = fbm(u, v, 8, 4, 61u);
        float blades = vnoise(u * 1.0f, v, 64, 62u);
        float streak = vnoise(u, v * 0.25f + u * 0.0f, 32, 63u);
        rgba c = mixc(C(0.13f, 0.30f, 0.08f), C(0.30f, 0.50f, 0.14f), n);
        c = mulc(c, 0.7f + 0.5f * blades * (0.6f + 0.4f * streak));
        float flower = hash2f(x / 3, y / 3) > 0.993f ? 1.0f : 0.0f;
        c = mixc(c, C(0.9f, 0.85f, 0.4f), flower * 0.7f);
        float dirt = smoothstepf(0.66f, 0.8f, fbm(u, v, 4, 3, 64u));
        c = mixc(c, C(0.30f, 0.23f, 0.14f), dirt * 0.6f);
        put(L, x, y, c);
    }
}

static void gen_plank(int L) {
    FOR_PIXELS {
        UV;
        int planks = 4;
        float fy = v * planks;
        int row = (int)floorf(fy);
        float ly = fy - row;
        float shift = hash1f((uint32_t)row * 31u + 7u);
        float uu = u + shift;
        float grain = sinf((uu * 24.0f + fbm(uu - floorf(uu), v, 4, 3, 71u + (uint32_t)row) * 6.0f) * MX_TAU * 0.5f);
        float hv = hash1f((uint32_t)row * 13u + 1u);
        rgba wood = C(0.45f + 0.08f * hv, 0.30f + 0.05f * hv, 0.17f + 0.03f * hv);
        wood = mulc(wood, 0.82f + 0.12f * grain + 0.12f * vnoise(u, v, 64, 72u));
        float gap = smoothstepf(0.0f, 0.04f, ly) * smoothstepf(0.0f, 0.04f, 1.0f - ly);
        /*butt joints*/
        float jx = fabsf((uu - floorf(uu)) - 0.5f);
        gap *= smoothstepf(0.0f, 0.01f, fabsf(jx - 0.49f) + 0.0f) * 0.4f + 0.6f;
        wood = mulc(wood, 0.35f + 0.65f * gap);
        /*nails*/
        float nx = (uu - floorf(uu)), ny = ly;
        float d1 = hypotf((nx - 0.06f) * 4.0f, (ny - 0.5f) * 1.0f);
        if (d1 < 0.06f) wood = C(0.15f, 0.14f, 0.13f);
        put(L, x, y, wood);
    }
}

static void gen_water(int L) {
    FOR_PIXELS {
        UV;
        float n1 = fbm(u, v, 4, 4, 81u);
        float c1 = fabsf(fbm(u + 0.3f, v, 6, 3, 82u) - 0.5f);
        float caust = smoothstepf(0.06f, 0.0f, c1);
        rgba c = mixc(C(0.04f, 0.16f, 0.30f), C(0.10f, 0.38f, 0.55f), n1);
        c = mixc(c, C(0.55f, 0.80f, 0.90f), caust * 0.6f);
        put(L, x, y, c);
    }
}

static void gen_lava(int L) {
    FOR_PIXELS {
        UV;
        float f1, f2;
        voronoi(u, v, 5, 91u, &f1, &f2, NULL);
        float edge = f2 - f1;
        float n = fbm(u, v, 8, 4, 92u);
        float hot = smoothstepf(0.25f, 0.0f, edge) * (0.6f + 0.4f * n);
        rgba crust = mulc(C(0.18f, 0.06f, 0.03f), 0.6f + 0.6f * n);
        rgba glow = mixc(C(1.0f, 0.35f, 0.02f), C(1.0f, 0.85f, 0.3f), hot);
        put(L, x, y, mixc(crust, glow, clampf(hot * 1.4f, 0, 1)));
    }
}

static void gen_ice(int L) {
    FOR_PIXELS {
        UV;
        float f1, f2;
        voronoi(u, v, 4, 101u, &f1, &f2, NULL);
        float n = fbm(u, v, 8, 4, 102u);
        float crack = smoothstepf(0.04f, 0.0f, f2 - f1);
        rgba c = mixc(C(0.62f, 0.80f, 0.92f), C(0.85f, 0.95f, 1.0f), n);
        c = mixc(c, C(0.95f, 1.0f, 1.0f), crack * 0.8f);
        put(L, x, y, c);
    }
}

static void gen_sand(int L) {
    FOR_PIXELS {
        UV;
        float n = fbm(u, v, 8, 4, 111u);
        float rip = 0.5f + 0.5f * sinf((v * 10.0f + fbm(u, v, 4, 2, 112u) * 3.0f) * MX_TAU);
        float speck = hash2f(x, y + 1000);
        rgba c = C(0.76f, 0.64f, 0.42f);
        c = mulc(c, 0.82f + 0.15f * n + 0.08f * rip + (speck > 0.97f ? -0.2f : 0.0f));
        put(L, x, y, c);
    }
}

static void gen_ash(int L) {
    FOR_PIXELS {
        UV;
        float n = fbm(u, v, 8, 5, 121u);
        float speck = hash2f(x + 77, y);
        float c = 0.20f + 0.14f * n;
        rgba col = C(c, c, c * 1.02f);
        if (speck > 0.985f) col = C(0.9f, 0.4f, 0.1f); /*embers*/
        put(L, x, y, col);
    }
}

static void gen_mud(int L) {
    FOR_PIXELS {
        UV;
        float n = fbm(u, v, 6, 5, 131u);
        float wet = smoothstepf(0.55f, 0.7f, fbm(u, v, 4, 3, 132u));
        rgba c = mulc(C(0.28f, 0.19f, 0.11f), 0.7f + 0.5f * n);
        c = mixc(c, C(0.18f, 0.14f, 0.10f), wet * 0.7f);
        put(L, x, y, c);
    }
}

static void gen_marble(int L) {
    FOR_PIXELS {
        UV;
        uint32_t id;
        float e = block_edge(u, v, 2, 2, false, &id);
        float t = fbm(u, v, 4, 5, 141u + (id & 3u));
        float vein = fabsf(sinf((u + v + t * 2.5f) * MX_TAU * 2.0f));
        vein = powf(vein, 12.0f);
        rgba c = C(0.88f, 0.87f, 0.84f);
        c = mixc(c, C(0.45f, 0.45f, 0.50f), vein * 0.6f);
        c = mulc(c, 0.9f + 0.1f * hash1f(id));
        c = mulc(c, 0.55f + 0.45f * smoothstepf(0.0f, 2.0f, e));
        put(L, x, y, c);
    }
}

static void gen_obsidian(int L) {
    FOR_PIXELS {
        UV;
        float f1, f2;
        voronoi(u, v, 4, 151u, &f1, &f2, NULL);
        float n = fbm(u, v, 8, 4, 152u);
        float sheen = smoothstepf(0.5f, 0.0f, f1) * 0.3f;
        rgba c = C(0.06f + sheen * 0.6f, 0.04f + sheen * 0.4f, 0.10f + sheen);
        c = mulc(c, 0.8f + 0.4f * n);
        put(L, x, y, c);
    }
}

static void gen_gold_vein(int L) {
    FOR_PIXELS {
        UV;
        float n = fbm(u, v, 8, 5, 161u);
        float vein = fabsf(fbm(u, v, 4, 4, 162u) - 0.5f);
        float g = smoothstepf(0.05f, 0.0f, vein);
        float fleck = hash2f(x * 3, y * 5) > 0.97f ? 1.0f : 0.0f;
        rgba rock = mulc(C(0.28f, 0.25f, 0.22f), 0.7f + 0.5f * n);
        rgba gold = mixc(C(0.85f, 0.65f, 0.15f), C(1.0f, 0.9f, 0.45f), n);
        put(L, x, y, mixc(rock, gold, clampf(g + fleck * 0.8f, 0, 1)));
    }
}

static void gen_crystal(int L) {
    FOR_PIXELS {
        UV;
        float f1, f2;
        voronoi(u, v, 3, 171u, &f1, &f2, NULL);
        float facet = 0.6f + 0.4f * smoothstepf(0.0f, 0.4f, f2 - f1);
        float sparkle = hash2f(x, y) > 0.992f ? 0.4f : 0.0f;
        float c = 0.7f * facet + 0.3f * fbm(u, v, 8, 3, 172u) + sparkle;
        put(L, x, y, C(c, c, c));
    }
}

static void gen_mushroom(int L) {
    FOR_PIXELS {
        UV;
        float f1;
        voronoi(u, v, 4, 181u, &f1, NULL, NULL);
        float spot = smoothstepf(0.28f, 0.2f, f1);
        float n = fbm(u, v, 8, 3, 182u);
        float c = 0.7f + 0.2f * n;
        rgba col = mixc(C(c, c, c), C(1.0f, 1.0f, 0.95f), spot);
        col.a = 1.0f;
        put(L, x, y, col);
    }
}

static void gen_door(int L) {
    FOR_PIXELS {
        UV;
        int planks = 4;
        float fx = u * planks;
        int col = (int)floorf(fx);
        float lx = fx - col;
        float hv = hash1f((uint32_t)col * 17u + 3u);
        float grain = sinf((v * 18.0f + fbm(u, v, 4, 3, 191u + (uint32_t)col) * 5.0f) * MX_TAU * 0.5f);
        rgba wood = C(0.38f + 0.06f * hv, 0.24f + 0.04f * hv, 0.13f);
        wood = mulc(wood, 0.8f + 0.12f * grain + 0.1f * vnoise(u, v, 64, 192u));
        wood = mulc(wood, 0.45f + 0.55f * smoothstepf(0.0f, 0.06f, fminf(lx, 1.0f - lx)));
        /*iron bands*/
        float band = fminf(fabsf(v - 0.2f), fabsf(v - 0.8f));
        if (band < 0.05f) {
            float m = 0.25f + 0.1f * vnoise(u, v, 32, 193u);
            wood = C(m, m, m * 1.05f);
            float rivet = hypotf((lx - 0.5f) * 0.25f, (band) * 1.0f);
            if (fabsf(v - 0.2f) < 0.05f || fabsf(v - 0.8f) < 0.05f)
                if (rivet < 0.02f) wood = C(0.55f, 0.55f, 0.58f);
        }
        put(L, x, y, wood);
    }
}

static void gen_iron(int L) {
    FOR_PIXELS {
        UV;
        float brushed = vnoise(u * 1.0f, v, 128, 201u) * 0.5f + vnoise(u, v, 16, 202u) * 0.5f;
        float c = 0.45f + 0.2f * brushed;
        float rust = smoothstepf(0.6f, 0.75f, fbm(u, v, 4, 4, 203u));
        rgba col = mixc(C(c, c, c * 1.04f), C(0.45f, 0.22f, 0.10f), rust * 0.7f);
        put(L, x, y, col);
    }
}

static void gen_trap(int L) {
    FOR_PIXELS {
        UV;
        float n = fbm(u, v, 8, 3, 211u);
        float c = 0.35f + 0.1f * n;
        rgba col = C(c, c * 0.95f, c * 0.9f);
        float gx = fmodf(u * 4.0f, 1.0f) - 0.5f, gy = fmodf(v * 4.0f, 1.0f) - 0.5f;
        if (hypotf(gx, gy) < 0.13f) col = C(0.05f, 0.04f, 0.04f);
        float border = fminf(fminf(u, 1 - u), fminf(v, 1 - v));
        if (border < 0.04f) col = C(0.25f, 0.2f, 0.15f);
        put(L, x, y, col);
    }
}

static void gen_stairs(int L) {
    FOR_PIXELS {
        UV;
        int steps = 4;
        float fy = v * steps;
        float ly = fy - floorf(fy);
        float n = fbm(u, v, 8, 3, 221u);
        float c = (0.40f + 0.12f * n) * (0.55f + 0.45f * ly);
        if (ly > 0.92f) c *= 1.35f; /*worn nosing highlight*/
        put(L, x, y, C(c, c * 0.97f, c * 0.93f));
    }
}

static void gen_skin(int L) {
    FOR_PIXELS {
        UV;
        float n = fbm(u, v, 8, 3, 231u);
        float c = 0.88f + 0.12f * n;
        put(L, x, y, C(c, c, c));
    }
}

static void gen_cloth(int L) {
    FOR_PIXELS {
        UV;
        float wx = 0.5f + 0.5f * sinf(u * TS * 0.8f * MX_PI);
        float wy = 0.5f + 0.5f * sinf(v * TS * 0.8f * MX_PI);
        float weave = ((x / 2 + y / 2) & 1) ? wx : wy;
        float n = fbm(u, v, 8, 3, 241u);
        float c = 0.72f + 0.12f * weave + 0.16f * n;
        put(L, x, y, C(c, c, c));
    }
}

static void gen_leather(int L) {
    FOR_PIXELS {
        UV;
        float f1, f2;
        voronoi(u, v, 14, 251u, &f1, &f2, NULL);
        float wr = smoothstepf(0.0f, 0.15f, f2 - f1);
        float n = fbm(u, v, 8, 3, 252u);
        float c = 0.62f + 0.18f * wr + 0.2f * n;
        put(L, x, y, C(c, c * 0.95f, c * 0.9f));
    }
}

static void gen_metal(int L) {
    FOR_PIXELS {
        UV;
        float br = vnoise(u, v, 128, 261u);
        float n = fbm(u, v, 8, 3, 262u);
        float c = 0.68f + 0.18f * br + 0.14f * n;
        float plate = fminf(fmodf(v * 3.0f, 1.0f), 1.0f - fmodf(v * 3.0f, 1.0f));
        if (plate < 0.04f) c *= 0.55f;
        put(L, x, y, C(c, c, c * 1.03f));
    }
}

static void gen_bone(int L) {
    FOR_PIXELS {
        UV;
        float n = fbm(u, v, 8, 4, 271u);
        float pores = hash2f(x, y) > 0.985f ? 0.75f : 1.0f;
        float c = (0.82f + 0.18f * n) * pores;
        put(L, x, y, C(c, c * 0.97f, c * 0.88f));
    }
}

static void gen_scales(int L) {
    FOR_PIXELS {
        UV;
        int rows = 8;
        float fy = v * rows;
        int row = (int)floorf(fy);
        float off = (row & 1) ? 0.5f : 0.0f;
        float fx = u * 8 + off;
        float lx = fx - floorf(fx) - 0.5f, ly = fy - row;
        float d = hypotf(lx, ly * 0.9f);
        float scale = smoothstepf(0.55f, 0.35f, d);
        float c = 0.55f + 0.4f * scale * (1.0f - ly * 0.5f);
        put(L, x, y, C(c, c, c));
    }
}

static void gen_fur(int L) {
    FOR_PIXELS {
        UV;
        float s = vnoise(u * 1.0f, v, 96, 281u) * 0.6f + vnoise(u, v, 12, 282u) * 0.4f;
        float strand = vnoise(u + v * 0.15f, v * 0.2f, 64, 283u);
        float c = 0.55f + 0.3f * s + 0.15f * strand;
        put(L, x, y, C(c, c, c));
    }
}

static void gen_slime(int L) {
    FOR_PIXELS {
        UV;
        float f1;
        voronoi(u, v, 6, 291u, &f1, NULL, NULL);
        float bubble = smoothstepf(0.25f, 0.18f, f1);
        float n = fbm(u, v, 4, 3, 292u);
        float c = 0.65f + 0.25f * n + 0.35f * bubble;
        put(L, x, y, C(c, c, c));
    }
}

static void gen_bark(int L) {
    FOR_PIXELS {
        UV;
        float ridges = fabsf(sinf((u * 10.0f + fbm(u, v, 4, 4, 301u) * 2.0f) * MX_PI));
        float c = 0.35f + 0.35f * ridges + 0.1f * vnoise(u, v, 32, 302u);
        put(L, x, y, C(c * 0.9f, c * 0.75f, c * 0.55f));
    }
}

static void gen_leaf(int L) {
    FOR_PIXELS {
        UV;
        float f1, f2;
        voronoi(u, v, 7, 311u, &f1, &f2, NULL);
        float leaf = smoothstepf(0.0f, 0.2f, f2 - f1);
        float n = fbm(u, v, 8, 3, 312u);
        float c = 0.45f + 0.35f * leaf + 0.2f * n;
        put(L, x, y, C(c * 0.75f, c, c * 0.6f));
    }
}

static void gen_glow(int L) {
    FOR_PIXELS {
        UV;
        float d = hypotf(u - 0.5f, v - 0.5f) * 2.0f;
        float a = expf(-d * d * 4.0f) * smoothstepf(1.0f, 0.8f, d);
        rgba c = {1, 1, 1, a};
        put(L, x, y, c);
    }
}

static void gen_spark(int L) {
    FOR_PIXELS {
        UV;
        float dx = fabsf(u - 0.5f) * 2.0f, dy = fabsf(v - 0.5f) * 2.0f;
        float star = fmaxf(expf(-dx * 14.0f) * expf(-dy * 2.5f), expf(-dy * 14.0f) * expf(-dx * 2.5f));
        float core = expf(-(dx * dx + dy * dy) * 10.0f);
        float a = clampf(star + core, 0, 1);
        rgba c = {1, 1, 1, a};
        put(L, x, y, c);
    }
}

static void gen_rune(int L) {
    FOR_PIXELS {
        UV;
        float dx = u - 0.5f, dy = v - 0.5f;
        float r = hypotf(dx, dy) * 2.0f;
        float ang = atan2f(dy, dx);
        float a = 0;
        a += smoothstepf(0.03f, 0.0f, fabsf(r - 0.92f));
        a += smoothstepf(0.02f, 0.0f, fabsf(r - 0.80f));
        a += smoothstepf(0.02f, 0.0f, fabsf(r - 0.45f));
        /*runic glyph band between the two outer rings*/
        if (r > 0.82f && r < 0.90f) {
            int seg = (int)floorf((ang + MX_PI) / MX_TAU * 24.0f);
            float local = (ang + MX_PI) / MX_TAU * 24.0f - (float)seg;
            uint32_t h = hash_u32((uint32_t)seg * 977u);
            if ((h & 1u) && local > 0.2f && local < 0.8f) a += 0.8f;
            if ((h & 2u) && fabsf(r - 0.86f) < 0.012f) a += 0.8f;
        }
        /*hexagram*/
        for (int k = 0; k < 6; k++) {
            float t = (float)k / 6.0f * MX_TAU;
            float nx = cosf(t), ny = sinf(t);
            float dline = fabsf(dx * 2.0f * nx + dy * 2.0f * ny - 0.4f);
            if (r < 0.80f) a += smoothstepf(0.02f, 0.0f, dline) * 0.8f;
        }
        rgba c = {1, 1, 1, clampf(a, 0, 1)};
        put(L, x, y, c);
    }
}

static void gen_shadow(int L) {
    FOR_PIXELS {
        UV;
        float d = hypotf(u - 0.5f, v - 0.5f) * 2.0f;
        float a = smoothstepf(1.0f, 0.2f, d) * 0.85f;
        rgba c = {0, 0, 0, a};
        put(L, x, y, c);
    }
}

static void gen_dirt(int L) {
    FOR_PIXELS {
        UV;
        float n = fbm(u, v, 8, 5, 331u);
        float pebble = hash2f(x / 2, y / 2) > 0.97f ? 0.2f : 0.0f;
        rgba c = mulc(C(0.33f, 0.25f, 0.17f), 0.7f + 0.5f * n + pebble);
        put(L, x, y, c);
    }
}

static void gen_roof(int L) {
    FOR_PIXELS {
        UV;
        uint32_t id;
        float e = block_edge(u, v, 4, 6, true, &id);
        float hv = hash1f(id);
        float fy = v * 6.0f;
        float ly = fy - floorf(fy);
        rgba c = C(0.45f + 0.1f * hv, 0.18f + 0.05f * hv, 0.12f);
        c = mulc(c, 0.55f + 0.45f * ly);
        c = mulc(c, 0.6f + 0.4f * smoothstepf(0.0f, 2.0f, e));
        put(L, x, y, c);
    }
}

static void gen_gold(int L) {
    FOR_PIXELS {
        UV;
        float f1, f2;
        voronoi(u, v, 8, 341u, &f1, &f2, NULL);
        float coin = smoothstepf(0.0f, 0.15f, f2 - f1);
        float shine = 1.0f - clampf(f1 * 1.6f, 0, 1);
        rgba c = mixc(C(0.55f, 0.38f, 0.08f), C(1.0f, 0.86f, 0.35f), coin * (0.5f + 0.5f * shine));
        put(L, x, y, c);
    }
}

static void gen_paper(int L) {
    FOR_PIXELS {
        UV;
        float n = fbm(u, v, 8, 4, 351u);
        float line = (fmodf(v * 12.0f, 1.0f) < 0.08f && u > 0.1f && u < 0.9f) ? 0.75f : 1.0f;
        float c = (0.80f + 0.15f * n) * line;
        put(L, x, y, C(c, c * 0.93f, c * 0.78f));
    }
}

static void gen_flame(int L) {
    FOR_PIXELS {
        UV;
        float dx = (u - 0.5f) * 2.0f;
        float h = 1.0f - v; /*0 bottom .. 1 top*/
        float width = (1.0f - h) * 0.9f * (0.8f + 0.2f * vnoise(u, v, 8, 361u));
        float core = clampf(1.0f - fabsf(dx) / fmaxf(width, 0.01f), 0, 1);
        float a = core * smoothstepf(1.0f, 0.6f, h) * smoothstepf(0.0f, 0.15f, h);
        rgba c = {1, 1, 1, clampf(a * 1.3f, 0, 1)};
        put(L, x, y, c);
    }
}

uint8_t *textures_generate(void) {
    g_buf = malloc((size_t)MAT_COUNT * TS * TS * 4);
    if (!g_buf) return NULL;
    void (*gens[MAT_COUNT])(int) = {
        [MAT_WHITE] = gen_white,         [MAT_FLAGSTONE] = gen_flagstone,
        [MAT_BRICK] = gen_brick,         [MAT_WALL_TOP] = gen_wall_top,
        [MAT_ROCK] = gen_rock,           [MAT_COBBLE] = gen_cobble,
        [MAT_GRASS] = gen_grass,         [MAT_PLANK] = gen_plank,
        [MAT_WATER] = gen_water,         [MAT_LAVA] = gen_lava,
        [MAT_ICE] = gen_ice,             [MAT_SAND] = gen_sand,
        [MAT_ASH] = gen_ash,             [MAT_MUD] = gen_mud,
        [MAT_MARBLE] = gen_marble,       [MAT_OBSIDIAN] = gen_obsidian,
        [MAT_GOLD_VEIN] = gen_gold_vein, [MAT_CRYSTAL] = gen_crystal,
        [MAT_MUSHROOM] = gen_mushroom,   [MAT_DOOR] = gen_door,
        [MAT_IRON] = gen_iron,           [MAT_TRAP] = gen_trap,
        [MAT_STAIRS] = gen_stairs,       [MAT_SKIN] = gen_skin,
        [MAT_CLOTH] = gen_cloth,         [MAT_LEATHER] = gen_leather,
        [MAT_METAL] = gen_metal,         [MAT_BONE] = gen_bone,
        [MAT_SCALES] = gen_scales,       [MAT_FUR] = gen_fur,
        [MAT_SLIME] = gen_slime,         [MAT_BARK] = gen_bark,
        [MAT_LEAF] = gen_leaf,           [MAT_GLOW] = gen_glow,
        [MAT_SPARK] = gen_spark,         [MAT_RUNE] = gen_rune,
        [MAT_SHADOW] = gen_shadow,       [MAT_DIRT] = gen_dirt,
        [MAT_ROOF] = gen_roof,           [MAT_GOLD] = gen_gold,
        [MAT_PAPER] = gen_paper,         [MAT_FLAME] = gen_flame,
    };
    for (int i = 0; i < MAT_COUNT; i++) {
        if (gens[i]) gens[i](i);
        else gen_white(i);
    }
    return g_buf;
}
