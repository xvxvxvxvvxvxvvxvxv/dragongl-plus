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

/*Scene orchestration — see scene.h.*/

#include "scene.h"
#include "scene_internal.h"
#include "textures.h"
#include "client_tome.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    int id;
    bool init;
    float x, z;          /*rendered (interpolated) position*/
    float yaw;
    float walk, move;
    float attack;        /*0 idle, (0,1] swinging*/
    float hurt;
    float spawn;         /*0..1 appear animation*/
    int last_x, last_y;
} EntAnim;

typedef struct {
    bool alive;
    Entity e;
    float x, z, yaw, t;
    ModelParams p;
} Corpse;

static EntAnim g_anim[CLIENT_MAX_ENTITIES];
static EntAnim g_me;
static Corpse g_corpses[32];
static bool g_title_filled;

static uint8_t g_explored[MAP_HEIGHT][MAP_WIDTH];
static uint32_t g_explored_version = 1;
static int g_floor = -1;
static int g_explore_x = -999, g_explore_y = -999, g_explore_r = -1;

static float cam_yaw = 0.0f, cam_pitch = 0.88f, cam_dist = 15.0f;
static float cam_yaw_t = 0.0f, cam_pitch_t = 0.88f, cam_dist_t = 15.0f;
static v3 cam_target;
static bool cam_init;
static m4 g_viewproj, g_inv_viewproj;
static v3 g_eye, g_cam_right, g_cam_up;
static float g_fbw = 1, g_fbh = 1, g_uiscale = 1;

static Mesh g_dyn, g_trans, g_add;
static SceneLabel g_labels[CLIENT_MAX_ENTITIES];
static int g_nlabels;
static WorldEmitter g_emit[512];
static double g_time;

/* ------------------------------------------------------------------ */

void scene_init(void) {
    fx_init();
    memset(g_anim, 0, sizeof(g_anim));
    memset(&g_me, 0, sizeof(g_me));
    world_reset(true);
}

void scene_new_floor(int floor) {
    g_floor = floor;
    world_reset(floor == 0);
    memset(g_explored, 0, sizeof(g_explored));
    g_explored_version++;
    memset(g_anim, 0, sizeof(g_anim));
    memset(g_corpses, 0, sizeof(g_corpses));
    g_me.init = false;
    cam_init = false;
    g_explore_x = -999;
    g_title_filled = false;
    fx_clear();
}

const uint8_t *scene_explored(void) { return &g_explored[0][0]; }
bool scene_tile_explored(int x, int y) {
    if (x < 0 || y < 0 || x >= MAP_WIDTH || y >= MAP_HEIGHT) return false;
    return g_explored[y][x] != 0;
}

static void explore(int px, int py, int r) {
    if (px == g_explore_x && py == g_explore_y && r == g_explore_r) return;
    g_explore_x = px; g_explore_y = py; g_explore_r = r;
    bool changed = false;
    int rr = r + 1;
    for (int y = py - rr; y <= py + rr; y++)
        for (int x = px - rr; x <= px + rr; x++) {
            if (x < 0 || y < 0 || x >= MAP_WIDTH || y >= MAP_HEIGHT) continue;
            int dx = x - px, dy = y - py;
            if (dx * dx + dy * dy > rr * rr) continue;
            if (!g_explored[y][x]) { g_explored[y][x] = 255; changed = true; }
        }
    if (changed) g_explored_version++;
}

/* ------------------------------------------------------------------ */
/* Camera                                                              */
/* ------------------------------------------------------------------ */

void scene_cam_orbit(float dyaw, float dpitch) {
    cam_yaw_t += dyaw;
    cam_pitch_t = clampf(cam_pitch_t + dpitch, 0.35f, 1.45f);
}
void scene_cam_zoom(float f) { cam_dist_t = clampf(cam_dist_t * f, 6.0f, 34.0f); }
void scene_cam_reset(void) { cam_yaw_t = 0; cam_pitch_t = 0.88f; cam_dist_t = 15.0f; }
float scene_cam_yaw(void) { return cam_yaw; }
v3 scene_player_pos(void) { return v3m(g_me.x, 0, g_me.z); }

static bool g_preview;
static bool g_gallery;
void scene_set_gallery(bool on) { g_gallery = on; }

