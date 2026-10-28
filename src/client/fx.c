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

/*Visual effects: particles, short-lived lights, floating combat text
 * and camera shake. Replaces client_particles.c / client_fct.c (which
 * lived in a player-relative space and had no notion of entities).
 * Everything here runs on the render thread only: the network thread
 * reports events through the client event queue instead of touching
 * these pools (no locking needed).*/

#include "scene_internal.h"
#include "textures.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define MAX_P 4096
#define MAX_FL 24
#define MAX_TXT 64

typedef struct {
    v3 p, v;
    uint32_t col;
    float age, life, size, grav, drag, rot, spin;
    int mat;
    bool alive;
} Particle;

typedef struct {
    v3 p;
    float r, g, b, radius, intensity, age, life;
    bool alive;
} FlashLight;

static Particle P[MAX_P];
static int p_next;
static FlashLight FL[MAX_FL];
static FloatText TXT[MAX_TXT];
static int ntxt;
static float shake;
static uint32_t rng = 0x12345678u;

static float frand(void) {
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return (float)(rng & 0xFFFFFF) / 16777215.0f;
}
static float srand1(void) { return frand() * 2.0f - 1.0f; }

void fx_init(void) { fx_clear(); }

void fx_clear(void) {
    memset(P, 0, sizeof(P));
    memset(FL, 0, sizeof(FL));
    ntxt = 0;
    shake = 0;
}

static Particle *alloc_p(void) {
    for (int i = 0; i < 64; i++) {
        Particle *p = &P[p_next];
        p_next = (p_next + 1) % MAX_P;
        if (!p->alive) return p;
    }
    /*pool full: recycle the slot anyway*/
    Particle *p = &P[p_next];
    p_next = (p_next + 1) % MAX_P;
    return p;
}

void fx_spark(v3 pos, v3 vel, uint32_t col, float life, float size, float gravity, int mat) {
    Particle *p = alloc_p();
    p->p = pos;
    p->v = vel;
    p->col = col;
    p->age = 0;
    p->life = life;
    p->size = size;
    p->grav = gravity;
    p->drag = 1.2f;
    p->rot = frand() * MX_TAU;
    p->spin = srand1() * 3.0f;
    p->mat = mat;
    p->alive = true;
}

void fx_burst(v3 pos, int count, uint32_t col, float speed, float life, float size, float gravity, int mat) {
    for (int i = 0; i < count; i++) {
        v3 d = v3norm(v3m(srand1(), srand1() * 0.6f + 0.5f, srand1()));
        fx_spark(pos, v3scale(d, speed * (0.4f + 0.6f * frand())), col, life * (0.6f + 0.6f * frand()),
                 size * (0.6f + 0.8f * frand()), gravity, mat);
    }
}

void fx_ring(v3 pos, float radius, int count, uint32_t col, float up_speed, float life) {
    for (int i = 0; i < count; i++) {
        float a = (float)i / (float)count * MX_TAU;
        v3 q = v3add(pos, v3m(cosf(a) * radius, 0.05f, sinf(a) * radius));
        fx_spark(q, v3m(-sinf(a) * 0.4f, up_speed * (0.6f + 0.6f * frand()), cosf(a) * 0.4f), col, life, 0.12f, 0, MAT_GLOW);
    }
}

void fx_beam(v3 a, v3 b, uint32_t col, int count, float life) {
    for (int i = 0; i < count; i++) {
        float t = frand();
        v3 q = v3lerp(a, b, t);
        q = v3add(q, v3m(srand1() * 0.08f, srand1() * 0.08f, srand1() * 0.08f));
        fx_spark(q, v3m(srand1() * 0.2f, srand1() * 0.2f, srand1() * 0.2f), col, life * (0.5f + t * 0.5f), 0.1f, 0, MAT_GLOW);
    }
}

void fx_flash_light(v3 p, float r, float g, float b, float radius, float intensity, float life) {
    for (int i = 0; i < MAX_FL; i++)
        if (!FL[i].alive) {
            FL[i] = (FlashLight){p, r, g, b, radius, intensity, 0, life, true};
            return;
        }
}

