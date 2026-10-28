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

/*World mesh: the voxel map turned into textured, ambient-occluded,
 * light-emitting geometry.
 *
 * The 300x300 map is split in 16x16 chunks. A chunk is (re)built only
 * when one of its tiles (or a neighbour's) changed; the chunks around
 * the camera are concatenated into the static world buffer whose
 * version tells the backend when to re-upload it. Each chunk also
 * records its light sources (lava, crystals, mushrooms, torches,
 * stairs) and particle emitters, gathered every frame near the player.*/

#include "scene_internal.h"
#include "textures.h"
#include <stdlib.h>
#include <string.h>

#define CH 16
#define NCX ((MAP_WIDTH + CH - 1) / CH)
#define NCY ((MAP_HEIGHT + CH - 1) / CH)
#define WALL_H 1.5f
#define ROCK_H 1.9f
#define MAX_CHUNK_LIGHTS 48
#define MAX_CHUNK_EMIT 96

typedef struct {
    Mesh opaque;
    Mesh trans;
    WorldEmitter emit[MAX_CHUNK_EMIT];
    int nemit;
    GfxLight lights[MAX_CHUNK_LIGHTS];
    int nlights;
    bool built;
    bool dirty;
    bool empty; /*no visible tile at all*/
} Chunk;

static Chunk g_chunks[NCY][NCX];
static TileType g_built_map[MAP_HEIGHT][MAP_WIDTH];
static uint32_t g_built_version;
static bool g_town;
static Mesh g_world;
static uint32_t g_world_version = 1;
static int g_vis_cx0 = -1, g_vis_cy0, g_vis_cx1, g_vis_cy1;

static const TileType (*M)[MAP_WIDTH];

/*Village shop doors: the server turns them into walls at night (shops
 * closed). Same placement rule as the server (update_city_doors / map.c):
 * the client draws a CLOSED door there instead of a block of wall.*/
static uint8_t g_shop_door[MAP_HEIGHT][MAP_WIDTH];
static bool g_shop_doors_init;

static void init_shop_doors(void) {
    if (g_shop_doors_init) return;
    g_shop_doors_init = true;
    int cx = MAP_CENTER_X, cy = MAP_CENTER_Y;
    for (int i = 0; i < 11; i++) {
        float angle = ((float)i * (360.0f / 11.0f)) * (3.14159265358979f / 180.0f);
        int sx = cx + (int)(cosf(angle) * 26.0f), sy = cy + (int)(sinf(angle) * 26.0f);
        int dx = cx - sx, dy = cy - sy, tx = sx, ty = sy;
        if (abs(dx) > abs(dy)) tx = dx > 0 ? sx + 4 : sx - 4;
        else ty = dy > 0 ? sy + 4 : sy - 4;
        if (tx >= 0 && ty >= 0 && tx < MAP_WIDTH && ty < MAP_HEIGHT) g_shop_door[ty][tx] = 1;
    }
}

static inline bool closed_shop_door(int x, int y) {
    return g_town && g_shop_door[y][x] && M[y][x] == VOXEL_WALL;
}

static inline TileType T(int x, int y) {
    if (x < 0 || y < 0 || x >= MAP_WIDTH || y >= MAP_HEIGHT) return VOXEL_ROCK;
    if (closed_shop_door(x, y)) return VOXEL_DOOR; /*neighbours keep their faces*/
    return M[y][x];
}

bool tile_is_block(TileType t) {
    return t == VOXEL_WALL || t == VOXEL_OBSIDIAN || t == VOXEL_GOLD_VEIN || t == VOXEL_ROCK;
}

bool tile_walkable(TileType t) {
    return !tile_is_block(t);
}

static bool is_crystal(TileType t) {
    return t == VOXEL_CRYSTAL_BLUE || t == VOXEL_CRYSTAL_PURPLE || t == VOXEL_CRYSTAL_RED ||
           t == VOXEL_CRYSTAL_GREEN || t == VOXEL_CRYSTAL_YELLOW || t == VOXEL_CRYSTAL_ORANGE ||
           t == VOXEL_CRYSTAL_CYAN || t == VOXEL_CRYSTAL_WHITE;
}

uint32_t crystal_color(TileType t) {
    switch (t) {
    case VOXEL_CRYSTAL_BLUE: return RGBA8(80, 150, 255, 255);
    case VOXEL_CRYSTAL_PURPLE: return RGBA8(190, 80, 255, 255);
    case VOXEL_CRYSTAL_RED: return RGBA8(255, 60, 60, 255);
    case VOXEL_CRYSTAL_GREEN: return RGBA8(60, 255, 110, 255);
    case VOXEL_CRYSTAL_YELLOW: return RGBA8(255, 230, 70, 255);
    case VOXEL_CRYSTAL_ORANGE: return RGBA8(255, 140, 30, 255);
    case VOXEL_CRYSTAL_CYAN: return RGBA8(40, 240, 255, 255);
    case VOXEL_CRYSTAL_WHITE: return RGBA8(235, 240, 255, 255);
    default: return RGBA8(255, 255, 255, 255);
    }
}

