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

/*Procedural, animated creature models.
 *
 * Every model is assembled each frame from boxes/prisms around a local
 * "rig" (position + facing + scale). Limbs swing with the walk cycle,
 * weapons arc through the attack animation, hits flash the whole body.
 * Colours derive from a stable per-template seed, so all "Cave Goblins"
 * look alike while two different creatures never do.
 *
 * Local frame: +Z forward (the facing direction), +Y up, +X right.*/

#include "scene_internal.h"
#include "textures.h"
#include "protocol.h"
#include "species.h"
#include "classes.h"
#include <string.h>
#include <strings.h>
#include <ctype.h>

typedef struct {
    Mesh *m;      /*opaque target*/
    Mesh *t;      /*translucent target*/
    Mesh *a;      /*additive target*/
    v3 pos;
    m3 R;         /*facing*/
    float s;      /*scale*/
    float flash;  /*hurt flash*/
    float fade;   /*1 = opaque*/
    v3 cr, cu;    /*camera axes for sprites*/
} Rig;

static Mat rmat(const Rig *g, int layer, uint32_t col, float glow) {
    Mat m = matg(layer, col, glow);
    if (g->flash > 0) {
        m.col = col_mix(col, RGBA8(255, 90, 70, 255), g->flash * 0.75f);
        m.glow = fmaxf(glow, g->flash * 0.5f);
    }
    m.uvs = 2.0f;
    return m;
}

static Mesh *rtarget(const Rig *g) { return g->fade < 0.999f ? g->t : g->m; }

static Mat rfade(const Rig *g, Mat m) {
    if (g->fade < 0.999f) {
        uint32_t a = (uint32_t)(clampf(g->fade, 0, 1) * 255.0f);
        m.col = (m.col & 0x00FFFFFFu) | (a << 24);
    }
    return m;
}

/*local -> world*/
static v3 rw(const Rig *g, v3 l) { return v3add(g->pos, m3apply(g->R, v3scale(l, g->s))); }

static void rbox(const Rig *g, v3 c, v3 h, m3 r, Mat mt) {
    mt = rfade(g, mt);
    mesh_box(rtarget(g), rw(g, c), v3scale(h, g->s), m3mul(g->R, r), &mt);
}

static void rlimb(const Rig *g, v3 pivot, float hx, float len, float hz, m3 r, Mat mt) {
    mt = rfade(g, mt);
    mesh_limb(rtarget(g), rw(g, pivot), hx * g->s, len * g->s, hz * g->s, m3mul(g->R, r), &mt);
}

static void rprism(const Rig *g, v3 b, float rb, float rt, float h, int sides, m3 r, Mat mt) {
    mt = rfade(g, mt);
    mesh_prism(rtarget(g), rw(g, b), rb * g->s, rt * g->s, h * g->s, sides, m3mul(g->R, r), &mt, true);
}

/*Tip of a limb in local space (for attaching hands/weapons).*/
static v3 limb_end(v3 pivot, float len, m3 r) { return v3add(pivot, m3apply(r, v3m(0, -len, 0))); }

static void rglow_sprite(const Rig *g, v3 lp, float size, uint32_t col) {
    Mat mt = matg(MAT_GLOW, col, 1.0f);
    mesh_billboard(g->a, rw(g, lp), size * g->s, g->cr, g->cu, 0, &mt);
}

static void reyes(const Rig *g, v3 head_c, float spread, float size, uint32_t col, float glow) {
    Mat e = rmat(g, MAT_WHITE, col, glow);
    rbox(g, v3add(head_c, v3m(-spread, 0, 0)), v3m(size, size, size * 0.5f), m3identity(), e);
    rbox(g, v3add(head_c, v3m(spread, 0, 0)), v3m(size, size, size * 0.5f), m3identity(), e);
    if (glow > 0.5f) {
        rglow_sprite(g, v3add(head_c, v3m(-spread, 0, 0.02f)), size * 4, col_scale(col, 0.6f));
        rglow_sprite(g, v3add(head_c, v3m(spread, 0, 0.02f)), size * 4, col_scale(col, 0.6f));
    }
}

/* ------------------------------------------------------------------ */
/* Colours                                                             */
/* ------------------------------------------------------------------ */

static uint32_t seed_col(uint32_t seed, int k, float h0, float hspan, float s, float v) {
    float h = h0 + hspan * hash1f(seed * 2654435761u + (uint32_t)k * 97u);
    float vv = v * (0.85f + 0.3f * hash1f(seed + (uint32_t)k * 131u));
    return col_hsv(h, s, clampf(vv, 0, 1), 255);
}

static uint32_t class_color(int cls) {
    static const uint32_t C[CLASS_COUNT] = {
        [CLASS_BARBARIAN] = RGBA8(150, 70, 40, 255), [CLASS_BARD] = RGBA8(180, 60, 140, 255),
        [CLASS_CLERIC] = RGBA8(230, 225, 210, 255),  [CLASS_DRUID] = RGBA8(70, 130, 60, 255),
        [CLASS_FIGHTER] = RGBA8(160, 40, 40, 255),   [CLASS_MONK] = RGBA8(230, 150, 40, 255),
        [CLASS_PALADIN] = RGBA8(60, 90, 190, 255),   [CLASS_RANGER] = RGBA8(60, 100, 50, 255),
        [CLASS_ROGUE] = RGBA8(55, 55, 65, 255),      [CLASS_SORCERER] = RGBA8(170, 40, 60, 255),
        [CLASS_WARLOCK] = RGBA8(90, 40, 120, 255),   [CLASS_WIZARD] = RGBA8(40, 60, 160, 255),
    };
    if (cls < 0 || cls >= CLASS_COUNT) return RGBA8(120, 120, 130, 255);
    return C[cls];
}

int weapon_from_name(const char *n) {
    if (!n || !n[0]) return WPN_NONE;
    static const struct { const char *kw; int w; } T[] = {
        {"bow", WPN_BOW}, {"crossbow", WPN_BOW}, {"staff", WPN_STAFF}, {"wand", WPN_STAFF},
        {"rod", WPN_STAFF}, {"axe", WPN_AXE}, {"halberd", WPN_AXE}, {"mace", WPN_MACE},
        {"hammer", WPN_MACE}, {"flail", WPN_MACE}, {"morning", WPN_MACE}, {"dagger", WPN_DAGGER},
        {"knife", WPN_DAGGER}, {"sickle", WPN_DAGGER}, {"spear", WPN_SPEAR}, {"pike", WPN_SPEAR},
        {"trident", WPN_SPEAR}, {"lance", WPN_SPEAR}, {"club", WPN_CLUB}, {"cudgel", WPN_CLUB},
        {"sword", WPN_SWORD}, {"blade", WPN_SWORD}, {"rapier", WPN_SWORD}, {"scimitar", WPN_SWORD},
        {"katana", WPN_SWORD}, {"saber", WPN_SWORD},
    };
    for (size_t i = 0; i < sizeof(T) / sizeof(T[0]); i++)
        if (strcasestr(n, T[i].kw)) return T[i].w;
    if (strcasestr(n, "bare") || strcasestr(n, "unarmed")) return WPN_NONE;
    return WPN_SWORD;
}

/* ------------------------------------------------------------------ */
/* Weapons                                                             */
/* ------------------------------------------------------------------ */

/*Draw a weapon held at hand position `h` with orientation r (local
 * frame of the arm: the blade points along -Y of r rotated forward).*/