int fx_lights(GfxLight *out, int max) {
    int n = 0;
    for (int i = 0; i < MAX_FL && n < max; i++) {
        if (!FL[i].alive) continue;
        float k = 1.0f - FL[i].age / FL[i].life;
        GfxLight *l = &out[n++];
        l->pos[0] = FL[i].p.x; l->pos[1] = FL[i].p.y; l->pos[2] = FL[i].p.z; l->pos[3] = FL[i].radius;
        l->col[0] = FL[i].r; l->col[1] = FL[i].g; l->col[2] = FL[i].b; l->col[3] = FL[i].intensity * k;
    }
    return n;
}

/*Server spell VFX (MSG_SPELL_VFX). type: 0 projectile, 1 explosion,
 * 2 aura/heal, 3 magic missile, 4 blizzard (see server).*/
void fx_spell_vfx(int type, float sx, float sz, float tx, float tz, float r, float g, float b) {
    uint32_t col = RGBA8((int)(clampf(r, 0, 1) * 255), (int)(clampf(g, 0, 1) * 255), (int)(clampf(b, 0, 1) * 255), 255);
    v3 s = v3m(sx, 1.0f, sz), t = v3m(tx, 0.8f, tz);
    switch (type) {
    case 0:
    case 3: {
        fx_beam(s, t, col, type == 3 ? 60 : 30, 0.5f);
        fx_burst(t, 30, col, 2.5f, 0.6f, 0.14f, -2.0f, MAT_SPARK);
        fx_flash_light(t, r, g, b, 4.0f, 2.0f, 0.5f);
        break;
    }
    case 1:
        fx_beam(s, t, col, 25, 0.35f);
        fx_burst(t, 140, col, 5.0f, 0.9f, 0.25f, -1.0f, MAT_GLOW);
        fx_burst(t, 40, RGBA8(255, 240, 200, 255), 7.0f, 0.4f, 0.12f, -4.0f, MAT_SPARK);
        fx_flash_light(t, r, g, b, 7.0f, 3.5f, 0.8f);
        fx_shake(0.35f);
        break;
    case 2:
        fx_ring(v3m(tx, 0, tz), 0.6f, 40, col, 1.5f, 1.2f);
        fx_burst(v3m(tx, 0.8f, tz), 30, col, 1.0f, 1.2f, 0.12f, 0.6f, MAT_SPARK);
        fx_flash_light(v3m(tx, 1.0f, tz), r, g, b, 4.0f, 1.5f, 1.0f);
        break;
    case 4:
        for (int i = 0; i < 160; i++) {
            v3 q = v3m(tx + srand1() * 2.5f, 3.0f + frand() * 2.0f, tz + srand1() * 2.5f);
            fx_spark(q, v3m(srand1() * 0.4f, -3.0f - frand() * 2.0f, srand1() * 0.4f), RGBA8(200, 230, 255, 255), 1.2f, 0.09f, 0, MAT_SPARK);
        }
        fx_flash_light(v3m(tx, 1.5f, tz), 0.6f, 0.8f, 1.0f, 6.0f, 2.0f, 1.2f);
        break;
    default:
        fx_burst(t, 50, col, 3.0f, 0.7f, 0.15f, -1.0f, MAT_GLOW);
        break;
    }
}