/*Visible, non-rock, so geometry should exist around it.*/
static inline bool open_tile(TileType t) { return !tile_is_block(t); }

static int floor_material(TileType t) {
    switch (t) {
    case VOXEL_GRASS: return MAT_GRASS;
    case VOXEL_WOOD: return MAT_PLANK;
    case VOXEL_COBBLE: return MAT_COBBLE;
    case VOXEL_ICE: return MAT_ICE;
    case VOXEL_SAND: return MAT_SAND;
    case VOXEL_ASH: return MAT_ASH;
    case VOXEL_MUD: return MAT_MUD;
    case VOXEL_MARBLE: return MAT_MARBLE;
    case VOXEL_TRAP: return MAT_TRAP;
    case VOXEL_DOOR: return MAT_PLANK;
    case VOXEL_MUSHROOM_GLOW: return MAT_DIRT;
    case VOXEL_STAIRS_UP: return MAT_FLAGSTONE;
    default:
        if (is_crystal(t)) return MAT_ROCK;
        return MAT_FLAGSTONE;
    }
}

static void add_light(Chunk *c, float x, float y, float z, float r, float g, float b, float radius, float intensity) {
    if (c->nlights >= MAX_CHUNK_LIGHTS) return;
    GfxLight *l = &c->lights[c->nlights++];
    l->pos[0] = x; l->pos[1] = y; l->pos[2] = z; l->pos[3] = radius;
    l->col[0] = r; l->col[1] = g; l->col[2] = b; l->col[3] = intensity;
}

static void add_emit(Chunk *c, int type, float x, float y, float z, uint32_t col, float phase) {
    if (c->nemit >= MAX_CHUNK_EMIT) return;
    WorldEmitter *e = &c->emit[c->nemit++];
    e->type = type;
    e->pos = v3m(x, y, z);
    e->col = col;
    e->phase = phase;
}

/*Ambient occlusion factor of a floor corner: s1, s2 = the two side
 * neighbours, d = the diagonal one (classic voxel AO).*/
static float corner_ao(bool s1, bool s2, bool d) {
    int occ = (s1 && s2) ? 3 : (int)s1 + (int)s2 + (int)d;
    static const float k[4] = {1.0f, 0.80f, 0.66f, 0.52f};
    return k[occ];
}

static void build_floor(Mesh *m, int x, int y, TileType t, float yh, int mat) {
    float h = hash2f(x, y);
    uint32_t base = RGBA8(255, 255, 255, 255);
    float var = 0.92f + 0.10f * h;
    /*solid neighbours*/
    bool n = tile_is_block(T(x, y - 1)), s = tile_is_block(T(x, y + 1));
    bool w = tile_is_block(T(x - 1, y)), e = tile_is_block(T(x + 1, y));
    bool nw = tile_is_block(T(x - 1, y - 1)), ne = tile_is_block(T(x + 1, y - 1));
    bool sw = tile_is_block(T(x - 1, y + 1)), se = tile_is_block(T(x + 1, y + 1));
    float x0 = x - 0.5f, x1 = x + 0.5f, z0 = y - 0.5f, z1 = y + 0.5f;
    uint32_t cols[4] = {
        col_scale(base, var * corner_ao(w, s, sw)),
        col_scale(base, var * corner_ao(e, s, se)),
        col_scale(base, var * corner_ao(e, n, ne)),
        col_scale(base, var * corner_ao(w, n, nw)),
    };
    Mat mt = matc(mat, base);
    /*Texture coordinates in world space: neighbouring tiles continue the
     * pattern seamlessly (one texture per tile).*/
    float u0 = (float)x, v0 = (float)y;
    (void)t;
    mesh_quad_c(m, v3m(x0, yh, z1), v3m(x1, yh, z1), v3m(x1, yh, z0), v3m(x0, yh, z0), v3m(0, 1, 0), &mt,
                u0, v0 + 1, u0 + 1, v0, cols, NULL);
}

/*Vertical face along one tile edge, from y0 to y1, facing direction d.*/
static void build_side(Mesh *m, int x, int y, int dir, float y0, float y1, int mat, uint32_t col, bool cut_top) {
    /*dir: 0 = -z (north), 1 = +x (east), 2 = +z (south), 3 = -x (west); the
     * face sits on that edge of tile (x,y) and faces OUTWARD from it.*/
    float x0 = x - 0.5f, x1 = x + 0.5f, z0 = y - 0.5f, z1 = y + 0.5f;
    v3 a, b, n;
    switch (dir) {
    case 0: a = v3m(x1, 0, z0); b = v3m(x0, 0, z0); n = v3m(0, 0, -1); break;
    case 1: a = v3m(x1, 0, z1); b = v3m(x1, 0, z0); n = v3m(1, 0, 0); break;
    case 2: a = v3m(x0, 0, z1); b = v3m(x1, 0, z1); n = v3m(0, 0, 1); break;
    default: a = v3m(x0, 0, z0); b = v3m(x0, 0, z1); n = v3m(-1, 0, 0); break;
    }
    v3 p0 = v3m(a.x, y0, a.z), p1 = v3m(b.x, y0, b.z), p2 = v3m(b.x, y1, b.z), p3 = v3m(a.x, y1, a.z);
    Mat mt = matc(mat, col);
    uint32_t dark = col_scale(col, 0.62f);
    uint32_t cols[4] = {dark, dark, col, col};
    float cut[4] = {0, 0, cut_top ? 1.0f : 0.0f, cut_top ? 1.0f : 0.0f};
    float u = (dir == 0 || dir == 2) ? (float)x : (float)y;
    mesh_quad_c(m, p0, p1, p2, p3, n, &mt, u, y0, u + 1.0f, y1, cols, cut);
}