static void update_camera(float dt, v3 target, bool title, double time) {
    if (!cam_init) {
        cam_target = target;
        cam_init = true;
    }
    cam_target = v3m(damp(cam_target.x, target.x, 10.0f, dt), damp(cam_target.y, target.y, 10.0f, dt),
                     damp(cam_target.z, target.z, 10.0f, dt));
    if (title && g_preview) {
        cam_yaw_t = 0.35f;
        cam_pitch_t = 0.20f;
        cam_dist_t = 5.4f;
    } else if (title && g_gallery) {
        const char *gy = getenv("DRAGONGL_GALLERY_YAW");
        cam_yaw_t = gy ? (float)atof(gy) : 0.25f;
        cam_pitch_t = 0.22f;
        const char *gs = getenv("DRAGONGL_GALLERY");
        cam_dist_t = (gs && gs[0] == 'r') ? 3.2f : (gs && gs[0] == 'd') ? 6.0f : 8.5f;
        if (gs && gs[0] == 'd') cam_pitch_t = 0.7f;
    } else if (title) {
        cam_yaw_t = (float)time * 0.05f;
        cam_pitch_t = 0.62f;
        cam_dist_t = 20.0f;
    }
    cam_yaw = damp(cam_yaw, cam_yaw_t, 12.0f, dt);
    cam_pitch = damp(cam_pitch, cam_pitch_t, 12.0f, dt);
    cam_dist = damp(cam_dist, cam_dist_t, 10.0f, dt);
    float sh = fx_shake_amount();
    v3 jitter = v3m(sinf((float)time * 53.0f) * sh * 0.15f, sinf((float)time * 61.0f) * sh * 0.1f, cosf((float)time * 47.0f) * sh * 0.15f);
    v3 look = v3add(cam_target, v3m(0, g_preview ? -0.05f : 0.6f, 0));
    v3 eye = v3add(look, v3m(cam_dist * cosf(cam_pitch) * sinf(cam_yaw), cam_dist * sinf(cam_pitch),
                             cam_dist * cosf(cam_pitch) * cosf(cam_yaw)));
    eye = v3add(eye, jitter);
    m4 view = m4lookat(eye, v3add(look, jitter), v3m(0, 1, 0));
    m4 proj = m4perspective(0.70f, g_fbw / g_fbh, 0.3f, 200.0f);
    g_viewproj = m4mul(proj, view);
    m4invert(&g_viewproj, &g_inv_viewproj);
    g_eye = eye;
    g_cam_right = v3m(view.m[0], view.m[4], view.m[8]);
    g_cam_up = v3m(view.m[1], view.m[5], view.m[9]);
}

bool scene_world_to_screen(v3 p, float *lx, float *ly) {
    float in[4] = {p.x, p.y, p.z, 1}, o[4];
    m4xform4(&g_viewproj, in, o);
    if (o[3] <= 0.01f) return false;
    float nx = o[0] / o[3], ny = o[1] / o[3];
    *lx = (nx * 0.5f + 0.5f) * g_fbw / g_uiscale;
    *ly = (1.0f - (ny * 0.5f + 0.5f)) * g_fbh / g_uiscale;
    return nx > -1.3f && nx < 1.3f && ny > -1.3f && ny < 1.3f;
}

static bool screen_ray(float lx, float ly, v3 *o, v3 *d) {
    float nx = lx * g_uiscale / g_fbw * 2.0f - 1.0f;
    float ny = 1.0f - ly * g_uiscale / g_fbh * 2.0f;
    float a[4] = {nx, ny, -1, 1}, b[4] = {nx, ny, 1, 1}, pa[4], pb[4];
    m4xform4(&g_inv_viewproj, a, pa);
    m4xform4(&g_inv_viewproj, b, pb);
    if (fabsf(pa[3]) < 1e-6f || fabsf(pb[3]) < 1e-6f) return false;
    v3 p0 = v3m(pa[0] / pa[3], pa[1] / pa[3], pa[2] / pa[3]);
    v3 p1 = v3m(pb[0] / pb[3], pb[1] / pb[3], pb[2] / pb[3]);
    *o = p0;
    *d = v3norm(v3sub(p1, p0));
    return true;
}

bool scene_screen_to_tile(float lx, float ly, int *tx, int *ty) {
    v3 o, d;
    if (!screen_ray(lx, ly, &o, &d) || d.y > -1e-4f) return false;
    float t = -o.y / d.y;
    v3 p = v3add(o, v3scale(d, t));
    *tx = (int)floorf(p.x + 0.5f);
    *ty = (int)floorf(p.z + 0.5f);
    return *tx >= 0 && *ty >= 0 && *tx < MAP_WIDTH && *ty < MAP_HEIGHT;
}

int scene_entity_at(float lx, float ly) {
    int best = 0;
    float bd = 1e9f;
    for (int i = 0; i < g_nlabels; i++) {
        SceneLabel *l = &g_labels[i];
        /*the body spans from the feet to the head plate*/
        float fx, fy;
        const EntAnim *a = &g_anim[l->slot];
        if (!scene_world_to_screen(v3m(a->x, 0, a->z), &fx, &fy)) continue;
        float top = l->sy, bottom = fy;
        float w = fmaxf(26.0f, (bottom - top) * 0.45f);
        if (lx < fx - w || lx > fx + w || ly < top - 6 || ly > bottom + 10) continue;
        float d = fabsf(lx - fx) + fabsf(ly - (top + bottom) * 0.5f) * 0.5f;
        if (d < bd) { bd = d; best = l->id; }
    }
    return best;
}

int scene_labels(const SceneLabel **out) {
    *out = g_labels;
    return g_nlabels;
}

int scene_screen_texts(ScreenText *out, int max) {
    const FloatText *t;
    int n = fx_texts(&t);
    int k = 0;
    for (int i = 0; i < n && k < max; i++) {
        float sx, sy;
        if (!scene_world_to_screen(t[i].pos, &sx, &sy)) continue;
        float f = t[i].age / t[i].life;
        out[k].sx = sx;
        out[k].sy = sy;
        out[k].alpha = f < 0.7f ? 1.0f : (1.0f - f) / 0.3f;
        float pop = f < 0.12f ? 1.0f + (1.0f - f / 0.12f) * 0.6f : 1.0f;
        out[k].scale = pop;
        out[k].type = t[i].type;
        out[k].text = t[i].text;
        k++;
    }
    return k;
}

/* ------------------------------------------------------------------ */
/* Events -> animations/effects                                        */
/* ------------------------------------------------------------------ */