/*World emitters near the player (torches, embers, crystals...).*/
void fx_emitters(const WorldEmitter *e, int n, float dt, double time) {
    for (int i = 0; i < n; i++) {
        const WorldEmitter *w = &e[i];
        switch (w->type) {
        case EMIT_TORCH:
        case EMIT_LAMP:
            if (frand() < dt * (w->type == EMIT_TORCH ? 7.0f : 1.5f))
                fx_spark(v3add(w->pos, v3m(srand1() * 0.04f, 0.08f, srand1() * 0.04f)),
                         v3m(srand1() * 0.1f, 0.6f + frand() * 0.4f, srand1() * 0.1f),
                         RGBA8(255, 150, 50, 255), 0.7f, 0.035f, 0.0f, MAT_GLOW);
            break;
        case EMIT_EMBER:
            if (frand() < dt * 1.2f)
                fx_spark(v3add(w->pos, v3m(srand1() * 0.4f, 0.05f, srand1() * 0.4f)),
                         v3m(srand1() * 0.2f, 0.8f + frand() * 0.8f, srand1() * 0.2f),
                         RGBA8(255, 120 + (int)(frand() * 80), 30, 255), 1.6f, 0.05f, -0.2f, MAT_GLOW);
            break;
        case EMIT_SPARKLE:
            if (frand() < dt * 0.9f)
                fx_spark(v3add(w->pos, v3m(srand1() * 0.35f, srand1() * 0.4f, srand1() * 0.35f)),
                         v3m(0, 0.1f, 0), w->col, 0.9f, 0.09f, 0, MAT_SPARK);
            break;
        case EMIT_SPORE:
            if (frand() < dt * 0.8f)
                fx_spark(v3add(w->pos, v3m(srand1() * 0.4f, 0, srand1() * 0.4f)),
                         v3m(srand1() * 0.08f, 0.15f + frand() * 0.15f, srand1() * 0.08f), w->col, 3.0f, 0.04f, 0, MAT_GLOW);
            break;
        case EMIT_RUNE_DOWN:
        case EMIT_RUNE_UP:
            if (frand() < dt * 3.0f) {
                float a = frand() * MX_TAU;
                fx_spark(v3add(w->pos, v3m(cosf(a) * 0.45f, 0.05f, sinf(a) * 0.45f)),
                         v3m(0, w->type == EMIT_RUNE_UP ? 0.9f : 0.6f, 0), w->col, 1.2f, 0.05f, 0, MAT_GLOW);
            }
            break;
        default:
            break;
        }
    }
    (void)time;
}

void fx_update(float dt) {
    for (int i = 0; i < MAX_P; i++) {
        Particle *p = &P[i];
        if (!p->alive) continue;
        p->age += dt;
        if (p->age >= p->life) { p->alive = false; continue; }
        p->v.y -= p->grav * dt * 9.81f * 0.5f;
        float d = expf(-p->drag * dt);
        p->v = v3scale(p->v, d);
        p->p = v3add(p->p, v3scale(p->v, dt));
        p->rot += p->spin * dt;
        if (p->p.y < -0.4f && p->grav > 0) { p->p.y = -0.4f; p->v.y *= -0.3f; }
    }
    for (int i = 0; i < MAX_FL; i++)
        if (FL[i].alive) {
            FL[i].age += dt;
            if (FL[i].age >= FL[i].life) FL[i].alive = false;
        }
    for (int i = 0; i < ntxt; i++) {
        TXT[i].age += dt;
        TXT[i].pos.y += dt * (TXT[i].type == FT_LEVEL ? 0.5f : 1.1f) * (1.0f - TXT[i].age / TXT[i].life);
        if (TXT[i].age >= TXT[i].life) {
            TXT[i] = TXT[--ntxt];
            i--;
        }
    }
    shake = fmaxf(0, shake - dt * 1.8f);
}

void fx_build(Mesh *add, Mesh *trans, v3 cr, v3 cu, double time, int quality) {
    (void)trans;
    (void)time;
    int stride = quality >= 2 ? 1 : (quality == 1 ? 2 : 3);
    for (int i = 0; i < MAX_P; i += stride) {
        Particle *p = &P[i];
        if (!p->alive) continue;
        float k = p->age / p->life;
        float a = (k < 0.15f ? k / 0.15f : 1.0f) * (1.0f - k * k);
        uint32_t c = col_scale(p->col, a);
        c = (c & 0x00FFFFFFu) | (255u << 24);
        Mat mt = matg(p->mat, c, 1.0f);
        mesh_billboard(add, p->p, p->size * (1.0f + 0.3f * k), cr, cu, p->rot, &mt);
    }
}

void fx_text(v3 pos, int type, const char *text) {
    if (ntxt >= MAX_TXT) {
        memmove(&TXT[0], &TXT[1], sizeof(FloatText) * (MAX_TXT - 1));
        ntxt = MAX_TXT - 1;
    }
    FloatText *t = &TXT[ntxt++];
    t->pos = v3add(pos, v3m(srand1() * 0.25f, 0, srand1() * 0.25f));
    t->type = type;
    snprintf(t->text, sizeof(t->text), "%s", text);
    t->age = 0;
    t->life = (type == FT_LEVEL) ? 2.6f : (type == FT_CRIT ? 1.6f : 1.25f);
}

int fx_texts(const FloatText **out) {
    *out = TXT;
    return ntxt;
}

void fx_shake(float amount) {
    if (amount > shake) shake = amount;
}

float fx_shake_amount(void) { return shake; }