static const int DX[4] = {0, 1, 0, -1}, DY[4] = {-1, 0, 1, 0};

static void build_block(Chunk *c, int x, int y, TileType t) {
    /*Only blocks that touch an open tile are drawn at all.*/
    bool exposed[4];
    bool any = false;
    for (int d = 0; d < 4; d++) {
        exposed[d] = open_tile(T(x + DX[d], y + DY[d]));
        any |= exposed[d];
    }
    bool diag = open_tile(T(x - 1, y - 1)) || open_tile(T(x + 1, y - 1)) ||
                open_tile(T(x - 1, y + 1)) || open_tile(T(x + 1, y + 1));
    float h = (t == VOXEL_ROCK) ? ROCK_H : WALL_H;
    if (!any && !diag) {
        /*Interior of a solid mass: only its top, so large rock bodies
         * read as a continuous plateau instead of black holes.*/
        Mat top = matc(t == VOXEL_ROCK ? MAT_ROCK : MAT_WALL_TOP, RGBA8(150, 142, 135, 255));
        float x0 = x - 0.5f, x1 = x + 0.5f, z0 = y - 0.5f, z1 = y + 0.5f;
        mesh_quad(&c->opaque, v3m(x0, h, z1), v3m(x1, h, z1), v3m(x1, h, z0), v3m(x0, h, z0), v3m(0, 1, 0), &top,
                  (float)x, (float)y + 1, (float)x + 1, (float)y);
        return;
    }
    if (g_town && t == VOXEL_WALL) h = 1.9f;
    int mat = MAT_BRICK;
    uint32_t col = RGBA8(255, 255, 255, 255);
    if (t == VOXEL_ROCK) { mat = MAT_ROCK; col = RGBA8(200, 190, 180, 255); }
    else if (t == VOXEL_OBSIDIAN) mat = MAT_OBSIDIAN;
    else if (t == VOXEL_GOLD_VEIN) mat = MAT_GOLD_VEIN;
    /*per-block tint variation*/
    col = col_scale(col, 0.90f + 0.12f * hash2f(x * 3, y * 7));
    for (int d = 0; d < 4; d++)
        if (exposed[d]) build_side(&c->opaque, x, y, d, 0.0f, h, mat, col, true);
    /*top cap*/
    Mat top = matc(t == VOXEL_ROCK ? MAT_ROCK : (t == VOXEL_GOLD_VEIN ? MAT_GOLD_VEIN : MAT_WALL_TOP),
                   col_scale(col, 0.85f));
    float x0 = x - 0.5f, x1 = x + 0.5f, z0 = y - 0.5f, z1 = y + 0.5f;
    float cut[4] = {1, 1, 1, 1};
    uint32_t tc = col_scale(col, 0.85f);
    uint32_t cols[4] = {tc, tc, tc, tc};
    mesh_quad_c(&c->opaque, v3m(x0, h, z1), v3m(x1, h, z1), v3m(x1, h, z0), v3m(x0, h, z0), v3m(0, 1, 0), &top,
                (float)x, (float)y + 1, (float)x + 1, (float)y, cols, cut);

    /*Wall torches: some exposed wall faces in the dungeon get a torch.*/
    if (t == VOXEL_WALL && !g_town) {
        for (int d = 0; d < 4; d++) {
            if (!exposed[d]) continue;
            if (hash2i(x * 5 + d, y * 11 + 3) % 1000 >= 45) continue;
            float px = (float)x + DX[d] * 0.62f, pz = (float)y + DY[d] * 0.62f;
            Mat iron = matc(MAT_IRON, RGBA8(120, 110, 100, 255));
            mesh_box(&c->opaque, v3m(px, 0.95f, pz), v3m(0.05f, 0.18f, 0.05f), m3rotx(DY[d] * 0.4f), &iron);
            Mat wood = matc(MAT_PLANK, RGBA8(140, 100, 60, 255));
            mesh_box(&c->opaque, v3m(px + DX[d] * 0.06f, 1.12f, pz + DY[d] * 0.06f), v3m(0.045f, 0.10f, 0.045f), m3identity(), &wood);
            add_emit(c, EMIT_TORCH, px + DX[d] * 0.06f, 1.30f, pz + DY[d] * 0.06f, RGBA8(255, 170, 60, 255), hash2f(x, y) * 10);
            add_light(c, px + DX[d] * 0.3f, 1.35f, pz + DY[d] * 0.3f, 1.0f, 0.62f, 0.28f, 6.5f, 1.25f);
            break;
        }
    }
    /*gold veins glitter*/
    if (t == VOXEL_GOLD_VEIN && (hash2i(x, y) & 3) == 0) {
        for (int d = 0; d < 4; d++)
            if (exposed[d]) {
                add_emit(c, EMIT_SPARKLE, (float)x + DX[d] * 0.52f, 0.8f, (float)y + DY[d] * 0.52f, RGBA8(255, 220, 90, 255), hash2f(y, x) * 10);
                break;
            }
    }
}

