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

/*Internal interfaces between the scene modules (world, models, fx).*/
#ifndef SCENE_INTERNAL_H
#define SCENE_INTERNAL_H

#include "mesh.h"
#include "map.h"
#include "gfx.h"
#include <stdbool.h>

/* ---------------- world (scene_world.c) ---------------- */
enum {
    EMIT_TORCH = 1, EMIT_LAMP, EMIT_EMBER, EMIT_SPARKLE, EMIT_SPORE,
    EMIT_RUNE_DOWN, EMIT_RUNE_UP, EMIT_TRAP
};
typedef struct {
    int type;
    v3 pos;
    uint32_t col;
    float phase;
} WorldEmitter;

bool tile_is_block(TileType t);
bool tile_walkable(TileType t);
uint32_t crystal_color(TileType t);
void world_reset(bool town);
bool world_is_town(void);
bool world_update(const TileType map[MAP_HEIGHT][MAP_WIDTH], uint32_t map_version, float fx, float fz, int radius);
const Mesh *world_mesh(uint32_t *version);
void world_append_trans(Mesh *dst);
int world_collect_lights(GfxLight *out, int max, float px, float pz, float radius);
int world_collect_emitters(WorldEmitter *out, int max, float px, float pz, float radius);

/* ---------------- models (scene_models.c) ---------------- */
enum { WPN_NONE = 0, WPN_SWORD, WPN_AXE, WPN_MACE, WPN_DAGGER, WPN_STAFF, WPN_BOW, WPN_SPEAR, WPN_CLUB };

typedef struct {
    int kind;
    uint32_t seed;
    int archetype;
    int level;
    int race_id, class_id;
    const char *name;
    float walk;      /*walk cycle phase (radians)*/
    float move;      /*0..1 how much it is walking*/
    float attack;    /*0..1 attack swing progress, 0 = idle*/
    float hurt;      /*0..1 hit flash*/
    float time;
    float scale;     /*extra scale (spawn/death animation)*/
    int weapon;
    bool shield, helm, cape, is_boss;
    float fade;      /*1 = opaque; < 1 = dying (drawn translucent)*/
} ModelParams;

typedef struct {
    float height;      /*top of the model (name plate anchor)*/
    bool has_light;
    GfxLight light;
} ModelOut;

void model_build(Mesh *opaque, Mesh *trans, Mesh *add, v3 pos, float yaw, const ModelParams *p,
                 v3 cam_right, v3 cam_up, ModelOut *out);
int weapon_from_name(const char *weapon_name);

/* ---------------- effects (fx.c) ---------------- */
void fx_init(void);
void fx_clear(void);
void fx_update(float dt);
/*Particles*/
void fx_burst(v3 p, int count, uint32_t col, float speed, float life, float size, float gravity, int mat);
void fx_spark(v3 p, v3 vel, uint32_t col, float life, float size, float gravity, int mat);
void fx_ring(v3 p, float radius, int count, uint32_t col, float up_speed, float life);
void fx_beam(v3 a, v3 b, uint32_t col, int count, float life);
void fx_spell_vfx(int type, float sx, float sz, float tx, float tz, float r, float g, float b);
void fx_emitters(const WorldEmitter *e, int n, float dt, double time);
void fx_build(Mesh *add, Mesh *trans, v3 cam_right, v3 cam_up, double time, int quality);
/*Short-lived dynamic lights (spells, hits)*/
int fx_lights(GfxLight *out, int max);
void fx_flash_light(v3 p, float r, float g, float b, float radius, float intensity, float life);
/*Floating combat text (world anchored)*/
enum { FT_DAMAGE_IN = 0, FT_DAMAGE_OUT, FT_HEAL, FT_CRIT, FT_XP, FT_GOLD, FT_MISS, FT_LEVEL, FT_INFO };
void fx_text(v3 p, int type, const char *text);
typedef struct { v3 pos; int type; char text[24]; float age, life; } FloatText;
int fx_texts(const FloatText **out);
/*Camera shake*/
void fx_shake(float amount);
float fx_shake_amount(void);

#endif