static int slot_of(const FrameSnapshot *s, int id) {
    if (id <= 0) return -1;
    for (int i = 0; i < CLIENT_MAX_ENTITIES; i++)
        if ((s->entities[i].active || s->entities[i].killed) && s->entities[i].id == id) return i;
    return -1;
}

static v3 ent_pos(const FrameSnapshot *s, int id, int fx, int fy) {
    if (id == s->my_entity_id && g_me.init) return v3m(g_me.x, 0, g_me.z);
    int sl = slot_of(s, id);
    if (sl >= 0 && g_anim[sl].init) return v3m(g_anim[sl].x, 0, g_anim[sl].z);
    return v3m((float)fx, 0, (float)fy);
}

static EntAnim *anim_of(const FrameSnapshot *s, int id) {
    if (id == s->my_entity_id) return &g_me;
    int sl = slot_of(s, id);
    return sl >= 0 ? &g_anim[sl] : NULL;
}

static void face(EntAnim *a, v3 from, v3 to) {
    float dx = to.x - from.x, dz = to.z - from.z;
    if (dx * dx + dz * dz > 1e-4f) a->yaw = atan2f(dx, dz);
}

void scene_handle_event(const ClientEvent *e, const FrameSnapshot *s) {
    char buf[32];
    switch (e->kind) {
    case CEV_SERVER: {
        const MsgGameEvent *g = &e->ev;
        v3 dst = ent_pos(s, g->dst_id, g->x, g->y);
        v3 src = ent_pos(s, g->src_id, g->x, g->y);
        bool me_src = g->src_id == s->my_entity_id, me_dst = g->dst_id == s->my_entity_id;
        switch (g->type) {
        case GEV_ATTACK_HIT:
        case GEV_ATTACK_CRIT:
        case GEV_ATTACK_MISS: {
            EntAnim *as = anim_of(s, g->src_id);
            if (as) { as->attack = 0.001f; face(as, src, dst); }
            if (g->type == GEV_ATTACK_MISS) {
                fx_text(v3add(dst, v3m(0, 1.6f, 0)), FT_MISS, "miss");
                fx_burst(v3add(dst, v3m(0, 0.9f, 0)), 6, RGBA8(200, 200, 210, 255), 2.0f, 0.3f, 0.06f, 0.5f, MAT_SPARK);
                break;
            }
            EntAnim *ad = anim_of(s, g->dst_id);
            if (ad) ad->hurt = 1.0f;
            bool crit = g->type == GEV_ATTACK_CRIT;
            fx_burst(v3add(dst, v3m(0, 0.9f, 0)), crit ? 40 : 18, RGBA8(200, 20, 20, 255), crit ? 3.5f : 2.2f, 0.6f, 0.07f, 1.0f, MAT_GLOW);
            fx_burst(v3add(dst, v3m(0, 1.0f, 0)), crit ? 16 : 6, RGBA8(255, 230, 180, 255), 4.0f, 0.25f, 0.08f, 0.0f, MAT_SPARK);
            if (!me_dst) {
                snprintf(buf, sizeof(buf), crit ? "%d!" : "%d", g->value);
                fx_text(v3add(dst, v3m(0, 1.7f, 0)), crit ? FT_CRIT : FT_DAMAGE_OUT, buf);
            }
            if (crit && (me_src || me_dst)) fx_shake(0.3f);
            if (me_dst) fx_shake(0.18f);
            break;
        }
        case GEV_HURT: {
            EntAnim *ad = anim_of(s, g->dst_id);
            if (ad) ad->hurt = 1.0f;
            if (!me_dst) {
                snprintf(buf, sizeof(buf), "%d", g->value);
                fx_text(v3add(dst, v3m(0, 1.7f, 0)), FT_DAMAGE_OUT, buf);
            }
            break;
        }
        case GEV_KILL: {
            int sl = slot_of(s, g->dst_id);
            if (sl >= 0) {
                for (int i = 0; i < 32; i++)
                    if (!g_corpses[i].alive) {
                        Corpse *c = &g_corpses[i];
                        c->alive = true;
                        c->e = s->entities[sl];
                        c->x = g_anim[sl].x;
                        c->z = g_anim[sl].z;
                        c->yaw = g_anim[sl].yaw;
                        c->t = 0;
                        break;
                    }
            }
            fx_burst(v3add(dst, v3m(0, 0.6f, 0)), 40, RGBA8(120, 110, 130, 255), 1.6f, 1.2f, 0.18f, -0.3f, MAT_GLOW);
            fx_burst(v3add(dst, v3m(0, 0.8f, 0)), 20, RGBA8(255, 220, 140, 255), 3.0f, 0.6f, 0.06f, 0.4f, MAT_SPARK);
            fx_flash_light(v3add(dst, v3m(0, 1, 0)), 1.0f, 0.8f, 0.5f, 3.0f, 1.2f, 0.4f);
            (void)me_src;
            break;
        }
        case GEV_LEVEL_UP: {
            v3 p = src;
            fx_ring(p, 0.8f, 60, RGBA8(255, 215, 90, 255), 3.0f, 1.6f);
            for (int i = 0; i < 80; i++)
                fx_spark(v3add(p, v3m(((float)(i % 9) - 4) * 0.05f, 0.1f, ((float)(i / 9) - 4) * 0.05f)),
                         v3m(0, 2.0f + (float)(i % 7) * 0.4f, 0), RGBA8(255, 240, 170, 255), 1.4f, 0.1f, 0, MAT_SPARK);
            fx_flash_light(v3add(p, v3m(0, 1.5f, 0)), 1.0f, 0.85f, 0.4f, 8.0f, 3.0f, 1.8f);
            if (me_src) fx_text(v3add(p, v3m(0, 2.2f, 0)), FT_LEVEL, "LEVEL UP!");
            break;
        }
        case GEV_BOSS_DEFEATED:
            fx_flash_light(v3add(dst, v3m(0, 2, 0)), 1.0f, 0.9f, 0.6f, 12.0f, 3.0f, 2.0f);
            fx_shake(0.6f);
            break;
        default:
            break;
        }
        break;
    }
    case CEV_VFX:
        fx_spell_vfx(e->vfx.vfx_type, (float)e->vfx.start_x, (float)e->vfx.start_y, (float)e->vfx.target_x,
                     (float)e->vfx.target_y, e->vfx.color_r, e->vfx.color_g, e->vfx.color_b);
        break;
    case CEV_GOLD_DELTA:
        if (e->value > 0) {
            snprintf(buf, sizeof(buf), "+%d gold", e->value);
            fx_text(v3m(g_me.x, 1.9f, g_me.z), FT_GOLD, buf);
            fx_burst(v3m(g_me.x, 1.0f, g_me.z), 16, RGBA8(255, 220, 90, 255), 2.0f, 0.7f, 0.07f, 1.0f, MAT_SPARK);
        }
        break;
    case CEV_HP_DELTA:
        if (e->value > 0) {
            snprintf(buf, sizeof(buf), "+%d", e->value);
            fx_text(v3m(g_me.x, 1.8f, g_me.z), FT_HEAL, buf);
            fx_ring(v3m(g_me.x, 0, g_me.z), 0.5f, 18, RGBA8(90, 255, 120, 255), 1.4f, 1.0f);
        } else if (e->value < 0) {
            snprintf(buf, sizeof(buf), "%d", e->value);
            fx_text(v3m(g_me.x, 1.8f, g_me.z), FT_DAMAGE_IN, buf);
            g_me.hurt = 1.0f;
        }
        break;
    case CEV_XP_DELTA:
        snprintf(buf, sizeof(buf), "+%d XP", e->value);
        fx_text(v3m(g_me.x, 2.1f, g_me.z), FT_XP, buf);
        break;
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Frame                                                               */
/* ------------------------------------------------------------------ */

static void step_anim(EntAnim *a, int tx, int ty, float dt, float speed) {
    if (!a->init) {
        a->x = (float)tx;
        a->z = (float)ty;
        a->init = true;
        a->last_x = tx;
        a->last_y = ty;
        a->spawn = 0;
        a->yaw = 0;
    }
    float dx = (float)tx - a->x, dz = (float)ty - a->z;
    float d = sqrtf(dx * dx + dz * dz);
    if (d > 4.0f) { a->x = (float)tx; a->z = (float)ty; d = 0; }
    if (d > 1e-3f) {
        float step = fminf(d, speed * dt * (1.0f + d * 0.5f));
        a->x += dx / d * step;
        a->z += dz / d * step;
        float ty_ = atan2f(dx, dz);
        a->yaw += angle_diff(a->yaw, ty_) * fminf(1.0f, dt * 14.0f);
        a->move = fminf(1.0f, a->move + dt * 8.0f);
        a->walk += dt * speed * 4.2f;
    } else {
        a->move = fmaxf(0.0f, a->move - dt * 5.0f);
        if (a->move > 0) a->walk += dt * 6.0f;
    }
    if (a->attack > 0) {
        a->attack += dt * 2.6f;
        if (a->attack >= 1.0f) a->attack = 0;
    }
    a->hurt = fmaxf(0, a->hurt - dt * 3.5f);
    a->spawn = fminf(1.0f, a->spawn + dt * 3.0f);
}

static void add_light(GfxLight *L, int *n, v3 p, float r, float g, float b, float radius, float inten) {
    if (*n >= GFX_MAX_LIGHTS) return;
    GfxLight *l = &L[(*n)++];
    l->pos[0] = p.x; l->pos[1] = p.y; l->pos[2] = p.z; l->pos[3] = radius;
    l->col[0] = r; l->col[1] = g; l->col[2] = b; l->col[3] = inten;
}

static bool hostile_entity(const Entity *e) {
    return !e->is_player && !e->is_merchant && !e->is_tombstone && e->kind != KIND_CHEST && e->kind != KIND_GOLD;
}

void scene_frame(const FrameSnapshot *snap, const SceneInput *in, float dt, double time,
                 int fb_w, int fb_h, SceneFrame *out) {
    g_time = time;
    g_fbw = (float)(fb_w > 0 ? fb_w : 1);
    g_fbh = (float)(fb_h > 0 ? fb_h : 1);
    g_uiscale = in->ui_scale > 0 ? in->ui_scale : 1;
    if (snap->my_floor != g_floor && !in->title_mode) scene_new_floor(snap->my_floor);

    mesh_reset(&g_dyn);
    mesh_reset(&g_trans);
    mesh_reset(&g_add);
    memset(out, 0, sizeof(*out));
    out->draw_scene = true;

    /*--- player ---*/
    float move_speed = 1.0f / fmaxf(0.08f, snap->movement_cooldown > 0 ? snap->movement_cooldown : 0.2f);
    g_preview = in->title_mode && in->preview;
    if (in->title_mode) {
        v3 c = g_preview ? v3m((float)in->preview_x, 0, (float)in->preview_y) : v3m((float)snap->my_x, 0, (float)snap->my_y);
        g_me.x = c.x; g_me.z = c.z; g_me.init = true;
    } else {
        step_anim(&g_me, snap->my_x, snap->my_y, dt, move_speed);
    }
    v3 pp = v3m(g_me.x, 0, g_me.z);
    int vr = snap->vision_radius > 0 ? snap->vision_radius : 6;
    if (in->title_mode) vr = 40;
    if (!in->title_mode) explore(snap->my_x, snap->my_y, vr);
    update_camera(dt, pp, in->title_mode, time);

    /*--- world ---*/
    int build_r = (int)fmaxf(28.0f, cam_dist * 1.6f + 6.0f);
    if (in->title_mode) build_r = 44;
    world_update(snap->map, snap->map_version, cam_target.x, cam_target.z, build_r);
    uint32_t wver;
    const Mesh *wm = world_mesh(&wver);
    out->world = wm->v;
    out->world_count = wm->n;
    out->world_version = wver;
    world_append_trans(&g_trans);

    /*--- emitters & particles ---*/
    int ne = world_collect_emitters(g_emit, 512, pp.x, pp.z, (float)vr + 6.0f);
    fx_emitters(g_emit, ne, dt, time);
    fx_update(dt);
    for (int i = 0; i < ne; i++) {
        WorldEmitter *w = &g_emit[i];
        if (w->type == EMIT_TORCH || w->type == EMIT_LAMP) {
            float fl = 0.85f + 0.15f * sinf((float)time * 13.0f + w->phase) * sinf((float)time * 7.3f + w->phase * 2);
            Mat flame = matg(MAT_FLAME, RGBA8(255, 170, 70, 255), 1.0f);
            float sz = w->type == EMIT_TORCH ? 0.16f * fl : 0.10f;
            if (w->type == EMIT_TORCH) {
                mesh_billboard(&g_add, v3add(w->pos, v3m(0, 0.06f, 0)), sz, g_cam_right, g_cam_up, 0, &flame);
                Mat core = matg(MAT_GLOW, RGBA8(255, 230, 160, 255), 1.0f);
                mesh_billboard(&g_add, w->pos, sz * 0.6f, g_cam_right, g_cam_up, 0, &core);
            }
            Mat halo = matg(MAT_GLOW, RGBA8(120, 70, 25, 255), 1.0f);
            mesh_billboard(&g_add, w->pos, (w->type == EMIT_TORCH ? 0.6f : 0.9f) * fl, g_cam_right, g_cam_up, 0, &halo);
        } else if (w->type == EMIT_RUNE_DOWN || w->type == EMIT_RUNE_UP) {
            float pulse = 0.7f + 0.3f * sinf((float)time * 2.0f);
            Mat rune = matg(MAT_RUNE, col_scale(w->col, pulse), 1.0f);
            mesh_decal(&g_add, w->pos.x, w->pos.z, w->pos.y + (w->type == EMIT_RUNE_UP ? 0.9f : 0.0f), 0.62f, (float)time * 0.4f, &rune);
        } else if (w->type == EMIT_TRAP) {
            Mat rune = matg(MAT_RUNE, RGBA8(140, 30, 20, 255), 1.0f);
            mesh_decal(&g_add, w->pos.x, w->pos.z, w->pos.y, 0.45f, -(float)time * 0.6f, &rune);
        }
    }

    /*--- lights ---*/
    GfxLight *L = out->u.lights;
    int nl = 0;
    bool town = world_is_town();
    float hour = (float)snap->game_h + (float)snap->game_m / 60.0f;
    float day = town ? smoothstepf(5.5f, 7.5f, hour) * (1.0f - smoothstepf(18.5f, 20.5f, hour)) : 0.0f;
    if (in->title_mode) day = 0.0f;
    float flick = 0.92f + 0.05f * sinf((float)time * 11.0f) + 0.03f * sinf((float)time * 23.7f);
    if (!in->title_mode) {
        float tr = (float)vr * 1.05f + 2.5f;
        add_light(L, &nl, v3add(pp, v3m(0.2f, 1.8f, 0.2f)), 1.0f, 0.78f, 0.52f, tr, (1.5f - day * 1.0f) * flick);
    }
    GfxLight tmp[GFX_MAX_LIGHTS];
    int nf = fx_lights(tmp, 8);
    for (int i = 0; i < nf && nl < GFX_MAX_LIGHTS; i++) L[nl++] = tmp[i];

    /*--- entities ---*/
    g_nlabels = 0;
    float sc_vr2 = ((float)vr + 1.5f) * ((float)vr + 1.5f);
    int ent_lights_room = 6;
    for (int i = 0; i < CLIENT_MAX_ENTITIES; i++) {
        const Entity *e = &snap->entities[i];
        EntAnim *a = &g_anim[i];
        if (!e->active || e->id == snap->my_entity_id) {
            if (a->init) { tome_anim_reset_slot(i); }
            a->init = false;
            continue;
        }
        if (a->id != e->id) { memset(a, 0, sizeof(*a)); a->id = e->id; }
        step_anim(a, e->x, e->y, dt, 6.0f);
        float dx = a->x - pp.x, dz = a->z - pp.z;
        float d2 = dx * dx + dz * dz;
        if (!in->title_mode && d2 > sc_vr2) continue;
        ModelParams mp;
        memset(&mp, 0, sizeof(mp));
        mp.kind = e->kind;
        mp.seed = e->look_seed ? e->look_seed : (uint32_t)e->id * 2654435761u;
        mp.archetype = e->archetype;
        mp.level = e->level;
        mp.race_id = e->race_id;
        mp.class_id = e->class_id;
        mp.name = e->username;
        mp.walk = a->walk;
        mp.move = a->move;
        mp.attack = a->attack;
        mp.hurt = a->hurt;
        mp.time = (float)time + (float)(i * 7 % 13);
        mp.scale = 0.6f + 0.4f * a->spawn;
        mp.fade = 1.0f;
        mp.is_boss = e->archetype == 5;
        mp.weapon = WPN_SWORD;
        if (e->kind == KIND_PLAYER) { mp.weapon = (e->class_id == 11 || e->class_id == 9) ? WPN_STAFF : WPN_SWORD; mp.cape = true; }
        ModelOut mo;
        v3 pos = v3m(a->x, 0, a->z);
        model_build(&g_dyn, &g_trans, &g_add, pos, a->yaw, &mp, g_cam_right, g_cam_up, &mo);
        if (mo.has_light && ent_lights_room > 0 && nl < GFX_MAX_LIGHTS) { L[nl++] = mo.light; ent_lights_room--; }
        /*blob shadow*/
        Mat sh = matc(MAT_SHADOW, RGBA8(255, 255, 255, 170));
        mesh_decal(&g_trans, a->x, a->z, 0.015f, 0.45f * (mp.is_boss ? 1.4f : 1.0f), 0, &sh);
        /*target/hover ring*/
        if (e->id == in->hover_id || e->id == in->target_id) {
            uint32_t rc = hostile_entity(e) ? RGBA8(255, 70, 50, 255) : RGBA8(255, 210, 90, 255);
            Mat ring = matg(MAT_RUNE, rc, 1.0f);
            mesh_decal(&g_add, a->x, a->z, 0.03f, 0.6f, (float)time * 1.5f, &ring);
        }
        /*label*/
        SceneLabel *l = &g_labels[g_nlabels];
        float sx, sy;
        if (scene_world_to_screen(v3m(a->x, mo.height, a->z), &sx, &sy)) {
            l->id = e->id;
            l->slot = i;
            l->sx = sx;
            l->sy = sy;
            l->dist = sqrtf(d2);
            l->hp = e->hp;
            l->max_hp = e->max_hp > 0 ? e->max_hp : 1;
            l->hp_frac = clampf((float)e->hp / (float)l->max_hp, 0, 1);
            l->kind = e->kind;
            l->level = e->level;
            l->archetype = e->archetype;
            l->is_player = e->is_player;
            l->is_merchant = e->is_merchant;
            l->hostile = hostile_entity(e);
            l->is_boss = mp.is_boss;
            snprintf(l->name, sizeof(l->name), "%s", e->username);
            g_nlabels++;
        }
        /*the floating tome of the martial bookshop (client_tome.c)*/
        if (e->is_merchant && e->shop_spec == SHOP_SPEC_BOOKS_MARTIAL) {
            float tp[3], to[16];
            tome_anim_update(i, e->id, e->floor_id, e->x, e->y, &snap->map[0][0], dt, tp, to);
            m3 r = {{to[0], to[4], to[8], to[1], to[5], to[9], to[2], to[6], to[10]}};
            Mat book = matg(MAT_LEATHER, RGBA8(200, 60, 50, 255), 0.2f);
            mesh_box(&g_dyn, v3m(tp[0], tp[1], tp[2]), v3m(0.22f, 0.05f, 0.17f), r, &book);
            Mat pages = matc(MAT_PAPER, RGBA8(255, 250, 230, 255));
            mesh_box(&g_dyn, v3m(tp[0], tp[1], tp[2]), v3m(0.2f, 0.035f, 0.15f), r, &pages);
            Mat glow = matg(MAT_GLOW, RGBA8(255, 200, 120, 255), 1.0f);
            mesh_billboard(&g_add, v3m(tp[0], tp[1], tp[2]), 0.5f, g_cam_right, g_cam_up, 0, &glow);
        }
    }

    /*--- corpses (death animation) ---*/
    for (int i = 0; i < 32; i++) {
        Corpse *c = &g_corpses[i];
        if (!c->alive) continue;
        c->t += dt;
        if (c->t > 1.6f) { c->alive = false; continue; }
        ModelParams mp;
        memset(&mp, 0, sizeof(mp));
        mp.kind = c->e.kind;
        mp.seed = c->e.look_seed ? c->e.look_seed : (uint32_t)c->e.id * 2654435761u;
        mp.archetype = c->e.archetype;
        mp.name = c->e.username;
        mp.time = (float)time;
        mp.scale = 1.0f;
        mp.is_boss = c->e.archetype == 5;
        mp.hurt = fmaxf(0, 1.0f - c->t * 3.0f);
        mp.fade = clampf(1.4f - c->t, 0, 1);
        mp.weapon = WPN_SWORD;
        ModelOut mo;
        float sink = smoothstepf(0.6f, 1.6f, c->t) * 0.8f;
        model_build(&g_dyn, &g_trans, &g_add, v3m(c->x, -sink, c->z), c->yaw + c->t * 0.3f, &mp, g_cam_right, g_cam_up, &mo);
    }

    /*--- own player ---*/
    if (!in->title_mode && snap->my_entity_id >= 0) {
        ModelParams mp;
        memset(&mp, 0, sizeof(mp));
        mp.kind = KIND_PLAYER;
        mp.seed = 0x9E3779B9u ^ (uint32_t)in->race_id * 31u;
        mp.race_id = in->race_id;
        mp.class_id = in->class_id;
        mp.walk = g_me.walk;
        mp.move = g_me.move;
        mp.attack = g_me.attack;
        mp.hurt = g_me.hurt;
        mp.time = (float)time;
        mp.scale = 1.0f;
        mp.fade = 1.0f;
        mp.weapon = weapon_from_name(in->weapon_name);
        mp.shield = in->shield;
        mp.helm = in->helm;
        mp.cape = in->cape;
        ModelOut mo;
        model_build(&g_dyn, &g_trans, &g_add, pp, g_me.yaw, &mp, g_cam_right, g_cam_up, &mo);
        Mat sh = matc(MAT_SHADOW, RGBA8(255, 255, 255, 190));
        mesh_decal(&g_trans, pp.x, pp.z, 0.015f, 0.45f, 0, &sh);
        Mat ring = matg(MAT_RUNE, RGBA8(90, 200, 255, 140), 1.0f);
        mesh_decal(&g_add, pp.x, pp.z, 0.025f, 0.55f, (float)time * 0.5f, &ring);
    }

    /*--- character-creation preview ---*/
    if (g_preview) {
        ModelParams mp;
        memset(&mp, 0, sizeof(mp));
        mp.kind = KIND_PLAYER;
        mp.seed = 0x9E3779B9u ^ (uint32_t)in->preview_race * 31u;
        mp.race_id = in->preview_race;
        mp.class_id = in->preview_class;
        mp.time = (float)time;
        mp.scale = 1.0f;
        mp.fade = 1.0f;
        mp.walk = (float)time * 2.0f;
        mp.move = 0.0f;
        mp.attack = fmodf((float)time, 4.0f) < 1.0f ? fmodf((float)time, 1.0f) + 0.001f : 0.0f;
        static const int cls_weapon[12] = {WPN_AXE, WPN_SWORD, WPN_MACE, WPN_STAFF, WPN_SWORD, WPN_NONE,
                                           WPN_SWORD, WPN_BOW, WPN_DAGGER, WPN_STAFF, WPN_STAFF, WPN_STAFF};
        mp.weapon = (in->preview_class >= 0 && in->preview_class < 12) ? cls_weapon[in->preview_class] : WPN_SWORD;
        mp.shield = in->preview_class == 2 || in->preview_class == 6;
        mp.cape = true;
        v3 side = v3scale(g_cam_right, in->preview_offset_x);
        v3 mpos = v3add(pp, side);
        ModelOut mo;
        float spin = (float)time * 0.6f;
        model_build(&g_dyn, &g_trans, &g_add, mpos, cam_yaw + 0.4f + sinf(spin) * 0.6f, &mp, g_cam_right, g_cam_up, &mo);
        Mat ring = matg(MAT_RUNE, RGBA8(255, 200, 90, 255), 1.0f);
        mesh_decal(&g_add, mpos.x, mpos.z, 0.03f, 0.9f, (float)time * 0.4f, &ring);
        add_light(L, &nl, v3add(mpos, v3add(v3m(0, 2.2f, 0), v3scale(g_cam_right, 0.8f))), 1.0f, 0.85f, 0.65f, 6.0f, 2.2f);
        add_light(L, &nl, v3add(mpos, v3m(0, 1.0f, 0)), 0.5f, 0.6f, 1.0f, 3.0f, 0.8f);
    }

    /*--- beacon over the stairs down: a beam of light visible from afar
     * (through the fog of war) so the way onward is never invisible ---*/
    if (!in->title_mode) {
        int r = 46;
        int bxs = -1, bys = -1;
        float bd = 1e9f;
        for (int y = snap->my_y - r; y <= snap->my_y + r; y++) {
            if (y < 0 || y >= MAP_HEIGHT) continue;
            for (int x = snap->my_x - r; x <= snap->my_x + r; x++) {
                if (x < 0 || x >= MAP_WIDTH || snap->map[y][x] != VOXEL_STAIRS_DOWN) continue;
                float d = (float)((x - snap->my_x) * (x - snap->my_x) + (y - snap->my_y) * (y - snap->my_y));
                if (d < bd) { bd = d; bxs = x; bys = y; }
            }
        }
        if (bxs >= 0 && bd > 4.0f) {
            v3 base = v3m((float)bxs, 0, (float)bys);
            v3 to_cam = v3sub(g_eye, base);
            to_cam.y = 0;
            to_cam = v3norm(to_cam);
            v3 side = v3m(to_cam.z, 0, -to_cam.x);
            float pulse = 0.75f + 0.25f * sinf((float)time * 1.7f);
            for (int k = 0; k < 2; k++) {
                float wdt = k == 0 ? 0.7f : 0.22f;
                float hgt = 14.0f;
                Mat beam = matg(MAT_GLOW, RGBA8(255, 255, 255, 255), 1.0f);
                beam.flags = SV_UNLIT;
                uint32_t cb = k == 0 ? RGBA8((int)(230 * pulse), (int)(170 * pulse), 50, 255) : RGBA8(255, 240, 190, 255);
                uint32_t ct = cb & 0x00FFFFFFu;
                uint32_t cols[4] = {cb, cb, ct, ct};
                v3 a = v3sub(base, v3scale(side, wdt)), b = v3add(base, v3scale(side, wdt));
                mesh_quad_c(&g_add, a, b, v3add(b, v3m(0, hgt, 0)), v3add(a, v3m(0, hgt, 0)), to_cam, &beam,
                            0.0f, 0.5f, 1.0f, 0.5f, cols, NULL);
            }
        }
    }

    /*--- path & hover markers ---*/
    for (int i = 0; i < in->path_len; i++) {
        float x = (float)in->path[i * 2], z = (float)in->path[i * 2 + 1];
        float pulse = 0.6f + 0.4f * sinf((float)time * 6.0f - (float)i * 0.6f);
        Mat dot = matg(MAT_GLOW, col_scale(RGBA8(255, 210, 120, 255), pulse), 1.0f);
        mesh_decal(&g_add, x, z, 0.04f, i == in->path_len - 1 ? 0.35f : 0.14f, 0, &dot);
    }
    if (in->hover_tx >= 0 && in->hover_id == 0) {
        Mat sq = matg(MAT_GLOW, RGBA8(120, 110, 90, 255), 1.0f);
        mesh_decal(&g_add, (float)in->hover_tx, (float)in->hover_ty, 0.03f, 0.42f, 0, &sq);
    }

    /*--- particles ---*/
    fx_build(&g_add, &g_trans, g_cam_right, g_cam_up, time, in->quality);

    /*--- world lights (nearest) ---*/
    int room = GFX_MAX_LIGHTS - nl;
    if (in->quality == 0 && room > 8) room = 8;
    if (in->quality == 1 && room > 16) room = 16;
    nl += world_collect_lights(L + nl, room, pp.x, pp.z, (float)vr + 8.0f);

    /*--- uniforms ---*/
    SceneUniforms *u = &out->u;
    memcpy(u->viewproj, g_viewproj.m, sizeof(u->viewproj));
    u->cam_pos[0] = g_eye.x; u->cam_pos[1] = g_eye.y; u->cam_pos[2] = g_eye.z; u->cam_pos[3] = (float)fmod(time, 10000.0);
    u->player_pos[0] = pp.x; u->player_pos[1] = 0; u->player_pos[2] = pp.z;
    u->player_pos[3] = in->title_mode ? 1000.0f : (float)vr + 0.5f;
    float amb_day[3] = {0.78f, 0.76f, 0.70f}, amb_night[3] = {0.10f, 0.12f, 0.22f};
    float amb[3];
    if (town) {
        float sunset = smoothstepf(16.5f, 19.0f, hour) * (1.0f - smoothstepf(19.5f, 21.0f, hour));
        for (int k = 0; k < 3; k++) amb[k] = lerpf(amb_night[k], amb_day[k], day);
        amb[0] += sunset * 0.25f; amb[1] += sunset * 0.08f;
    } else {
        /*deeper floors are darker and colder*/
        float depth = clampf((float)snap->my_floor / 100.0f, 0, 1);
        amb[0] = 0.17f - depth * 0.06f; amb[1] = 0.155f - depth * 0.05f; amb[2] = 0.21f - depth * 0.04f;
    }
    if (in->title_mode) { amb[0] = 0.20f; amb[1] = 0.19f; amb[2] = 0.27f; }
    if (in->title_mode && g_gallery) { amb[0] = 0.85f; amb[1] = 0.82f; amb[2] = 0.8f; }
    float br = in->brightness > 0 ? in->brightness : 1.0f;
    u->ambient[0] = amb[0] * br; u->ambient[1] = amb[1] * br; u->ambient[2] = amb[2] * br;
    u->ambient[3] = town ? 0.45f : 0.62f; /*memory brightness (explored, out of sight)*/
    u->fog_color[0] = 0.015f; u->fog_color[1] = 0.012f; u->fog_color[2] = 0.025f; u->fog_color[3] = 0.012f;
    if (town && day > 0) { u->fog_color[0] = 0.35f * day; u->fog_color[1] = 0.38f * day; u->fog_color[2] = 0.45f * day; }
    float fwdx = -sinf(cam_yaw), fwdz = -cosf(cam_yaw);
    u->cut_dir[0] = fwdx; u->cut_dir[1] = fwdz; u->cut_dir[2] = 0; u->cut_dir[3] = (in->title_mode && !g_preview) ? 0.0f : 1.0f;
    u->misc[0] = (float)nl;
    u->misc[1] = (float)MAP_WIDTH;
    u->misc[2] = 1.0f;
    u->misc[3] = br;
    out->explored.data = &g_explored[0][0];
    out->explored.w = MAP_WIDTH;
    out->explored.h = MAP_HEIGHT;
    out->explored.version = in->title_mode ? 0xFFFFFFFFu : g_explored_version;
    if (in->title_mode) {
        if (!g_title_filled) { memset(g_explored, 255, sizeof(g_explored)); g_title_filled = true; g_explored_version++; }
        out->explored.version = g_explored_version;
    }
    out->clear_color[0] = u->fog_color[0];
    out->clear_color[1] = u->fog_color[1];
    out->clear_color[2] = u->fog_color[2];
    out->clear_color[3] = 1;
    out->vignette = 0.45f;

    out->dyn = g_dyn.v; out->dyn_count = g_dyn.n;
    out->trans = g_trans.v; out->trans_count = g_trans.n;
    out->add = g_add.v; out->add_count = g_add.n;
}