static void build_crystal(Chunk *c, int x, int y, TileType t) {
    uint32_t col = crystal_color(t);
    uint32_t h = hash2i(x, y);
    int n = 3 + (int)(h % 3);
    Mat mt = matg(MAT_CRYSTAL, col, 0.55f);
    mt.uvs = 1.5f;
    for (int i = 0; i < n; i++) {
        uint32_t hh = hash_u32(h + (uint32_t)i * 7919u);
        float ox = (hash1f(hh) - 0.5f) * 0.55f, oz = (hash1f(hh * 3u) - 0.5f) * 0.55f;
        float height = 0.45f + hash1f(hh * 5u) * (i == 0 ? 1.0f : 0.6f);
        float rad = 0.07f + hash1f(hh * 7u) * 0.08f;
        m3 r = m3mul(m3roty(hash1f(hh * 11u) * MX_TAU), m3mul(m3rotx((hash1f(hh * 13u) - 0.5f) * 0.6f), m3rotz((hash1f(hh * 17u) - 0.5f) * 0.6f)));
        v3 base = v3m((float)x + ox, -0.05f, (float)y + oz);
        mesh_prism(&c->opaque, base, rad, rad * 0.8f, height * 0.75f, 6, r, &mt, false);
        v3 top = v3add(base, m3apply(r, v3m(0, height * 0.75f, 0)));
        mesh_prism(&c->opaque, top, rad * 0.8f, 0.0f, height * 0.3f, 6, r, &mt, false);
    }
    float r = ((col) & 255) / 255.0f, g = ((col >> 8) & 255) / 255.0f, b = ((col >> 16) & 255) / 255.0f;
    if (((x + y) & 1) == 0) add_light(c, (float)x, 0.9f, (float)y, r, g, b, 4.0f, 0.9f);
    add_emit(c, EMIT_SPARKLE, (float)x, 0.7f, (float)y, col, hash1f(h) * 10);
}

static void build_mushrooms(Chunk *c, int x, int y) {
    uint32_t h = hash2i(x * 7, y * 13);
    int n = 2 + (int)(h % 3);
    for (int i = 0; i < n; i++) {
        uint32_t hh = hash_u32(h + (uint32_t)i * 104729u);
        float ox = (hash1f(hh) - 0.5f) * 0.6f, oz = (hash1f(hh * 3u) - 0.5f) * 0.6f;
        float sz = 0.6f + hash1f(hh * 5u) * 0.8f;
        Mat stalk = matc(MAT_SKIN, RGBA8(220, 215, 190, 255));
        mesh_prism(&c->opaque, v3m((float)x + ox, 0, (float)y + oz), 0.04f * sz, 0.035f * sz, 0.28f * sz, 6, m3identity(), &stalk, false);
        uint32_t cc = (hh & 1) ? RGBA8(60, 255, 170, 255) : RGBA8(90, 200, 255, 255);
        Mat cap = matg(MAT_MUSHROOM, cc, 0.85f);
        mesh_prism(&c->opaque, v3m((float)x + ox, 0.26f * sz, (float)y + oz), 0.16f * sz, 0.0f, 0.12f * sz, 8, m3identity(), &cap, true);
    }
    if (((x * 3 + y) % 3) == 0) add_light(c, (float)x, 0.6f, (float)y, 0.25f, 1.0f, 0.65f, 3.5f, 0.8f);
    add_emit(c, EMIT_SPORE, (float)x, 0.4f, (float)y, RGBA8(120, 255, 190, 255), hash1f(h) * 10);
}