static void weapon(const Rig *g, int w, v3 hand, m3 arm) {
    m3 r = m3mul(arm, m3rotx(-1.5708f)); /*point the weapon forward along the arm's end*/
    Mat steel = rmat(g, MAT_METAL, RGBA8(210, 214, 222, 255), 0.05f);
    Mat wood = rmat(g, MAT_PLANK, RGBA8(140, 96, 56, 255), 0);
    Mat gold = rmat(g, MAT_METAL, RGBA8(230, 190, 80, 255), 0.1f);
    v3 up = m3apply(r, v3m(0, 1, 0));
    switch (w) {
    case WPN_SWORD:
        rbox(g, v3add(hand, v3scale(up, 0.40f)), v3m(0.03f, 0.34f, 0.012f), r, steel);
        rbox(g, v3add(hand, v3scale(up, 0.05f)), v3m(0.11f, 0.02f, 0.03f), r, gold);
        rbox(g, v3add(hand, v3scale(up, -0.06f)), v3m(0.02f, 0.07f, 0.02f), r, wood);
        break;
    case WPN_DAGGER:
        rbox(g, v3add(hand, v3scale(up, 0.18f)), v3m(0.022f, 0.14f, 0.01f), r, steel);
        rbox(g, v3add(hand, v3scale(up, 0.03f)), v3m(0.06f, 0.015f, 0.025f), r, gold);
        break;
    case WPN_AXE:
        rbox(g, v3add(hand, v3scale(up, 0.25f)), v3m(0.02f, 0.35f, 0.02f), r, wood);
        rbox(g, v3add(hand, v3add(v3scale(up, 0.5f), m3apply(r, v3m(0.09f, 0, 0)))), v3m(0.09f, 0.1f, 0.012f), r, steel);
        break;
    case WPN_MACE:
    case WPN_CLUB:
        rbox(g, v3add(hand, v3scale(up, 0.22f)), v3m(0.022f, 0.28f, 0.022f), r, wood);
        rbox(g, v3add(hand, v3scale(up, 0.52f)), v3m(0.07f, 0.08f, 0.07f), r, w == WPN_MACE ? steel : wood);
        break;
    case WPN_STAFF: {
        rbox(g, v3add(hand, v3scale(up, 0.25f)), v3m(0.02f, 0.75f, 0.02f), r, wood);
        Mat orb = rmat(g, MAT_CRYSTAL, RGBA8(120, 200, 255, 255), 1.0f);
        v3 tip = v3add(hand, v3scale(up, 1.02f));
        rbox(g, tip, v3m(0.06f, 0.06f, 0.06f), m3mul(r, m3rotz(0.78f)), orb);
        rglow_sprite(g, tip, 0.25f, RGBA8(90, 160, 255, 255));
        break;
    }
    case WPN_SPEAR:
        rbox(g, v3add(hand, v3scale(up, 0.35f)), v3m(0.018f, 0.75f, 0.018f), r, wood);
        rbox(g, v3add(hand, v3scale(up, 1.15f)), v3m(0.035f, 0.08f, 0.01f), r, steel);
        break;
    case WPN_BOW: {
        m3 rb = arm;
        for (int i = -2; i <= 2; i++) {
            float a = (float)i * 0.32f;
            v3 c = v3add(hand, m3apply(rb, v3m(0, sinf(a) * 0.32f, cosf(a) * 0.1f - 0.08f)));
            rbox(g, c, v3m(0.018f, 0.07f, 0.018f), m3mul(rb, m3rotx(a)), wood);
        }
        break;
    }
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Humanoid                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    float height, width;
    uint32_t skin, cloth, cloth2, hair, metal;
    int skin_mat, cloth_mat;
    bool horns, big_horns, tail, ears_pointy, ears_cat, beak, snout, shell, halo, wings, fairy_wings;
    bool skeleton, hood, hat, beard, glow_eyes, apron, crown, robe;
    uint32_t eye_col;
    int weapon;
    bool shield, helm, cape;
    float hover;
} HumanoidLook;

static void humanoid(const Rig *g0, const ModelParams *p, const HumanoidLook *L, ModelOut *out) {
    Rig rg = *g0;
    Rig *g = &rg;
    float H = L->height, W = L->width;
    float t = p->time;
    float sw = sinf(p->walk) * p->move;
    float breathe = sinf(t * 2.0f + (float)(p->seed & 63)) * 0.012f;
    float bob = fabsf(cosf(p->walk)) * 0.05f * p->move;
    float hover = L->hover > 0 ? (L->hover + sinf(t * 2.2f) * 0.06f) : 0;
    float y0 = bob + hover;

    float leg = 0.42f * H, torso = 0.40f * H, headh = 0.17f * H;
    float hip_y = y0 + leg;
    float sh_y = hip_y + torso;
    int skm = L->skin_mat, clm = L->cloth_mat;
    Mat skin = rmat(g, skm, L->skin, 0);
    Mat cloth = rmat(g, clm, L->cloth, 0);
    Mat cloth2 = rmat(g, clm, L->cloth2, 0);
    Mat boot = rmat(g, MAT_LEATHER, col_scale(L->cloth2, 0.6f), 0);

    /*legs*/
    for (int s = -1; s <= 1; s += 2) {
        float a = sw * 0.7f * (float)s;
        m3 r = m3rotx(a);
        v3 pv = v3m(0.09f * W * (float)s, hip_y, 0);
        if (L->skeleton) {
            rlimb(g, pv, 0.03f, leg * 0.98f, 0.03f, r, skin);
        } else if (L->robe) {
            /*robe hides legs: just feet*/
        } else {
            rlimb(g, pv, 0.07f * W, leg * 0.55f, 0.07f, r, cloth2);
            v3 knee = limb_end(pv, leg * 0.55f, r);
            rlimb(g, knee, 0.065f * W, leg * 0.45f, 0.065f, m3rotx(a * 0.5f - fabsf(sw) * 0.3f), boot);
        }
    }
    /*torso / robe*/
    if (L->skeleton) {
        Mat bone = skin;
        rbox(g, v3m(0, hip_y + torso * 0.5f, 0), v3m(0.025f, torso * 0.5f, 0.025f), m3identity(), bone);
        for (int i = 0; i < 4; i++)
            rbox(g, v3m(0, hip_y + torso * (0.45f + 0.13f * (float)i), 0.02f), v3m(0.15f * W, 0.018f, 0.09f), m3identity(), bone);
        rbox(g, v3m(0, hip_y + 0.02f, 0), v3m(0.14f * W, 0.04f, 0.07f), m3identity(), bone);
    } else if (L->robe) {
        rprism(g, v3m(0, y0, 0), 0.26f * W, 0.17f * W, leg + torso * 0.55f, 8, m3identity(), cloth);
        rbox(g, v3m(0, hip_y + torso * 0.72f + breathe, 0), v3m(0.18f * W, torso * 0.3f, 0.12f), m3identity(), cloth);
    } else {
        rbox(g, v3m(0, hip_y + torso * 0.5f + breathe, 0), v3m(0.19f * W, torso * 0.5f, 0.12f), m3identity(), cloth);
        /*belt*/
        rbox(g, v3m(0, hip_y + 0.04f, 0), v3m(0.2f * W, 0.035f, 0.13f), m3identity(), rmat(g, MAT_LEATHER, RGBA8(90, 60, 35, 255), 0));
        rbox(g, v3m(0, hip_y + 0.04f, 0.13f), v3m(0.035f, 0.03f, 0.01f), m3identity(), rmat(g, MAT_METAL, RGBA8(220, 190, 90, 255), 0));
    }
    if (L->apron)
        rbox(g, v3m(0, hip_y + torso * 0.25f, 0.13f), v3m(0.16f * W, torso * 0.42f, 0.01f), m3identity(), rmat(g, MAT_CLOTH, RGBA8(200, 190, 160, 255), 0));
    if (L->shell)
        rbox(g, v3m(0, hip_y + torso * 0.55f, -0.16f), v3m(0.24f * W, torso * 0.5f, 0.08f), m3identity(), rmat(g, MAT_SCALES, RGBA8(110, 130, 70, 255), 0));
    /*cape*/
    if (L->cape) {
        float flap = 0.15f + 0.25f * p->move + sinf(t * 3.0f) * 0.04f;
        m3 r = m3rotx(flap);
        rlimb(g, v3m(0, sh_y - 0.02f, -0.13f), 0.19f * W, torso + leg * 0.6f, 0.015f, r, rmat(g, MAT_CLOTH, col_scale(L->cloth, 0.7f), 0));
    }
    /*arms*/
    float arm_len = 0.40f * H;
    v3 hand_r = v3m(0, 0, 0);
    m3 arm_r = m3identity();
    for (int s = -1; s <= 1; s += 2) {
        float a = -sw * 0.6f * (float)s;
        m3 r = m3mul(m3rotz(0.12f * (float)s), m3rotx(a));
        if (s == 1 && p->attack > 0) {
            /*right arm: wind up then strike*/
            float k = p->attack;
            float ang = k < 0.35f ? lerpf(0, -2.6f, k / 0.35f) : lerpf(-2.6f, 0.9f, clampf((k - 0.35f) / 0.3f, 0, 1));
            if (k > 0.65f) ang = lerpf(0.9f, 0.0f, (k - 0.65f) / 0.35f);
            r = m3mul(m3rotz(0.25f), m3rotx(ang));
        } else if (s == 1 && L->weapon == WPN_STAFF) {
            r = m3mul(m3rotz(0.15f), m3rotx(-0.5f));
        } else if (s == -1 && L->shield) {
            r = m3mul(m3rotz(-0.15f), m3rotx(-0.9f));
        }
        v3 pv = v3m((0.22f * W + 0.02f) * (float)s, sh_y - 0.03f, 0);
        if (L->skeleton) {
            rlimb(g, pv, 0.025f, arm_len, 0.025f, r, skin);
        } else {
            rlimb(g, pv, 0.06f, arm_len * 0.62f, 0.06f, r, cloth);
            v3 elbow = limb_end(pv, arm_len * 0.62f, r);
            rlimb(g, elbow, 0.05f, arm_len * 0.38f, 0.05f, r, skin);
        }
        v3 hand = limb_end(pv, arm_len, r);
        if (s == 1) { hand_r = hand; arm_r = r; }
        if (s == -1 && L->shield) {
            Mat sh = rmat(g, MAT_METAL, RGBA8(170, 150, 120, 255), 0);
            rbox(g, v3add(hand, m3apply(r, v3m(0, 0.08f, 0.06f))), v3m(0.17f, 0.2f, 0.02f), r, sh);
            rbox(g, v3add(hand, m3apply(r, v3m(0, 0.08f, 0.08f))), v3m(0.05f, 0.05f, 0.01f), r, rmat(g, MAT_METAL, RGBA8(230, 190, 80, 255), 0));
        }
    }
    if (L->weapon != WPN_NONE) weapon(g, L->weapon, hand_r, arm_r);

    /*head*/
    float head_y = sh_y + 0.04f + headh * 0.55f + breathe;
    v3 hc = v3m(0, head_y, 0.01f);
    float hw = 0.13f * (L->skeleton ? 0.9f : 1.0f);
    rbox(g, hc, v3m(hw, headh * 0.55f, hw), m3identity(), skin);
    if (L->snout)
        rbox(g, v3add(hc, v3m(0, -0.03f, hw + 0.06f)), v3m(0.07f, 0.06f, 0.08f), m3identity(), skin);
    if (L->beak)
        rprism(g, v3add(hc, v3m(0, -0.01f, hw)), 0.05f, 0.0f, 0.14f, 4, m3rotx(1.5708f), rmat(g, MAT_BONE, RGBA8(230, 180, 60, 255), 0));
    if (L->beard)
        rbox(g, v3add(hc, v3m(0, -headh * 0.45f, hw * 0.8f)), v3m(hw * 0.8f, headh * 0.35f, 0.05f), m3identity(), rmat(g, MAT_FUR, L->hair, 0));
    /*hair / hood / hat / helm*/
    if (L->helm) {
        Mat hm = rmat(g, MAT_METAL, RGBA8(190, 190, 200, 255), 0.05f);
        rbox(g, v3add(hc, v3m(0, headh * 0.25f, -0.01f)), v3m(hw + 0.025f, headh * 0.38f, hw + 0.025f), m3identity(), hm);
        rbox(g, v3add(hc, v3m(0, headh * 0.25f + 0.05f, hw + 0.03f)), v3m(0.02f, headh * 0.3f, 0.01f), m3identity(), hm);
    } else if (L->hood) {
        rbox(g, v3add(hc, v3m(0, headh * 0.15f, -0.02f)), v3m(hw + 0.03f, headh * 0.62f, hw + 0.02f), m3identity(), cloth2);
    } else if (L->hat) {
        rbox(g, v3add(hc, v3m(0, headh * 0.55f, 0)), v3m(hw + 0.09f, 0.015f, hw + 0.09f), m3identity(), cloth2);
        rprism(g, v3add(hc, v3m(0, headh * 0.55f, 0)), hw + 0.01f, 0.0f, 0.34f, 6, m3rotx(-0.15f), cloth2);
    } else if (!L->skeleton && L->hair) {
        rbox(g, v3add(hc, v3m(0, headh * 0.42f, -0.02f)), v3m(hw + 0.015f, headh * 0.2f, hw + 0.01f), m3identity(), rmat(g, MAT_FUR, L->hair, 0));
    }
    if (L->crown) {
        Mat gold = rmat(g, MAT_METAL, RGBA8(240, 200, 70, 255), 0.3f);
        for (int i = 0; i < 5; i++) {
            float a = (float)i / 5.0f * MX_TAU;
            rbox(g, v3add(hc, v3m(cosf(a) * hw, headh * 0.62f, sinf(a) * hw)), v3m(0.025f, 0.05f, 0.025f), m3identity(), gold);
        }
    }
    if (L->horns || L->big_horns) {
        float hs = L->big_horns ? 1.8f : 1.0f;
        Mat horn = rmat(g, MAT_BONE, RGBA8(70, 60, 55, 255), 0);
        for (int s = -1; s <= 1; s += 2)
            rprism(g, v3add(hc, v3m(hw * 0.7f * (float)s, headh * 0.4f, 0)), 0.035f * hs, 0.0f, 0.18f * hs, 5,
                   m3rotz(-0.5f * (float)s * (L->big_horns ? 1.6f : 1.0f)), horn);
    }
    if (L->ears_pointy)
        for (int s = -1; s <= 1; s += 2)
            rbox(g, v3add(hc, v3m((hw + 0.02f) * (float)s, 0.02f, -0.02f)), v3m(0.02f, 0.03f, 0.05f), m3rotx(-0.6f), skin);
    if (L->ears_cat)
        for (int s = -1; s <= 1; s += 2)
            rprism(g, v3add(hc, v3m(hw * 0.6f * (float)s, headh * 0.5f, 0)), 0.05f, 0.0f, 0.09f, 3, m3identity(), skin);
    reyes(g, v3add(hc, v3m(0, 0.01f, hw + 0.002f)), 0.05f, 0.018f, L->eye_col, L->glow_eyes ? 1.0f : 0.0f);
    if (L->halo) {
        float hy = head_y + headh * 0.9f + sinf(t * 2) * 0.02f;
        Mat ring = rmat(g, MAT_METAL, RGBA8(255, 230, 120, 255), 1.0f);
        for (int i = 0; i < 10; i++) {
            float a = (float)i / 10.0f * MX_TAU;
            rbox(g, v3m(cosf(a) * 0.16f, hy, sinf(a) * 0.16f), v3m(0.04f, 0.012f, 0.012f), m3roty(-a + 1.5708f), ring);
        }
        rglow_sprite(g, v3m(0, hy, 0), 0.5f, RGBA8(255, 220, 120, 255));
    }
    if (L->tail) {
        float wag = sinf(t * 3.0f) * 0.4f;
        rlimb(g, v3m(0, hip_y, -0.12f), 0.03f, 0.45f * H, 0.03f, m3mul(m3roty(wag), m3rotx(-2.2f)), skin);
    }
    if (L->wings || L->fairy_wings) {
        float flap = sinf(t * (L->fairy_wings ? 14.0f : 5.0f)) * 0.5f;
        uint32_t wc = L->fairy_wings ? RGBA8(200, 230, 255, 160) : (L->halo ? RGBA8(250, 248, 240, 255) : RGBA8(70, 30, 30, 255));
        for (int s = -1; s <= 1; s += 2) {
            m3 r = m3mul(m3roty((0.5f + flap) * (float)s), m3rotz(-0.4f * (float)s));
            v3 root = v3m(0.08f * (float)s, sh_y - 0.05f, -0.13f);
            Mat wm = rmat(g, L->fairy_wings ? MAT_CRYSTAL : MAT_LEATHER, wc, L->fairy_wings ? 0.6f : 0);
            rbox(g, v3add(root, m3apply(r, v3m(0.3f * (float)s * H, 0.12f, 0))), v3m(0.3f * H, 0.2f * H, 0.01f), r, wm);
        }
    }
    out->height = (head_y + headh) * g->s + 0.1f;
}

/* ------------------------------------------------------------------ */
/* Other bodies                                                        */
/* ------------------------------------------------------------------ */

static void quadruped(const Rig *g, const ModelParams *p, uint32_t body, int mat, float len, float hgt,
                      bool long_neck, bool horns, ModelOut *out) {
    float t = p->time, sw = sinf(p->walk * 1.2f) * p->move;
    float leg = hgt * 0.5f;
    float by = leg + hgt * 0.22f + fabsf(sinf(p->walk * 1.2f)) * 0.03f * p->move;
    Mat m = rmat(g, mat, body, 0);
    Mat dark = rmat(g, mat, col_scale(body, 0.7f), 0);
    rbox(g, v3m(0, by, 0), v3m(0.2f * hgt + 0.05f, hgt * 0.22f, len * 0.5f), m3identity(), m);
    for (int i = 0; i < 4; i++) {
        float sx = (i & 1) ? 1.0f : -1.0f, sz = (i & 2) ? 1.0f : -1.0f;
        float a = sw * 0.7f * ((i == 0 || i == 3) ? 1.0f : -1.0f);
        rlimb(g, v3m(sx * (0.12f * hgt + 0.03f), by - 0.05f, sz * len * 0.38f), 0.05f * hgt + 0.01f, leg + 0.05f, 0.05f * hgt + 0.01f, m3rotx(a), dark);
    }
    float neck_up = long_neck ? 0.35f : 0.1f;
    float lunge = p->attack > 0 ? sinf(p->attack * MX_PI) * 0.25f : 0;
    v3 head = v3m(0, by + neck_up * hgt + 0.05f, len * 0.5f + 0.12f * hgt + lunge);
    if (long_neck) rbox(g, v3m(0, by + neck_up * hgt * 0.5f, len * 0.45f), v3m(0.07f * hgt, neck_up * hgt * 0.6f, 0.07f * hgt), m3rotx(0.5f), m);
    rbox(g, head, v3m(0.12f * hgt + 0.02f, 0.11f * hgt + 0.02f, 0.14f * hgt + 0.02f), m3rotx(p->attack > 0 ? -0.3f : 0.1f), m);
    rbox(g, v3add(head, v3m(0, -0.03f, 0.15f * hgt)), v3m(0.07f * hgt, 0.06f * hgt, 0.09f * hgt), m3identity(), dark);
    for (int s = -1; s <= 1; s += 2)
        rprism(g, v3add(head, v3m(0.08f * hgt * (float)s, 0.1f * hgt, -0.04f)), 0.04f * hgt, 0.0f, (horns ? 0.25f : 0.1f) * hgt, 4,
               m3rotz(-0.35f * (float)s), horns ? rmat(g, MAT_BONE, RGBA8(220, 210, 180, 255), 0) : m);
    reyes(g, v3add(head, v3m(0, 0.03f * hgt, 0.14f * hgt + 0.02f)), 0.06f * hgt, 0.018f, RGBA8(255, 200, 60, 255), 0.6f);
    float wag = sinf(t * 4.0f) * 0.5f;
    rlimb(g, v3m(0, by + 0.05f, -len * 0.5f), 0.03f * hgt + 0.01f, 0.35f * hgt, 0.03f * hgt + 0.01f, m3mul(m3roty(wag), m3rotx(-2.4f)), m);
    out->height = (by + neck_up * hgt + 0.3f * hgt) * g->s + 0.1f;
}

static void spider(const Rig *g, const ModelParams *p, uint32_t body, ModelOut *out) {
    float t = p->time;
    float by = 0.32f + sinf(p->walk * 2.0f) * 0.02f * p->move;
    Mat m = rmat(g, MAT_FUR, body, 0);
    Mat dark = rmat(g, MAT_SCALES, col_scale(body, 0.6f), 0);
    rbox(g, v3m(0, by + 0.06f, -0.22f), v3m(0.2f, 0.17f, 0.24f), m3rotx(0.3f), m);
    rbox(g, v3m(0, by, 0.1f), v3m(0.13f, 0.09f, 0.13f), m3identity(), dark);
    for (int s = -1; s <= 1; s += 2)
        for (int i = 0; i < 4; i++) {
            float ph = p->walk * 2.0f + (float)i * 1.3f + (s > 0 ? MX_PI : 0);
            float lift = sinf(ph) * 0.35f * p->move;
            float spread = -0.6f + (float)i * 0.4f;
            m3 r1 = m3mul(m3roty(spread * (float)s), m3rotz((-1.0f - lift) * (float)s));
            v3 root = v3m(0.1f * (float)s, by, 0.12f - (float)i * 0.06f);
            rlimb(g, root, 0.018f, 0.32f, 0.018f, r1, dark);
            v3 knee = limb_end(root, 0.32f, r1);
            m3 r2 = m3mul(m3roty(spread * (float)s), m3rotz(0.6f * (float)s));
            rlimb(g, knee, 0.015f, 0.38f, 0.015f, r2, dark);
        }
    for (int i = 0; i < 4; i++)
        reyes(g, v3m(0, by + 0.03f + (float)(i / 2) * 0.03f, 0.23f), 0.025f + 0.02f * (float)(i % 2), 0.012f, RGBA8(255, 40, 40, 255), 1.0f);
    float fang = p->attack > 0 ? sinf(p->attack * MX_PI) * 0.6f : sinf(t * 3) * 0.1f;
    for (int s = -1; s <= 1; s += 2)
        rlimb(g, v3m(0.04f * (float)s, by - 0.04f, 0.22f), 0.012f, 0.08f, 0.012f, m3rotx(0.3f + fang), rmat(g, MAT_BONE, RGBA8(230, 230, 210, 255), 0));
    out->height = (by + 0.35f) * g->s + 0.1f;
}

static void dragon(const Rig *g, const ModelParams *p, uint32_t body, ModelOut *out) {
    float t = p->time;
    Mat sc = rmat(g, MAT_SCALES, body, 0);
    Mat belly = rmat(g, MAT_SCALES, col_mix(body, RGBA8(230, 200, 140, 255), 0.5f), 0);
    float sw = sinf(p->walk) * p->move;
    float by = 0.75f;
    rbox(g, v3m(0, by, 0), v3m(0.32f, 0.28f, 0.6f), m3identity(), sc);
    rbox(g, v3m(0, by - 0.18f, 0.05f), v3m(0.26f, 0.12f, 0.5f), m3identity(), belly);
    for (int i = 0; i < 4; i++) {
        float sx = (i & 1) ? 1.0f : -1.0f, sz = (i & 2) ? 1.0f : -1.0f;
        float a = sw * 0.6f * ((i == 0 || i == 3) ? 1.0f : -1.0f);
        rlimb(g, v3m(sx * 0.3f, by - 0.1f, sz * 0.4f), 0.09f, 0.62f, 0.09f, m3rotx(a), sc);
    }
    /*neck + head*/
    float lunge = p->attack > 0 ? sinf(p->attack * MX_PI) : 0;
    v3 nb = v3m(0, by + 0.1f, 0.55f);
    m3 nr = m3rotx(0.9f - lunge * 0.6f + sinf(t * 1.3f) * 0.05f);
    rlimb(g, nb, 0.12f, -0.65f, 0.12f, nr, sc);
    v3 head = limb_end(nb, -0.7f, nr);
    rbox(g, head, v3m(0.16f, 0.13f, 0.25f), m3rotx(0.2f), sc);
    float jaw = 0.1f + lunge * 0.5f;
    rbox(g, v3add(head, v3m(0, -0.12f, 0.12f)), v3m(0.12f, 0.04f, 0.2f), m3rotx(jaw), belly);
    for (int s = -1; s <= 1; s += 2)
        rprism(g, v3add(head, v3m(0.1f * (float)s, 0.1f, -0.15f)), 0.05f, 0.0f, 0.3f, 5, m3rotx(-1.0f), rmat(g, MAT_BONE, RGBA8(230, 220, 190, 255), 0));
    reyes(g, v3add(head, v3m(0, 0.06f, 0.18f)), 0.09f, 0.025f, RGBA8(255, 220, 40, 255), 1.0f);
    /*wings*/
    float flap = sinf(t * 3.0f) * 0.4f + 0.2f;
    uint32_t wc = col_scale(body, 0.75f);
    for (int s = -1; s <= 1; s += 2) {
        m3 r = m3mul(m3rotz((-0.4f - flap) * (float)s), m3roty(0.2f * (float)s));
        v3 root = v3m(0.25f * (float)s, by + 0.25f, 0.1f);
        rbox(g, v3add(root, m3apply(r, v3m(0.55f * (float)s, 0, 0))), v3m(0.55f, 0.02f, 0.38f), r, rmat(g, MAT_LEATHER, wc, 0));
    }
    /*tail*/
    v3 tp = v3m(0, by, -0.6f);
    for (int i = 0; i < 6; i++) {
        float a = sinf(t * 2.0f + (float)i * 0.6f) * 0.25f;
        m3 r = m3mul(m3roty(a), m3rotx(-1.85f));
        float w = 0.12f * (1.0f - (float)i / 7.0f);
        rlimb(g, tp, w, 0.22f, w, r, sc);
        tp = limb_end(tp, 0.22f, r);
    }
    if (p->attack > 0.3f && p->attack < 0.8f)
        rglow_sprite(g, v3add(head, v3m(0, -0.1f, 0.5f)), 0.6f, RGBA8(255, 120, 30, 255));
    out->height = (by + 1.1f) * g->s + 0.1f;
}

static void serpent(const Rig *g, const ModelParams *p, uint32_t body, ModelOut *out) {
    float t = p->time;
    Mat sc = rmat(g, MAT_SCALES, body, 0);
    int n = 9;
    float speed = 3.0f + 4.0f * p->move;
    float lunge = p->attack > 0 ? sinf(p->attack * MX_PI) * 0.4f : 0;
    for (int i = 0; i < n; i++) {
        float k = (float)i / (float)(n - 1);
        float z = 0.45f - k * 1.2f + lunge * (1 - k);
        float x = sinf(t * speed * 0.5f - k * 6.0f) * 0.22f * (0.3f + k);
        float y = 0.12f + (1.0f - k) * 0.25f * (i < 3 ? 1.0f : 0.0f);
        float w = 0.13f * (1.0f - k * 0.6f);
        rbox(g, v3m(x, y, z), v3m(w, w * 0.8f, 0.09f), m3roty(sinf(t * speed * 0.5f - k * 6.0f) * 0.4f), sc);
    }
    v3 head = v3m(sinf(t * speed * 0.5f) * 0.05f, 0.42f, 0.55f + lunge);
    rbox(g, head, v3m(0.12f, 0.09f, 0.15f), m3identity(), sc);
    reyes(g, v3add(head, v3m(0, 0.04f, 0.12f)), 0.07f, 0.02f, RGBA8(255, 240, 60, 255), 0.8f);
    Mat tongue = rmat(g, MAT_SKIN, RGBA8(220, 60, 80, 255), 0);
    if (sinf(t * 7) > 0.3f) rbox(g, v3add(head, v3m(0, -0.03f, 0.2f)), v3m(0.01f, 0.005f, 0.06f), m3identity(), tongue);
    out->height = 0.7f * g->s + 0.1f;
}

static void ooze(const Rig *g, const ModelParams *p, uint32_t body, ModelOut *out) {
    float t = p->time;
    float sq = sinf(t * 3.0f + p->walk) * 0.08f + (p->attack > 0 ? sinf(p->attack * MX_PI) * 0.2f : 0);
    Mat m = rmat(g, MAT_SLIME, body, 0.25f);
    rbox(g, v3m(0, 0.16f - sq * 0.3f, 0), v3m(0.42f + sq, 0.16f - sq * 0.5f, 0.42f + sq), m3roty(t * 0.3f), m);
    rbox(g, v3m(0, 0.38f - sq * 0.5f, 0), v3m(0.3f + sq * 0.8f, 0.12f, 0.3f + sq * 0.8f), m3roty(-t * 0.4f), m);
    rbox(g, v3m(0, 0.55f - sq * 0.6f, 0), v3m(0.17f + sq * 0.5f, 0.08f, 0.17f + sq * 0.5f), m3roty(t * 0.5f), m);
    reyes(g, v3m(0, 0.4f - sq * 0.5f, 0.29f + sq), 0.09f, 0.03f, RGBA8(20, 20, 20, 255), 0);
    out->height = 0.8f * g->s;
}

static void flyer(const Rig *g, const ModelParams *p, uint32_t body, bool bat, ModelOut *out) {
    float t = p->time;
    float hy = 1.1f + sinf(t * 2.5f) * 0.12f;
    float flap = sinf(t * (bat ? 16.0f : 11.0f)) * 0.9f;
    Mat m = rmat(g, bat ? MAT_FUR : MAT_LEATHER, body, 0);
    float lunge = p->attack > 0 ? sinf(p->attack * MX_PI) * 0.4f : 0;
    rbox(g, v3m(0, hy - lunge * 0.5f, lunge), v3m(0.12f, 0.1f, 0.2f), m3identity(), m);
    rbox(g, v3m(0, hy + 0.07f - lunge * 0.5f, 0.22f + lunge), v3m(0.08f, 0.08f, 0.08f), m3identity(), m);
    if (!bat) rprism(g, v3m(0, hy + 0.06f - lunge * 0.5f, 0.29f + lunge), 0.035f, 0.0f, 0.1f, 4, m3rotx(1.5708f), rmat(g, MAT_BONE, RGBA8(240, 190, 60, 255), 0));
    reyes(g, v3m(0, hy + 0.1f - lunge * 0.5f, 0.3f + lunge), 0.04f, 0.015f, bat ? RGBA8(255, 50, 50, 255) : RGBA8(20, 20, 20, 255), bat ? 1.0f : 0.0f);
    for (int s = -1; s <= 1; s += 2) {
        m3 r = m3rotz(flap * (float)s);
        rbox(g, v3add(v3m(0.1f * (float)s, hy + 0.02f, 0), m3apply(r, v3m(0.3f * (float)s, 0, 0))), v3m(0.3f, 0.015f, 0.16f), r, rmat(g, MAT_LEATHER, col_scale(body, 0.8f), 0));
    }
    out->height = (hy + 0.35f) * g->s;
}

static void spirit(const Rig *g0, const ModelParams *p, uint32_t body, ModelOut *out) {
    Rig rg = *g0;
    rg.fade = fminf(rg.fade, 0.55f);
    HumanoidLook L;
    memset(&L, 0, sizeof(L));
    L.height = 1.0f; L.width = 1.0f;
    L.skin = body; L.cloth = body; L.cloth2 = col_scale(body, 0.7f);
    L.skin_mat = MAT_CLOTH; L.cloth_mat = MAT_CLOTH;
    L.robe = true; L.hood = true; L.glow_eyes = true; L.eye_col = RGBA8(160, 240, 255, 255);
    L.hover = 0.35f;
    humanoid(&rg, p, &L, out);
    rglow_sprite(g0, v3m(0, 1.0f, 0), 1.2f, col_scale(body, 0.35f));
}

static void elemental(const Rig *g, const ModelParams *p, uint32_t body, ModelOut *out) {
    float t = p->time;
    float hy = 0.9f + sinf(t * 1.5f) * 0.08f;
    Mat core = rmat(g, MAT_CRYSTAL, body, 1.0f);
    rbox(g, v3m(0, hy, 0), v3m(0.18f, 0.18f, 0.18f), m3mul(m3roty(t), m3rotx(t * 0.7f)), core);
    Mat rock = rmat(g, MAT_ROCK, col_mix(body, RGBA8(90, 80, 70, 255), 0.6f), 0.1f);
    for (int i = 0; i < 7; i++) {
        float a = t * (0.8f + 0.1f * (float)i) + (float)i * 0.9f;
        float rr = 0.35f + 0.08f * sinf(t * 2 + (float)i);
        v3 c = v3m(cosf(a) * rr, hy + sinf(a * 1.3f) * 0.3f, sinf(a) * rr);
        rbox(g, c, v3m(0.07f, 0.07f, 0.07f), m3mul(m3roty(a), m3rotx(a * 1.7f)), rock);
    }
    rglow_sprite(g, v3m(0, hy, 0), 1.0f, col_scale(body, 0.6f));
    out->has_light = true;
    out->light.pos[0] = g->pos.x; out->light.pos[1] = g->pos.y + hy * g->s; out->light.pos[2] = g->pos.z; out->light.pos[3] = 4.0f;
    out->light.col[0] = ((body) & 255) / 255.0f; out->light.col[1] = ((body >> 8) & 255) / 255.0f;
    out->light.col[2] = ((body >> 16) & 255) / 255.0f; out->light.col[3] = 1.0f;
    out->height = (hy + 0.55f) * g->s;
}

static void construct(const Rig *g, const ModelParams *p, uint32_t body, ModelOut *out) {
    HumanoidLook L;
    memset(&L, 0, sizeof(L));
    L.height = 1.25f; L.width = 1.6f;
    L.skin = body; L.cloth = body; L.cloth2 = col_scale(body, 0.8f);
    L.skin_mat = MAT_ROCK; L.cloth_mat = MAT_ROCK;
    L.glow_eyes = true; L.eye_col = RGBA8(255, 170, 60, 255);
    humanoid(g, p, &L, out);
}

static void aberration(const Rig *g, const ModelParams *p, uint32_t body, ModelOut *out) {
    float t = p->time;
    float hy = 1.0f + sinf(t * 1.7f) * 0.1f;
    Mat skin = rmat(g, MAT_SKIN, body, 0);
    rbox(g, v3m(0, hy, 0), v3m(0.3f, 0.3f, 0.3f), m3mul(m3roty(sinf(t * 0.7f) * 0.3f), m3rotz(0.785f * 0.3f)), skin);
    rbox(g, v3m(0, hy + 0.02f, 0.27f), v3m(0.16f, 0.16f, 0.06f), m3identity(), rmat(g, MAT_WHITE, RGBA8(250, 250, 240, 255), 0));
    float look = sinf(t * 0.9f) * 0.05f;
    rbox(g, v3m(look, hy + 0.02f, 0.33f), v3m(0.07f, 0.07f, 0.02f), m3identity(), rmat(g, MAT_WHITE, RGBA8(200, 40, 40, 255), 0.8f));
    for (int i = 0; i < 6; i++) {
        float a = (float)i / 6.0f * MX_TAU;
        v3 root = v3m(cosf(a) * 0.2f, hy - 0.2f, sinf(a) * 0.2f);
        m3 r = m3mul(m3roty(-a), m3rotz(0.3f + sinf(t * 3 + (float)i) * 0.4f));
        rlimb(g, root, 0.03f, 0.35f, 0.03f, r, skin);
        v3 e = limb_end(root, 0.35f, r);
        rlimb(g, e, 0.02f, 0.3f, 0.02f, m3mul(m3roty(-a), m3rotz(0.8f + sinf(t * 3.5f + (float)i) * 0.6f)), skin);
    }
    out->height = (hy + 0.45f) * g->s;
}

static void plant(const Rig *g, const ModelParams *p, uint32_t body, ModelOut *out) {
    float t = p->time;
    Mat bark = rmat(g, MAT_BARK, RGBA8(150, 120, 90, 255), 0);
    Mat leaf = rmat(g, MAT_LEAF, body, 0);
    float sway = sinf(t * 1.5f) * 0.08f + (p->attack > 0 ? sinf(p->attack * MX_PI) * 0.4f : 0);
    rprism(g, v3m(0, 0, 0), 0.16f, 0.1f, 1.0f, 6, m3rotx(sway), bark);
    for (int i = 0; i < 5; i++) {
        float a = (float)i / 5.0f * MX_TAU + 0.3f;
        v3 c = v3m(cosf(a) * 0.25f + sway * 0.8f, 1.0f + 0.15f * sinf((float)i * 2.3f), sinf(a) * 0.25f);
        rbox(g, c, v3m(0.22f, 0.15f, 0.22f), m3roty(a), leaf);
    }
    for (int s = -1; s <= 1; s += 2)
        rlimb(g, v3m(0.12f * (float)s, 0.65f, 0), 0.04f, 0.5f, 0.04f, m3mul(m3rotz((0.7f + sinf(t * 2 + (float)s) * 0.2f) * (float)s), m3rotx(-p->attack * 1.5f)), bark);
    reyes(g, v3m(0, 0.75f, 0.13f), 0.05f, 0.02f, RGBA8(255, 230, 80, 255), 0.9f);
    out->height = 1.35f * g->s;
}

static void chest(const Rig *g, const ModelParams *p, ModelOut *out) {
    Mat wood = rmat(g, MAT_PLANK, RGBA8(170, 120, 70, 255), 0);
    Mat gold = rmat(g, MAT_METAL, RGBA8(240, 200, 80, 255), 0.15f);
    rbox(g, v3m(0, 0.2f, 0), v3m(0.32f, 0.2f, 0.22f), m3identity(), wood);
    rbox(g, v3m(0, 0.44f, -0.01f), v3m(0.33f, 0.07f, 0.23f), m3identity(), wood);
    for (int s = -1; s <= 1; s += 2)
        rbox(g, v3m(0.22f * (float)s, 0.28f, 0), v3m(0.03f, 0.25f, 0.235f), m3identity(), gold);
    rbox(g, v3m(0, 0.36f, 0.225f), v3m(0.05f, 0.06f, 0.015f), m3identity(), gold);
    float tw = 0.5f + 0.5f * sinf(p->time * 3.0f);
    rglow_sprite(g, v3m(0.1f, 0.55f, 0.15f), 0.15f + 0.1f * tw, RGBA8(255, 230, 140, 255));
    out->height = 0.8f * g->s;
}

static void gold_pile(const Rig *g, const ModelParams *p, ModelOut *out) {
    Mat gold = rmat(g, MAT_GOLD, RGBA8(255, 215, 90, 255), 0.25f);
    for (int i = 0; i < 9; i++) {
        uint32_t h = hash_u32(p->seed + (uint32_t)i * 31u);
        float a = hash1f(h) * MX_TAU, r = hash1f(h * 3u) * 0.22f;
        rprism(g, v3m(cosf(a) * r, (float)(i / 3) * 0.035f, sinf(a) * r), 0.08f, 0.08f, 0.025f, 8, m3rotx((hash1f(h * 7u) - 0.5f) * 0.4f), gold);
    }
    float tw = 0.5f + 0.5f * sinf(p->time * 4.0f + (float)(p->seed & 7));
    rglow_sprite(g, v3m(0.05f, 0.2f, 0.05f), 0.12f + 0.12f * tw, RGBA8(255, 240, 160, 255));
    out->height = 0.5f * g->s;
}

static void tombstone(const Rig *g, const ModelParams *p, ModelOut *out) {
    Mat stone = rmat(g, MAT_ROCK, RGBA8(170, 165, 175, 255), 0);
    Mat dirt = rmat(g, MAT_DIRT, RGBA8(160, 140, 120, 255), 0);
    rbox(g, v3m(0, 0.05f, 0.15f), v3m(0.22f, 0.06f, 0.35f), m3identity(), dirt);
    rbox(g, v3m(0, 0.38f, -0.15f), v3m(0.2f, 0.32f, 0.05f), m3rotx(-0.08f), stone);
    rbox(g, v3m(0, 0.73f, -0.15f), v3m(0.13f, 0.06f, 0.05f), m3rotx(-0.08f), stone);
    float y = 0.8f + sinf(p->time * 1.5f) * 0.12f;
    rglow_sprite(g, v3m(sinf(p->time) * 0.2f, y, 0.1f), 0.3f, RGBA8(170, 80, 255, 255));
    out->height = 1.0f * g->s;
}

/* ------------------------------------------------------------------ */
/* Dispatcher                                                          */
/* ------------------------------------------------------------------ */

static void race_look(HumanoidLook *L, int race, uint32_t seed) {
    static const uint32_t skins[] = {RGBA8(240, 200, 170, 255), RGBA8(210, 160, 120, 255), RGBA8(160, 110, 80, 255),
                                     RGBA8(110, 75, 55, 255), RGBA8(250, 215, 190, 255)};
    L->skin = skins[hash_u32(seed) % 5];
    L->hair = seed_col(seed, 9, 0.02f, 0.1f, 0.6f, 0.35f);
    switch (race) {
    case RACE_ELF: case RACE_HALF_ELF: case RACE_ASTRAL_ELF: case RACE_VEDALKEN:
        L->height = 1.08f; L->width = 0.9f; L->ears_pointy = true; break;
    case RACE_DWARF: L->height = 0.78f; L->width = 1.25f; L->beard = true; break;
    case RACE_GNOME: case RACE_HALFLING: case RACE_KENDER: case RACE_AUTOGNOME:
        L->height = 0.68f; L->width = 0.95f; L->ears_pointy = race == RACE_GNOME; break;
    case RACE_GOLIATH: case RACE_FIRBOLG: L->height = 1.25f; L->width = 1.2f;
        L->skin = race == RACE_GOLIATH ? RGBA8(150, 150, 160, 255) : RGBA8(140, 160, 200, 255); break;
    case RACE_ORC: case RACE_HALF_ORC: L->height = 1.08f; L->width = 1.2f; L->skin = RGBA8(110, 150, 90, 255); break;
    case RACE_TIEFLING: case RACE_HEXBLOOD: L->skin = RGBA8(190, 70, 70, 255); L->horns = true; L->tail = true; break;
    case RACE_AASIMAR: L->halo = true; L->eye_col = RGBA8(255, 240, 180, 255); L->glow_eyes = true; break;
    case RACE_DRAGONBORN: L->skin = seed_col(seed, 3, 0.0f, 1.0f, 0.6f, 0.6f); L->skin_mat = MAT_SCALES; L->snout = true; L->height = 1.08f; break;
    case RACE_TABAXI: case RACE_LEONIN: L->skin = RGBA8(200, 150, 80, 255); L->skin_mat = MAT_FUR; L->ears_cat = true; L->tail = true; break;
    case RACE_AARAKOCRA: case RACE_KENKU: case RACE_OWLIN: L->skin = RGBA8(120, 90, 70, 255); L->skin_mat = MAT_FUR; L->beak = true; break;
    case RACE_TORTLE: L->skin = RGBA8(120, 160, 90, 255); L->shell = true; L->width = 1.2f; break;
    case RACE_GOBLIN: L->height = 0.66f; L->skin = RGBA8(120, 160, 70, 255); L->ears_pointy = true; break;
    case RACE_HOBGOBLIN: L->skin = RGBA8(200, 110, 60, 255); L->ears_pointy = true; break;
    case RACE_BUGBEAR: L->height = 1.2f; L->skin = RGBA8(150, 100, 60, 255); L->skin_mat = MAT_FUR; break;
    case RACE_KOBOLD: L->height = 0.62f; L->skin = RGBA8(150, 80, 60, 255); L->skin_mat = MAT_SCALES; L->snout = true; L->tail = true; break;
    case RACE_MINOTAUR: L->height = 1.25f; L->width = 1.3f; L->skin = RGBA8(110, 70, 50, 255); L->skin_mat = MAT_FUR; L->big_horns = true; L->snout = true; break;
    case RACE_YUAN_TI: case RACE_TRITON: case RACE_LOCATHAH: case RACE_GRUNG:
        L->skin = seed_col(seed, 4, 0.25f, 0.3f, 0.5f, 0.6f); L->skin_mat = MAT_SCALES; break;
    case RACE_GENASI: L->skin = seed_col(seed, 5, 0.0f, 1.0f, 0.5f, 0.8f); L->glow_eyes = true; break;
    case RACE_FAIRY: L->height = 0.6f; L->fairy_wings = true; L->hover = 0.4f; break;
    case RACE_WARFORGED: L->skin = RGBA8(150, 150, 140, 255); L->skin_mat = MAT_METAL; L->glow_eyes = true; L->eye_col = RGBA8(120, 200, 255, 255); break;
    case RACE_SATYR: L->horns = true; break;
    case RACE_HARENGON: L->ears_cat = true; L->skin_mat = MAT_FUR; L->skin = RGBA8(230, 220, 210, 255); break;
    case RACE_LOXODON: L->height = 1.2f; L->width = 1.3f; L->skin = RGBA8(150, 150, 150, 255); L->snout = true; break;
    case RACE_DHAMPIR: L->skin = RGBA8(230, 225, 230, 255); L->glow_eyes = true; L->eye_col = RGBA8(255, 60, 60, 255); break;
    case RACE_PLASMOID: L->skin = seed_col(seed, 6, 0.3f, 0.5f, 0.6f, 0.8f); L->skin_mat = MAT_SLIME; break;
    default: break;
    }
}

void model_build(Mesh *opaque, Mesh *trans, Mesh *add, v3 pos, float yaw, const ModelParams *p,
                 v3 cam_right, v3 cam_up, ModelOut *out) {
    Rig g;
    g.m = opaque;
    g.t = trans;
    g.a = add;
    g.pos = pos;
    g.R = m3roty(yaw);
    g.s = p->scale;
    g.flash = p->hurt;
    g.fade = p->fade;
    g.cr = cam_right;
    g.cu = cam_up;
    out->height = 1.6f;
    out->has_light = false;
    uint32_t sd = p->seed;
    const char *nm = p->name ? p->name : "";

    if (p->is_boss) g.s *= 1.45f;
    else if (p->archetype == 3 /*ARCH_BRUTE*/) g.s *= 1.2f;
    else if (p->archetype == 9 /*ARCH_SWARM*/) g.s *= 0.8f;

    HumanoidLook L;
    memset(&L, 0, sizeof(L));
    L.height = 1.0f; L.width = 1.0f;
    L.skin_mat = MAT_SKIN; L.cloth_mat = MAT_CLOTH;
    L.eye_col = RGBA8(30, 30, 40, 255);
    L.metal = RGBA8(190, 190, 200, 255);

    switch (p->kind) {
    case KIND_PLAYER: {
        race_look(&L, p->race_id, sd);
        L.cloth = class_color(p->class_id);
        L.cloth2 = col_scale(L.cloth, 0.55f);
        L.weapon = p->weapon;
        L.shield = p->shield;
        L.helm = p->helm;
        L.cape = p->cape;
        if (p->class_id == CLASS_WIZARD || p->class_id == CLASS_SORCERER || p->class_id == CLASS_WARLOCK) {
            L.robe = true;
            if (!L.helm && p->class_id == CLASS_WIZARD) L.hat = true;
            if (p->class_id == CLASS_WARLOCK) L.hood = true;
        }
        if (p->class_id == CLASS_DRUID || p->class_id == CLASS_CLERIC) L.robe = true;
        if (p->class_id == CLASS_ROGUE || p->class_id == CLASS_RANGER) L.hood = !L.helm;
        if (p->class_id == CLASS_FIGHTER || p->class_id == CLASS_PALADIN) L.cloth_mat = MAT_METAL;
        humanoid(&g, p, &L, out);
        break;
    }
    case KIND_MERCHANT:
        L.skin = RGBA8(230, 190, 160, 255);
        L.cloth = seed_col(sd, 1, 0.0f, 1.0f, 0.5f, 0.55f);
        L.cloth2 = RGBA8(80, 60, 40, 255);
        L.hair = RGBA8(90, 70, 50, 255);
        L.apron = true;
        L.hat = (sd & 1);
        L.beard = (sd & 2) != 0;
        L.width = 1.15f;
        humanoid(&g, p, &L, out);
        {
            /*floating coin marker*/
            float y = out->height / g.s + 0.25f + sinf(p->time * 2) * 0.05f;
            Mat gold = rmat(&g, MAT_GOLD, RGBA8(255, 215, 80, 255), 0.6f);
            rbox(&g, v3m(0, y, 0), v3m(0.1f, 0.1f, 0.02f), m3roty(p->time * 2.0f), gold);
            out->height += 0.3f * g.s;
        }
        break;
    case KIND_UNDEAD:
        L.skeleton = !(strcasestr(nm, "zombie") || strcasestr(nm, "ghoul") || strcasestr(nm, "mummy") || strcasestr(nm, "vampire"));
        L.skin = L.skeleton ? RGBA8(225, 220, 195, 255) : RGBA8(140, 160, 130, 255);
        L.skin_mat = L.skeleton ? MAT_BONE : MAT_SKIN;
        L.cloth = seed_col(sd, 1, 0.0f, 1.0f, 0.3f, 0.3f);
        L.cloth2 = col_scale(L.cloth, 0.7f);
        L.glow_eyes = true;
        L.eye_col = strcasestr(nm, "lich") ? RGBA8(80, 220, 255, 255) : RGBA8(255, 60, 40, 255);
        L.crown = strcasestr(nm, "lich") || strcasestr(nm, "lord") || strcasestr(nm, "king");
        L.robe = strcasestr(nm, "lich") || strcasestr(nm, "priest");
        L.weapon = p->archetype == 1 ? WPN_STAFF : WPN_SWORD;
        L.shield = !L.robe && (sd & 1);
        humanoid(&g, p, &L, out);
        break;
    case KIND_GIANT:
        L.height = 1.0f; L.width = 1.35f;
        g.s *= 1.7f;
        L.skin = strcasestr(nm, "troll") ? RGBA8(90, 130, 90, 255) : seed_col(sd, 2, 0.05f, 0.08f, 0.35f, 0.6f);
        L.cloth = RGBA8(110, 80, 50, 255); L.cloth_mat = MAT_LEATHER;
        L.cloth2 = RGBA8(80, 60, 40, 255);
        L.hair = RGBA8(60, 50, 40, 255);
        L.weapon = WPN_CLUB;
        humanoid(&g, p, &L, out);
        break;
    case KIND_FIEND:
        L.skin = seed_col(sd, 2, 0.95f, 0.1f, 0.75f, 0.6f);
        L.cloth = RGBA8(40, 20, 25, 255); L.cloth2 = RGBA8(30, 15, 20, 255);
        L.horns = true; L.tail = true; L.glow_eyes = true; L.eye_col = RGBA8(255, 200, 40, 255);
        L.wings = (sd & 1) != 0;
        L.weapon = (sd & 2) ? WPN_SPEAR : WPN_AXE;
        L.width = 1.15f;
        humanoid(&g, p, &L, out);
        break;
    case KIND_CELESTIAL:
        L.skin = RGBA8(250, 235, 210, 255);
        L.cloth = RGBA8(245, 240, 225, 255); L.cloth2 = RGBA8(220, 190, 110, 255);
        L.halo = true; L.wings = true; L.robe = true; L.glow_eyes = true; L.eye_col = RGBA8(255, 240, 180, 255);
        L.hover = 0.2f; L.weapon = WPN_SWORD; L.height = 1.1f;
        humanoid(&g, p, &L, out);
        out->has_light = true;
        out->light.pos[0] = pos.x; out->light.pos[1] = pos.y + 1.5f; out->light.pos[2] = pos.z; out->light.pos[3] = 4.5f;
        out->light.col[0] = 1.0f; out->light.col[1] = 0.9f; out->light.col[2] = 0.6f; out->light.col[3] = 1.0f;
        break;
    case KIND_FEY:
        L.height = 0.75f;
        L.skin = seed_col(sd, 2, 0.25f, 0.5f, 0.35f, 0.85f);
        L.cloth = seed_col(sd, 3, 0.3f, 0.5f, 0.6f, 0.6f); L.cloth2 = col_scale(L.cloth, 0.7f);
        L.ears_pointy = true; L.fairy_wings = (sd & 1) != 0; L.hover = L.fairy_wings ? 0.4f : 0;
        L.weapon = WPN_DAGGER;
        humanoid(&g, p, &L, out);
        break;
    case KIND_BEAST: {
        bool bear = strcasestr(nm, "bear") || strcasestr(nm, "ape");
        bool rat = strcasestr(nm, "rat") != NULL;
        bool horned = strcasestr(nm, "stag") || strcasestr(nm, "elk") || strcasestr(nm, "boar") || strcasestr(nm, "horn");
        uint32_t c = seed_col(sd, 2, 0.04f, 0.08f, 0.55f, 0.5f);
        if (strcasestr(nm, "wolf") || strcasestr(nm, "hound")) c = seed_col(sd, 2, 0.0f, 0.1f, 0.15f, 0.45f);
        float sz = rat ? 0.55f : (bear ? 1.35f : 1.0f);
        g.s *= sz;
        quadruped(&g, p, c, (strcasestr(nm, "lizard") ? MAT_SCALES : MAT_FUR), 0.95f, 0.85f, false, horned, out);
        break;
    }
    case KIND_INSECT:
        spider(&g, p, seed_col(sd, 2, 0.0f, 1.0f, 0.45f, 0.35f), out);
        break;
    case KIND_DRAGON:
        dragon(&g, p, seed_col(sd, 2, 0.0f, 1.0f, 0.65f, 0.6f), out);
        break;
    case KIND_SERPENT:
        g.s *= 1.2f;
        serpent(&g, p, seed_col(sd, 2, 0.15f, 0.5f, 0.6f, 0.55f), out);
        break;
    case KIND_OOZE: {
        uint32_t c = seed_col(sd, 2, 0.15f, 0.7f, 0.7f, 0.75f);
        c = (c & 0x00FFFFFFu) | (200u << 24);
        ooze(&g, p, c, out);
        break;
    }
    case KIND_FLYER:
        flyer(&g, p, seed_col(sd, 2, 0.02f, 0.15f, 0.4f, 0.45f), strcasestr(nm, "bat") != NULL, out);
        break;
    case KIND_SPIRIT:
        spirit(&g, p, seed_col(sd, 2, 0.5f, 0.25f, 0.35f, 0.9f), out);
        break;
    case KIND_ELEMENTAL: {
        uint32_t c = seed_col(sd, 2, 0.0f, 1.0f, 0.6f, 1.0f);
        if (strcasestr(nm, "fire") || strcasestr(nm, "flame") || strcasestr(nm, "heat") || strcasestr(nm, "magma")) c = RGBA8(255, 120, 30, 255);
        else if (strcasestr(nm, "ice") || strcasestr(nm, "frost")) c = RGBA8(140, 220, 255, 255);
        else if (strcasestr(nm, "air") || strcasestr(nm, "wind") || strcasestr(nm, "gale") || strcasestr(nm, "storm")) c = RGBA8(220, 235, 255, 255);
        else if (strcasestr(nm, "earth") || strcasestr(nm, "stone")) c = RGBA8(200, 150, 90, 255);
        else if (strcasestr(nm, "water")) c = RGBA8(60, 140, 255, 255);
        elemental(&g, p, c, out);
        break;
    }
    case KIND_CONSTRUCT:
        construct(&g, p, seed_col(sd, 2, 0.05f, 0.1f, 0.15f, 0.6f), out);
        break;
    case KIND_ABERRATION:
        aberration(&g, p, seed_col(sd, 2, 0.7f, 0.3f, 0.4f, 0.55f), out);
        break;
    case KIND_PLANT:
        plant(&g, p, seed_col(sd, 2, 0.2f, 0.15f, 0.6f, 0.55f), out);
        break;
    case KIND_CHEST: chest(&g, p, out); break;
    case KIND_GOLD: gold_pile(&g, p, out); break;
    case KIND_TOMBSTONE: tombstone(&g, p, out); break;
    case KIND_HUMANOID:
    default: {
        bool goblin = strcasestr(nm, "goblin") || strcasestr(nm, "kobold") || strcasestr(nm, "imp");
        bool orc = strcasestr(nm, "orc") || strcasestr(nm, "hobgoblin") || strcasestr(nm, "bugbear");
        L.skin = goblin ? RGBA8(120, 160, 70, 255) : (orc ? RGBA8(100, 140, 80, 255) : seed_col(sd, 2, 0.04f, 0.06f, 0.4f, 0.8f));
        L.height = goblin ? 0.72f : (orc ? 1.1f : 1.0f);
        L.width = orc ? 1.2f : 1.0f;
        L.ears_pointy = goblin || strcasestr(nm, "elf");
        L.cloth = seed_col(sd, 3, 0.0f, 1.0f, 0.5f, 0.45f);
        L.cloth2 = seed_col(sd, 4, 0.0f, 1.0f, 0.3f, 0.3f);
        L.hair = seed_col(sd, 5, 0.0f, 0.12f, 0.6f, 0.3f);
        if (p->archetype == 1) { L.robe = true; L.hood = true; L.weapon = WPN_STAFF; L.glow_eyes = true; L.eye_col = RGBA8(170, 120, 255, 255); }
        else if (p->archetype == 2) { L.hood = true; L.weapon = WPN_DAGGER; L.cloth = RGBA8(50, 50, 60, 255); }
        else if (p->archetype == 3) { L.weapon = WPN_CLUB; L.width *= 1.2f; }
        else { L.weapon = (sd & 1) ? WPN_SWORD : WPN_SPEAR; L.shield = (sd & 2) != 0; L.helm = (sd & 4) != 0; }
        L.crown = p->is_boss;
        humanoid(&g, p, &L, out);
        break;
    }
    }
    if (p->is_boss && !out->has_light) {
        out->has_light = true;
        out->light.pos[0] = pos.x; out->light.pos[1] = pos.y + out->height; out->light.pos[2] = pos.z; out->light.pos[3] = 5.0f;
        out->light.col[0] = 1.0f; out->light.col[1] = 0.3f; out->light.col[2] = 0.2f; out->light.col[3] = 0.9f;
    }
}