static void build_door(Chunk *c, int x, int y, bool closed) {
    /*Passage axis from BOTH clues: open tiles in front/behind and solid
     * tiles beside. Looking only for walls on the left AND right failed in
     * round rooms and at corridor corners (the door came out turned 90 deg).*/
    int ns_score = (open_tile(T(x, y - 1)) ? 1 : 0) + (open_tile(T(x, y + 1)) ? 1 : 0) +
                   (tile_is_block(T(x - 1, y)) ? 1 : 0) + (tile_is_block(T(x + 1, y)) ? 1 : 0);
    int ew_score = (open_tile(T(x - 1, y)) ? 1 : 0) + (open_tile(T(x + 1, y)) ? 1 : 0) +
                   (tile_is_block(T(x, y - 1)) ? 1 : 0) + (tile_is_block(T(x, y + 1)) ? 1 : 0);
    bool ns = ns_score >= ew_score;
    Mat frame = matc(MAT_BRICK, RGBA8(210, 200, 190, 255));
    Mat wood = matc(MAT_DOOR, RGBA8(255, 255, 255, 255));
    float hh = 1.25f;
    if (ns) {
        mesh_box(&c->opaque, v3m((float)x - 0.44f, hh * 0.5f, (float)y), v3m(0.07f, hh * 0.5f, 0.14f), m3identity(), &frame);
        mesh_box(&c->opaque, v3m((float)x + 0.44f, hh * 0.5f, (float)y), v3m(0.07f, hh * 0.5f, 0.14f), m3identity(), &frame);
        mesh_box(&c->opaque, v3m((float)x, hh + 0.08f, (float)y), v3m(0.52f, 0.08f, 0.15f), m3identity(), &frame);
        /*leaf swung open against the left post (or shut across the frame)*/
        m3 r = m3roty(closed ? 0.0f : 1.25f);
        v3 hinge = v3m((float)x - 0.36f, 0, (float)y - 0.05f);
        v3 cen = v3add(hinge, m3apply(r, v3m(0.36f, hh * 0.5f - 0.02f, 0)));
        mesh_box(&c->opaque, cen, v3m(0.36f, hh * 0.5f - 0.04f, 0.035f), r, &wood);
    } else {
        mesh_box(&c->opaque, v3m((float)x, hh * 0.5f, (float)y - 0.44f), v3m(0.14f, hh * 0.5f, 0.07f), m3identity(), &frame);
        mesh_box(&c->opaque, v3m((float)x, hh * 0.5f, (float)y + 0.44f), v3m(0.14f, hh * 0.5f, 0.07f), m3identity(), &frame);
        mesh_box(&c->opaque, v3m((float)x, hh + 0.08f, (float)y), v3m(0.15f, 0.08f, 0.52f), m3identity(), &frame);
        m3 r = m3roty(closed ? -MX_PI * 0.5f : -1.25f + MX_PI * 0.5f); /*roty(-pi/2) maps +X to +Z*/
        v3 hinge = v3m((float)x - 0.05f, 0, (float)y - 0.36f);
        v3 cen = v3add(hinge, m3apply(r, v3m(0.36f, hh * 0.5f - 0.02f, 0)));
        mesh_box(&c->opaque, cen, v3m(0.36f, hh * 0.5f - 0.04f, 0.035f), r, &wood);
    }
}

static void build_stairs(Chunk *c, int x, int y, bool down) {
    Mat st = matc(MAT_STAIRS, RGBA8(230, 225, 215, 255));
    /*Orientation: the steps run away from the most open side.*/
    int dir = 0;
    for (int d = 0; d < 4; d++)
        if (open_tile(T(x + DX[d], y + DY[d]))) { dir = d; break; }
    float ang = -(float)dir * MX_PI * 0.5f;
    m3 r = m3roty(ang);
    v3 cen = v3m((float)x, 0, (float)y);
    if (down) {
        /*dark pit with steps descending*/
        Mat pit = matc(MAT_ROCK, RGBA8(70, 64, 60, 255));
        for (int d = 0; d < 4; d++) {
            float x0 = x - 0.5f, x1 = x + 0.5f, z0 = y - 0.5f, z1 = y + 0.5f;
            v3 a, b, n;
            switch (d) {
            case 0: a = v3m(x0, 0, z0); b = v3m(x1, 0, z0); n = v3m(0, 0, 1); break;
            case 1: a = v3m(x1, 0, z0); b = v3m(x1, 0, z1); n = v3m(-1, 0, 0); break;
            case 2: a = v3m(x1, 0, z1); b = v3m(x0, 0, z1); n = v3m(0, 0, -1); break;
            default: a = v3m(x0, 0, z1); b = v3m(x0, 0, z0); n = v3m(1, 0, 0); break;
            }
            mesh_quad(&c->opaque, v3m(a.x, -1.2f, a.z), v3m(b.x, -1.2f, b.z), v3m(b.x, 0, b.z), v3m(a.x, 0, a.z), n, &pit, 0, 0, 1, 1.2f);
        }
        for (int i = 0; i < 4; i++) {
            float zf = -0.5f + 0.125f + (float)i * 0.25f;
            float yb = -0.3f * (float)(i + 1);
            v3 p = v3add(cen, m3apply(r, v3m(0, yb + 0.06f, zf)));
            mesh_box(&c->opaque, p, v3m(0.46f, 0.06f, 0.125f), r, &st);
        }
        Mat bottom = matc(MAT_ROCK, RGBA8(20, 18, 18, 255));
        mesh_quad(&c->opaque, v3m(x - 0.5f, -1.2f, y + 0.5f), v3m(x + 0.5f, -1.2f, y + 0.5f), v3m(x + 0.5f, -1.2f, y - 0.5f),
                  v3m(x - 0.5f, -1.2f, y - 0.5f), v3m(0, 1, 0), &bottom, 0, 0, 1, 1);
        add_light(c, (float)x, 0.6f, (float)y, 1.0f, 0.75f, 0.25f, 3.5f, 0.9f);
        add_emit(c, EMIT_RUNE_DOWN, (float)x, 0.03f, (float)y, RGBA8(255, 190, 60, 255), 0);
    } else {
        build_floor(&c->opaque, x, y, VOXEL_STAIRS_UP, 0.0f, MAT_FLAGSTONE);
        for (int i = 0; i < 4; i++) {
            float zf = 0.5f - 0.125f - (float)i * 0.25f;
            float ytop = 0.22f * (float)(i + 1);
            v3 p = v3add(cen, m3apply(r, v3m(0, ytop * 0.5f, zf)));
            mesh_box(&c->opaque, p, v3m(0.46f, ytop * 0.5f, 0.125f), r, &st);
        }
        add_light(c, (float)x, 1.2f, (float)y, 0.4f, 0.85f, 1.0f, 3.5f, 0.8f);
        add_emit(c, EMIT_RUNE_UP, (float)x, 0.03f, (float)y, RGBA8(90, 220, 255, 255), 0);
    }
}

static void build_liquid(Chunk *c, int x, int y, bool lava) {
    float bottom = lava ? -0.30f : -0.42f;
    float surf = lava ? -0.12f : -0.10f;
    /*bed*/
    build_floor(&c->opaque, x, y, VOXEL_MUD, bottom, lava ? MAT_OBSIDIAN : MAT_MUD);
    /*banks: vertical faces toward non-liquid neighbours*/
    TileType self = lava ? VOXEL_LAVA : VOXEL_WATER;
    for (int d = 0; d < 4; d++) {
        TileType nb = T(x + DX[d], y + DY[d]);
        if (nb == self || tile_is_block(nb)) continue;
        /*face looks into the pool (inward): build on the neighbour edge*/
        float x0 = x - 0.5f, x1 = x + 0.5f, z0 = y - 0.5f, z1 = y + 0.5f;
        v3 a, b, n;
        switch (d) {
        case 0: a = v3m(x0, 0, z0); b = v3m(x1, 0, z0); n = v3m(0, 0, 1); break;
        case 1: a = v3m(x1, 0, z0); b = v3m(x1, 0, z1); n = v3m(-1, 0, 0); break;
        case 2: a = v3m(x1, 0, z1); b = v3m(x0, 0, z1); n = v3m(0, 0, -1); break;
        default: a = v3m(x0, 0, z1); b = v3m(x0, 0, z0); n = v3m(1, 0, 0); break;
        }
        Mat bank = matc(MAT_ROCK, RGBA8(150, 140, 130, 255));
        mesh_quad(&c->opaque, v3m(a.x, bottom, a.z), v3m(b.x, bottom, b.z), v3m(b.x, 0, b.z), v3m(a.x, 0, a.z), n, &bank,
                  0, 0, 1, -bottom);
    }
    float x0 = x - 0.5f, x1 = x + 0.5f, z0 = y - 0.5f, z1 = y + 0.5f;
    if (lava) {
        Mat mt = matg(MAT_LAVA, RGBA8(230, 200, 190, 255), 0.62f);
        mt.flags = SV_LAVA;
        mesh_quad(&c->opaque, v3m(x0, surf, z1), v3m(x1, surf, z1), v3m(x1, surf, z0), v3m(x0, surf, z0), v3m(0, 1, 0), &mt,
                  (float)x * 0.7f, (float)y * 0.7f + 0.7f, (float)x * 0.7f + 0.7f, (float)y * 0.7f);
        if ((x % 3) == 0 && (y % 3) == 0) add_light(c, (float)x, 0.5f, (float)y, 1.0f, 0.42f, 0.08f, 5.5f, 1.3f);
        if ((hash2i(x, y) & 7) == 0) add_emit(c, EMIT_EMBER, (float)x, surf, (float)y, RGBA8(255, 140, 40, 255), hash2f(x, y) * 10);
    } else {
        Mat mt = matc(MAT_WATER, RGBA8(170, 210, 235, 170));
        mt.flags = SV_WATER;
        mt.glow = 0.05f;
        mesh_quad(&c->trans, v3m(x0, surf, z1), v3m(x1, surf, z1), v3m(x1, surf, z0), v3m(x0, surf, z0), v3m(0, 1, 0), &mt,
                  (float)x * 0.5f, (float)y * 0.5f + 0.5f, (float)x * 0.5f + 0.5f, (float)y * 0.5f);
    }
}

static void build_grass_tufts(Chunk *c, int x, int y) {
    uint32_t h = hash2i(x * 31, y * 17);
    if ((h & 3) != 0) return;
    int n = 2 + (int)((h >> 3) % 3);
    for (int i = 0; i < n; i++) {
        uint32_t hh = hash_u32(h + (uint32_t)i * 3571u);
        float ox = (hash1f(hh) - 0.5f) * 0.8f, oz = (hash1f(hh * 3u) - 0.5f) * 0.8f;
        float ht = 0.12f + hash1f(hh * 5u) * 0.16f;
        float a = hash1f(hh * 7u) * MX_TAU;
        float dx = cosf(a) * 0.05f, dz = sinf(a) * 0.05f;
        Mat mt = matc(MAT_LEAF, RGBA8(110, 200, 80, 255));
        mt.flags = SV_FOLIAGE;
        v3 b0 = v3m((float)x + ox - dx, 0, (float)y + oz - dz), b1 = v3m((float)x + ox + dx, 0, (float)y + oz + dz);
        v3 tp = v3m((float)x + ox + dz * 0.6f, ht, (float)y + oz - dx * 0.6f);
        mesh_tri(&c->opaque, b0, b1, tp, 0, 0, 1, 0, 0.5f, 1, v3m(0, 1, 0), &mt);
    }
}

static void build_town_lamp(Chunk *c, int x, int y) {
    Mat iron = matc(MAT_IRON, RGBA8(70, 66, 62, 255));
    mesh_prism(&c->opaque, v3m((float)x, 0, (float)y), 0.06f, 0.045f, 1.8f, 6, m3identity(), &iron, false);
    mesh_box(&c->opaque, v3m((float)x, 1.82f, (float)y), v3m(0.13f, 0.03f, 0.13f), m3identity(), &iron);
    Mat glass = matg(MAT_WHITE, RGBA8(255, 220, 140, 255), 1.0f);
    mesh_box(&c->opaque, v3m((float)x, 1.98f, (float)y), v3m(0.09f, 0.13f, 0.09f), m3identity(), &glass);
    mesh_prism(&c->opaque, v3m((float)x, 2.11f, (float)y), 0.14f, 0.0f, 0.12f, 4, m3roty(0.785f), &iron, true);
    add_light(c, (float)x, 2.0f, (float)y, 1.0f, 0.78f, 0.45f, 7.0f, 1.2f);
    add_emit(c, EMIT_LAMP, (float)x, 1.98f, (float)y, RGBA8(255, 210, 140, 255), hash2f(x, y) * 10);
}

static void build_chunk(int cx, int cy) {
    Chunk *c = &g_chunks[cy][cx];
    mesh_reset(&c->opaque);
    mesh_reset(&c->trans);
    c->nlights = 0;
    c->nemit = 0;
    c->built = true;
    c->dirty = false;
    int x0 = cx * CH, y0 = cy * CH;
    for (int y = y0; y < y0 + CH && y < MAP_HEIGHT; y++)
        for (int x = x0; x < x0 + CH && x < MAP_WIDTH; x++) {
            TileType t = M[y][x];
            if (closed_shop_door(x, y)) {
                build_floor(&c->opaque, x, y, VOXEL_COBBLE, 0.0f, floor_material(VOXEL_COBBLE));
                build_door(c, x, y, true);
                continue;
            }
            if (tile_is_block(t)) {
                build_block(c, x, y, t);
                continue;
            }
            switch (t) {
            case VOXEL_WATER: build_liquid(c, x, y, false); break;
            case VOXEL_LAVA: build_liquid(c, x, y, true); break;
            case VOXEL_STAIRS_DOWN: build_stairs(c, x, y, true); break;
            case VOXEL_STAIRS_UP: build_stairs(c, x, y, false); break;
            default:
                build_floor(&c->opaque, x, y, t, 0.0f, floor_material(t));
                if (is_crystal(t)) build_crystal(c, x, y, t);
                else if (t == VOXEL_MUSHROOM_GLOW) build_mushrooms(c, x, y);
                else if (t == VOXEL_DOOR) build_door(c, x, y, false);
                else if (t == VOXEL_GRASS) build_grass_tufts(c, x, y);
                else if (t == VOXEL_TRAP) add_emit(c, EMIT_TRAP, (float)x, 0.02f, (float)y, RGBA8(255, 60, 40, 255), 0);
                if (g_town && t == VOXEL_COBBLE && (hash2i(x * 13, y * 29) % 100) < 3) {
                    /*lamps only next to the street edge (some wall/grass nearby)*/
                    bool edge = false;
                    for (int d = 0; d < 4; d++)
                        if (T(x + DX[d], y + DY[d]) != VOXEL_COBBLE) edge = true;
                    if (edge) build_town_lamp(c, x, y);
                }
                break;
            }
        }
    c->empty = c->opaque.n == 0 && c->trans.n == 0;
}

/* ------------------------------------------------------------------ */

void world_reset(bool town) {
    g_town = town;
    init_shop_doors();
    for (int y = 0; y < NCY; y++)
        for (int x = 0; x < NCX; x++) {
            g_chunks[y][x].built = false;
            g_chunks[y][x].dirty = true;
        }
    memset(g_built_map, 0xFF, sizeof(g_built_map));
    g_built_version = 0;
    g_vis_cx0 = -1;
}

bool world_is_town(void) { return g_town; }

/*Mark chunks whose tiles changed (and neighbours: faces/AO depend on them).*/
static void diff_map(const TileType map[MAP_HEIGHT][MAP_WIDTH]) {
    for (int y = 0; y < MAP_HEIGHT; y++) {
        const TileType *a = map[y];
        TileType *b = g_built_map[y];
        if (memcmp(a, b, MAP_WIDTH) == 0) continue;
        for (int x = 0; x < MAP_WIDTH; x++) {
            if (a[x] == b[x]) continue;
            int cx = x / CH, cy = y / CH;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int nx = (x + dx) / CH, ny = (y + dy) / CH;
                    if (x + dx < 0 || y + dy < 0 || nx >= NCX || ny >= NCY) continue;
                    g_chunks[ny][nx].dirty = true;
                }
            (void)cx; (void)cy;
        }
        memcpy(b, a, MAP_WIDTH);
    }
}

bool world_update(const TileType map[MAP_HEIGHT][MAP_WIDTH], uint32_t map_version, float fx, float fz, int radius) {
    M = map;
    if (map_version != g_built_version) {
        diff_map(map);
        g_built_version = map_version;
    }
    int cx0 = (int)floorf((fx - radius) / CH), cx1 = (int)floorf((fx + radius) / CH);
    int cy0 = (int)floorf((fz - radius) / CH), cy1 = (int)floorf((fz + radius) / CH);
    if (cx0 < 0) cx0 = 0;
    if (cy0 < 0) cy0 = 0;
    if (cx1 >= NCX) cx1 = NCX - 1;
    if (cy1 >= NCY) cy1 = NCY - 1;
    bool changed = (cx0 != g_vis_cx0 || cx1 != g_vis_cx1 || cy0 != g_vis_cy0 || cy1 != g_vis_cy1);
    /*Rebuild budget: a handful of chunks per frame keeps hitches away;
     * the visible set is rebuilt over a few frames at most.*/
    int budget = 12;
    while (budget > 0) {
        /*nearest pending chunk first: the area around the player is
         * always rebuilt before the horizon*/
        int bx = -1, by = -1;
        float bd = 1e9f;
        for (int cy = cy0; cy <= cy1; cy++)
            for (int cx = cx0; cx <= cx1; cx++) {
                Chunk *c = &g_chunks[cy][cx];
                if (!(c->dirty || !c->built)) continue;
                float dx = (float)(cx * CH + CH / 2) - fx, dz = (float)(cy * CH + CH / 2) - fz;
                float d = dx * dx + dz * dz;
                if (d < bd) { bd = d; bx = cx; by = cy; }
            }
        if (bx < 0) break;
        build_chunk(bx, by);
        budget--;
        changed = true;
    }
    if (changed) {
        g_vis_cx0 = cx0; g_vis_cx1 = cx1; g_vis_cy0 = cy0; g_vis_cy1 = cy1;
        mesh_reset(&g_world);
        for (int cy = cy0; cy <= cy1; cy++)
            for (int cx = cx0; cx <= cx1; cx++)
                if (g_chunks[cy][cx].built) mesh_append(&g_world, &g_chunks[cy][cx].opaque);
        g_world_version++;
    }
    return changed;
}

const Mesh *world_mesh(uint32_t *version) {
    *version = g_world_version;
    return &g_world;
}

void world_append_trans(Mesh *dst) {
    if (g_vis_cx0 < 0) return;
    for (int cy = g_vis_cy0; cy <= g_vis_cy1; cy++)
        for (int cx = g_vis_cx0; cx <= g_vis_cx1; cx++)
            if (g_chunks[cy][cx].built) mesh_append(dst, &g_chunks[cy][cx].trans);
}

int world_collect_lights(GfxLight *out, int max, float px, float pz, float radius) {
    /*Gather candidates, keep the closest `max`.*/
    static GfxLight cand[2048];
    static float dist[2048];
    int n = 0;
    if (g_vis_cx0 < 0) return 0;
    for (int cy = g_vis_cy0; cy <= g_vis_cy1; cy++)
        for (int cx = g_vis_cx0; cx <= g_vis_cx1; cx++) {
            Chunk *c = &g_chunks[cy][cx];
            for (int i = 0; i < c->nlights && n < 2048; i++) {
                float dx = c->lights[i].pos[0] - px, dz = c->lights[i].pos[2] - pz;
                float d = sqrtf(dx * dx + dz * dz);
                if (d > radius + c->lights[i].pos[3]) continue;
                cand[n] = c->lights[i];
                dist[n] = d;
                n++;
            }
        }
    /*partial selection sort*/
    int k = n < max ? n : max;
    for (int i = 0; i < k; i++) {
        int best = i;
        for (int j = i + 1; j < n; j++)
            if (dist[j] < dist[best]) best = j;
        GfxLight tl = cand[i]; cand[i] = cand[best]; cand[best] = tl;
        float td = dist[i]; dist[i] = dist[best]; dist[best] = td;
        out[i] = cand[i];
    }
    return k;
}

int world_collect_emitters(WorldEmitter *out, int max, float px, float pz, float radius) {
    int n = 0;
    if (g_vis_cx0 < 0) return 0;
    for (int cy = g_vis_cy0; cy <= g_vis_cy1; cy++)
        for (int cx = g_vis_cx0; cx <= g_vis_cx1; cx++) {
            Chunk *c = &g_chunks[cy][cx];
            for (int i = 0; i < c->nemit && n < max; i++) {
                float dx = c->emit[i].pos.x - px, dz = c->emit[i].pos.z - pz;
                if (dx * dx + dz * dz > radius * radius) continue;
                out[n++] = c->emit[i];
            }
        }
    return n;
}
