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

/*In-game interface: HUD, windows and the event -> feedback glue
 * (sounds, banners, floating numbers, tutorial hints).
 *
 * Every window issues ordinary text commands ("w 3", "buy 2", "cast
 * Fire Bolt"...): the server and its rules are untouched, the interface
 * only removes the need to know and type them.*/

#include "app.h"
#include "audio.h"
#include "settings.h"
#include "platform.h"
#include "scene_internal.h"
#include "species.h"
#include "classes.h"
#include "alignment.h"
#include "items.h"
#include "net.h"
#include <GLFW/glfw3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static const char *floor_title(int f, char *buf, size_t cap) {
    if (f == 0) return "The Dragon's Maw Village";
    if (f == 100) return "The Dragon's Maw";
    if (f % 10 == 0) { snprintf(buf, cap, "The Seal of Floor %d", f); return buf; }
    static const char *tiers[] = {"The Upper Halls", "The Fungal Warrens", "The Crystal Deeps", "The Sunken Catacombs",
                                  "The Ashen Forges", "The Frozen Abyss", "The Obsidian Labyrinth",
                                  "The Drowned Caverns", "The Burning Depths", "The Dragon's Throat"};
    return tiers[(f / 10) % 10];
}

static int item_icon(const InvItem *it) {
    const char *n = it->name;
    switch (it->category) {
    case ITEM_WEAPON:
        if (strcasestr(n, "bow")) return ICON_BOW;
        if (strcasestr(n, "axe") || strcasestr(n, "halberd")) return ICON_AXE;
        if (strcasestr(n, "staff") || strcasestr(n, "rod")) return ICON_STAFF;
        if (strcasestr(n, "wand")) return ICON_WAND;
        if (strcasestr(n, "dagger") || strcasestr(n, "knife") || strcasestr(n, "sickle")) return ICON_DAGGER;
        if (strcasestr(n, "mace") || strcasestr(n, "hammer") || strcasestr(n, "club") || strcasestr(n, "flail")) return ICON_MACE;
        if (strcasestr(n, "spear") || strcasestr(n, "pike") || strcasestr(n, "trident")) return ICON_SPEAR;
        return ICON_SWORD;
    case ITEM_SHIELD: return ICON_SHIELD;
    case ITEM_ARMOR: return ICON_ARMOR;
    case ITEM_HEAD: return ICON_HELM;
    case ITEM_NECK: return ICON_AMULET;
    case ITEM_BACK: return ICON_CLOAK;
    case ITEM_HANDS: return ICON_GLOVES;
    case ITEM_FEET: return ICON_BOOTS;
    case ITEM_RING: return ICON_RING;
    case ITEM_BRACELET: return ICON_BRACELET;
    case ITEM_LIGHT_SOURCE: return ICON_TORCH;
    case ITEM_FUEL: return ICON_OIL;
    case ITEM_AMMO: return ICON_ARROW;
    case ITEM_BOOK: return ICON_BOOK;
    case ITEM_CONSUMABLE:
        if (strcasestr(n, "scroll")) return ICON_SCROLL;
        if (strcasestr(n, "wand")) return ICON_WAND;
        if (strcasestr(n, "ration") || strcasestr(n, "bread") || strcasestr(n, "food") || strcasestr(n, "meat") ||
            strcasestr(n, "apple") || strcasestr(n, "cheese") || strcasestr(n, "fruit"))
            return ICON_FOOD;
        return ICON_POTION;
    default:
        if (strcasestr(n, "gem") || strcasestr(n, "crystal") || strcasestr(n, "ruby") || strcasestr(n, "diamond")) return ICON_GEM;
        if (strcasestr(n, "key")) return ICON_QUESTION;
        if (strcasestr(n, "scroll")) return ICON_SCROLL;
        return ICON_BAG;
    }
}

static uint32_t item_rarity_col(const InvItem *it) {
    if (it->flags & INVF_ARTIFACT) return UIC(255, 170, 40, 255);
    if (it->flags & INVF_CURSED) return UIC(220, 60, 60, 255);
    if (it->flags & INVF_MAGIC) return UIC(100, 160, 255, 255);
    return UIC(140, 120, 90, 255);
}

static const char *cat_name(int c) {
    static const char *N[] = {"Misc", "Weapon", "Shield", "Armor", "Helm", "Amulet", "Cloak", "Gloves",
                              "Boots", "Ring", "Light", "Consumable", "Fuel", "Ammo", "Book", "Bracelet"};
    return (c >= 0 && c < 16) ? N[c] : "Item";
}

static void item_tooltip(const InvItem *it, const char *hint) {
    char body[700];
    int n = 0;
    n += snprintf(body + n, sizeof(body) - (size_t)n, "^9%s", cat_name(it->category));
    if (it->qty > 1) n += snprintf(body + n, sizeof(body) - (size_t)n, "  x%d", it->qty);
    n += snprintf(body + n, sizeof(body) - (size_t)n, "^0\n");
    if (it->dmg_count > 0) n += snprintf(body + n, sizeof(body) - (size_t)n, "Damage: ^7%dd%d^0\n", it->dmg_count, it->dmg_sides);
    if (it->ac > 0) n += snprintf(body + n, sizeof(body) - (size_t)n, "Armor: ^7%d^0\n", it->ac);
    if (it->heal > 0) n += snprintf(body + n, sizeof(body) - (size_t)n, "Heals ^2%d HP^0\n", it->heal);
    if (it->light > 0) n += snprintf(body + n, sizeof(body) - (size_t)n, "Light radius ^3%d^0\n", it->light);
    if (it->max_durability > 0)
        n += snprintf(body + n, sizeof(body) - (size_t)n, "Durability %d/%d\n", it->durability, it->max_durability);
    n += snprintf(body + n, sizeof(body) - (size_t)n, "Weight %.1f lb  -  Value %u gp\n", (double)it->weight, it->value);
    if (!(it->flags & INVF_IDENTIFIED)) n += snprintf(body + n, sizeof(body) - (size_t)n, "^3Unidentified^0\n");
    if (it->flags & INVF_CURSED) n += snprintf(body + n, sizeof(body) - (size_t)n, "^1Cursed!^0\n");
    if (it->flags & INVF_ARTIFACT) n += snprintf(body + n, sizeof(body) - (size_t)n, "^8Legendary artifact^0\n");
    if (hint) snprintf(body + n, sizeof(body) - (size_t)n, "\n^9%s", hint);
    ui_tooltip_color(it->name, item_rarity_col(it), body);
}

static void slot_cell(float x, float y, float s, const InvItem *it, bool highlight) {
    ui_rrect_grad(x, y, s, s, 6, UIC(34, 29, 38, 235), UIC(18, 15, 22, 235));
    ui_rrect_line(x, y, s, s, 6, 1.2f, it ? item_rarity_col(it) : UIC(70, 60, 50, 200));
    if (highlight) ui_rrect_line(x - 2, y - 2, s + 4, s + 4, 7, 1.5f, COL_GOLD);
    if (!it) return;
    ui_icon(item_icon(it), x + s * 0.14f, y + s * 0.12f, s * 0.72f, COL_WHITE);
    if (it->qty > 1) {
        char q[12];
        snprintf(q, sizeof(q), "%d", it->qty);
        ui_text_outline(FONT_BOLD, 14, x + s - 5, y + s - 18, q, COL_WHITE, UIC(0, 0, 0, 220), ALIGN_RIGHT);
    }
    if (it->max_durability > 0 && it->durability * 4 < it->max_durability)
        ui_rect(x + 4, y + s - 5, (s - 8) * (float)it->durability / (float)it->max_durability, 2.5f, COL_RED);
}

/* ------------------------------------------------------------------ */
/* Events -> feedback                                                  */
/* ------------------------------------------------------------------ */

static float pan_for(int x, int y) {
    float sx, sy;
    if (scene_world_to_screen(v3m((float)x, 0.5f, (float)y), &sx, &sy)) return clampf((sx / ui_width()) * 2.0f - 1.0f, -1, 1) * 0.7f;
    return 0;
}

static void show_banner(const char *title, const char *sub) {
    snprintf(g_app.banner, sizeof(g_app.banner), "%s", title);
    snprintf(g_app.banner_sub, sizeof(g_app.banner_sub), "%s", sub ? sub : "");
    g_app.banner_time = g_app.time;
}

void hud_on_floor_change(int floor) {
    char tb[64], sub[128];
    const char *t = floor_title(floor, tb, sizeof(tb));
    if (floor == 0) snprintf(sub, sizeof(sub), "Safe haven - merchants, temples and the sacred fountain");
    else if (floor % 10 == 0) snprintf(sub, sizeof(sub), "Floor %d - %d ft below - a guardian seals the stairs", floor, floor * 50);
    else snprintf(sub, sizeof(sub), "Floor %d - %d ft below the surface", floor, floor * 50);
    show_banner(t, sub);
    g_app.path_len = 0;
    g_app.target_id = 0;
    g_app.path_target_id = 0;
    if (g_app.windows & WIN_SHOP) g_app.windows &= ~(uint32_t)WIN_SHOP;
}

static void tip(int id, const char *text) {
    if (!g_settings.show_hints || id < 0 || id >= 16 || g_app.tip_shown[id]) return;
    if (g_app.time - g_app.tip_time < 6.0) return;
    g_app.tip_shown[id] = true;
    g_app.tip_time = g_app.time;
    ui_toast(text, UIC(190, 220, 255, 255));
}

static void handle_event(const ClientEvent *e) {
    FrameSnapshot *s = g_app.snap;
    scene_handle_event(e, s);
    switch (e->kind) {
    case CEV_SERVER: {
        const MsgGameEvent *g = &e->ev;
        bool me_src = g->src_id == s->my_entity_id, me_dst = g->dst_id == s->my_entity_id;
        float pan = pan_for(g->x, g->y);
        switch (g->type) {
        case GEV_ATTACK_HIT:
            audio_sfx_pitch(me_dst ? SFX_HURT : SFX_HIT, 0.7f, pan, 0.9f + (float)(rand() % 20) / 100.0f);
            if (me_src || me_dst) g_app.last_combat = g_app.time;
            if (me_dst) g_app.damage_flash = 0.7f;
            break;
        case GEV_ATTACK_CRIT:
            audio_sfx(SFX_CRIT, 0.85f, pan);
            if (me_src || me_dst) g_app.last_combat = g_app.time;
            if (me_dst) g_app.damage_flash = 1.0f;
            break;
        case GEV_ATTACK_MISS:
            audio_sfx_pitch(SFX_MISS, 0.5f, pan, 0.9f + (float)(rand() % 25) / 100.0f);
            if (me_src || me_dst) g_app.last_combat = g_app.time;
            break;
        case GEV_HURT:
            audio_sfx(SFX_HIT, 0.5f, pan);
            break;
        case GEV_KILL:
            audio_sfx(SFX_KILL, 0.8f, pan);
            if (g->dst_id == g_app.target_id) g_app.target_id = 0;
            if (g->dst_id == g_app.path_target_id) g_app.path_target_id = 0;
            if (me_src) tip(5, "Victory! Defeated foes may drop loot: press G to pick up items on your tile.");
            break;
        case GEV_LEVEL_UP:
            if (me_src) {
                audio_sfx(SFX_LEVELUP, 1.0f, 0);
                g_app.levelup_time = g_app.time;
                g_app.levelup_level = g->value;
                tip(6, "You gained stat points! Open your Character sheet (C) to spend them.");
            }
            break;
        case GEV_PLAYER_DIED:
            if (me_dst) {
                audio_sfx(SFX_DEATH, 1.0f, 0);
                g_app.dead = true;
                g_app.dead_time = g_app.time;
                g_app.path_len = 0;
                g_app.target_id = 0;
            }
            break;
        case GEV_BOSS_DEFEATED:
            audio_sfx(SFX_BOSS, 1.0f, 0);
            show_banner("Guardian Defeated!", "The seal is broken: the stairs below are open.");
            break;
        default:
            break;
        }
        break;
    }
    case CEV_VFX:
        audio_sfx(e->vfx.vfx_type == 1 ? SFX_EXPLOSION : SFX_SPELL, 0.7f, pan_for(e->vfx.target_x, e->vfx.target_y));
        break;
    case CEV_GOLD_DELTA:
        if (e->value > 0) audio_sfx(SFX_COIN, 0.7f, 0);
        break;
    case CEV_HP_DELTA:
        if (e->value > 2) audio_sfx(SFX_HEAL, 0.6f, 0);
        else if (e->value < 0) g_app.damage_flash = fmaxf(g_app.damage_flash, 0.6f);
        break;
    case CEV_FLOOR_CHANGE:
        audio_sfx(SFX_STAIRS, 0.8f, 0);
        hud_on_floor_change(e->value);
        if (e->value > 0) tip(3, "Every floor hides ONE staircase down: look for its beam of light in the dark.");
        break;
    case CEV_INV_CHANGED: {
        static int last_count = -1;
        if (last_count >= 0 && e->value > last_count) audio_sfx(SFX_PICKUP, 0.6f, 0);
        last_count = e->value;
        break;
    }
    case CEV_SHOP_OPEN:
        g_app.windows |= WIN_SHOP | WIN_INVENTORY;
        audio_sfx(SFX_OPEN, 0.6f, 0);
        tip(4, "Shop open: click Buy, or Sell items from your backpack. Walk away to close it.");
        break;
    case CEV_SHOP_CLOSE:
        g_app.windows &= ~(uint32_t)WIN_SHOP;
        break;
    case CEV_WELCOME: {
        char t[96];
        snprintf(t, sizeof(t), e->value ? "Welcome, %s" : "Welcome back, %s", g_my_name);
        show_banner(t, e->value ? "Your adventure begins in the Dragon's Maw Village" : "The Maw awaits your return");
        tip(0, "Move with WASD (or click the ground). Hold the right mouse button to turn the camera.");
        break;
    }
    case CEV_TEXT: {
        const LogLine *l = &g_log[(uint32_t)e->value % LOG_CAP];
        if (strncmp(l->text, "[MSG]", 5) == 0 || strncmp(l->text, "[SAY]", 5) == 0) audio_sfx(SFX_MESSAGE, 0.6f, 0);
        else if (strncmp(l->text, "[ERROR]", 7) == 0) audio_sfx(SFX_ERROR, 0.35f, 0);
        break;
    }
    default:
        break;
    }
}

void hud_handle_events(void) {
    ClientEvent ev[64];
    int n;
    while ((n = evq_read(&g_app.ev_cursor_hud, ev, 64)) > 0)
        for (int i = 0; i < n; i++) handle_event(&ev[i]);
    if (g_app.snap->my_floor != g_app.last_floor_seen) {
        if (g_app.last_floor_seen < 0) hud_on_floor_change(g_app.snap->my_floor);
        g_app.last_floor_seen = g_app.snap->my_floor;
    }
    /*timed tips*/
    if (g_app.screen_t > 25) tip(1, "Click a monster to walk up and attack it. Space attacks the nearest enemy.");
    if (g_app.screen_t > 60) tip(2, "Press I for your inventory, H for the full guide, Enter to chat or type commands.");
    if (g_stat_points > 0 && g_app.screen_t > 8) tip(7, "You have unspent stat points: open the Character sheet (C).");
}

/* ------------------------------------------------------------------ */
/* Windows                                                             */
/* ------------------------------------------------------------------ */

void hud_toggle(uint32_t w) {
    if (g_app.windows & w) {
        g_app.windows &= ~w;
        audio_sfx(SFX_CLOSE, 0.4f, 0);
    } else {
        g_app.windows |= w;
        audio_sfx(SFX_OPEN, 0.4f, 0);
    }
}

bool hud_game_input_allowed(void) { return !(g_app.windows & WIN_PAUSE) && !g_app.dead; }

/*--- context menu (right click on an item) ---*/
typedef struct { bool open; float x, y; InvItem item; int idx; } CtxMenu;
static CtxMenu g_ctx;

static void smart_use(const InvItem *it) {
    int n = it->slot + 1;
    switch (it->where) {
    case INV_EQUIPPED: {
        static const char *slot[] = {"head", "neck", "body", "back", "hand_r", "hand_l", "hands", "arm_r", "arm_l", "feet"};
        if (it->slot < 10) app_send("takeoff %s", slot[it->slot]);
        else app_send("takeoff %s", it->name);
        audio_sfx(SFX_EQUIP, 0.6f, 0);
        return;
    }
    case INV_BELT:
        app_send("use belt %d", n);
        return;
    default:
        break;
    }
    switch (it->category) {
    case ITEM_CONSUMABLE: app_send("use %d", n); break;
    case ITEM_BOOK: app_send("lore %d", n); break;
    case ITEM_FUEL: app_send("fill lantern"); break;
    case ITEM_MISC: case ITEM_AMMO: app_send("lore %d", n); break;
    default: app_send("w %d", n); audio_sfx(SFX_EQUIP, 0.6f, 0); break;
    }
}

static void ctx_menu_draw(void) {
    if (!g_ctx.open) return;
    ui_layer(1);
    const InvItem *it = &g_ctx.item;
    int n = it->slot + 1;
    const char *labels[8];
    char cmds[8][96];
    int k = 0;
#define ADD(L, ...) do { labels[k] = L; snprintf(cmds[k], sizeof(cmds[k]), __VA_ARGS__); k++; } while (0)
    if (it->where == INV_BACKPACK) {
        if (it->category == ITEM_CONSUMABLE) ADD("Use", "use %d", n);
        if (it->category == ITEM_BOOK || strcasestr(it->name, "scroll")) ADD("Read", "read %d", n);
        if (it->category != ITEM_CONSUMABLE && it->category != ITEM_BOOK && it->category != ITEM_MISC &&
            it->category != ITEM_FUEL && it->category != ITEM_AMMO)
            ADD("Equip", "w %d", n);
        if (it->category == ITEM_CONSUMABLE || it->category == ITEM_AMMO || it->category == ITEM_FUEL) ADD("Put on belt", "belt %d", n);
        ADD("Inspect", "lore %d", n);
        if (g_shop.merchant_id) ADD("Sell", "sell %d", n);
        ADD("Drop", "drop %d", n);
    } else if (it->where == INV_BELT) {
        ADD("Use", "use belt %d", n);
        ADD("Back to backpack", "unbelt %d", n);
    } else {
        ADD("Take off", "takeoff %s", it->name);
    }
#undef ADD
    float w = 200, rh = 32, h = (float)k * rh + 12;
    float x = fminf(g_ctx.x, ui_width() - w - 4), y = fminf(g_ctx.y, ui_height() - h - 4);
    ui_shadow(x, y, w, h, 6, 10, 0.6f);
    ui_rrect_grad(x, y, w, h, 6, UIC(40, 33, 46, 250), UIC(22, 18, 26, 250));
    ui_rrect_line(x, y, w, h, 6, 1.2f, COL_GOLD);
    ui_block(x, y, w, h);
    for (int i = 0; i < k; i++) {
        float ry = y + 6 + (float)i * rh;
        bool hov = ui_hover(x + 4, ry, w - 8, rh);
        if (hov) ui_rrect(x + 4, ry, w - 8, rh, 4, UIC(120, 86, 40, 200));
        ui_text(FONT_BOLD, 16, x + 16, ry + rh * 0.5f, labels[i], strcmp(labels[i], "Drop") == 0 ? UIC(255, 150, 130, 255) : COL_TEXT, ALIGN_MIDDLE);
        if (hov && ui_input()->pressed[0]) {
            app_send("%s", cmds[i]);
            audio_sfx(SFX_CLICK, 0.4f, 0);
            g_ctx.open = false;
        }
    }
    if ((ui_input()->pressed[0] || ui_input()->pressed[1]) && !ui_hover(x, y, w, h)) g_ctx.open = false;
    if (ui_input()->keys[UIK_ESCAPE]) g_ctx.open = false;
    ui_layer(0);
}

static const InvItem *inv_find(int where, int slot) {
    for (int i = 0; i < g_inventory.count; i++)
        if (g_inventory.items[i].where == where && g_inventory.items[i].slot == slot) return &g_inventory.items[i];
    return NULL;
}

static void item_cell_interact(float x, float y, float s, const InvItem *it, const char *hint) {
    if (!it || !ui_hover(x, y, s, s) || g_ctx.open) return;
    item_tooltip(it, hint);
    if (ui_input()->pressed[0]) {
        smart_use(it);
        audio_sfx(SFX_CLICK, 0.35f, 0);
    }
    if (ui_input()->pressed[1]) {
        g_ctx.open = true;
        g_ctx.x = ui_input()->mx;
        g_ctx.y = ui_input()->my;
        g_ctx.item = *it;
    }
}

static void draw_inventory(float x, float y) {
    float w = 600, h = 668;
    if (!ui_window("Inventory", x, y, w, h, true)) { g_app.windows &= ~(uint32_t)WIN_INVENTORY; return; }
    pthread_mutex_lock(&g_state_mutex);
    MsgInventory inv = g_inventory;
    pthread_mutex_unlock(&g_state_mutex);
    (void)inv;
    /*equipment paper doll*/
    float ex = x + 24, ey = y + 60, s = 54;
    ui_header("Equipment", ex, ey, w - 48);
    ey += 30;
    static const struct { int slot; int icon; const char *label; float gx, gy; } EQ[] = {
        {EQ_HEAD, ICON_HELM, "Head", 1, 0},      {EQ_NECK, ICON_AMULET, "Neck", 2, 0},
        {EQ_BACK, ICON_CLOAK, "Back", 0, 0},     {EQ_BODY, ICON_ARMOR, "Body", 1, 1},
        {EQ_HAND_R, ICON_SWORD, "Right hand", 0, 1}, {EQ_HAND_L, ICON_SHIELD, "Left hand", 2, 1},
        {EQ_HANDS, ICON_GLOVES, "Hands", 3, 1},  {EQ_ARM_R, ICON_BRACELET, "Right arm", 3, 0},
        {EQ_ARM_L, ICON_BRACELET, "Left arm", 4, 0}, {EQ_FEET, ICON_BOOTS, "Feet", 4, 1},
    };
    for (size_t i = 0; i < sizeof(EQ) / sizeof(EQ[0]); i++) {
        float cx = ex + EQ[i].gx * (s + 8), cy = ey + EQ[i].gy * (s + 8);
        const InvItem *it = inv_find(INV_EQUIPPED, EQ[i].slot);
        slot_cell(cx, cy, s, it, false);
        if (!it) {
            ui_icon(EQ[i].icon, cx + s * 0.2f, cy + s * 0.2f, s * 0.6f, UIC(255, 255, 255, 40));
            if (ui_hover(cx, cy, s, s)) ui_tooltip(EQ[i].label, "Empty slot");
        }
        item_cell_interact(cx, cy, s, it, "Click: take off");
    }
    /*rings*/
    float rx = ex + 5 * (s + 8) + 10;
    ui_text(FONT_BODY, 14, rx, ey - 4, "Rings", COL_TEXT_FAINT, ALIGN_LEFT);
    for (int r = 0; r < 10; r++) {
        float cx = rx + (float)(r % 5) * 34, cy = ey + 14 + (float)(r / 5) * 34;
        const InvItem *it = inv_find(INV_EQUIPPED, EQ_RING0 + r);
        slot_cell(cx, cy, 30, it, false);
        item_cell_interact(cx, cy, 30, it, "Click: take off");
    }
    /*backpack*/
    float by = ey + 2 * (s + 8) + 22;
    char hdr[64];
    int nb = 0;
    for (int i = 0; i < inv.count; i++) if (inv.items[i].where == INV_BACKPACK) nb++;
    snprintf(hdr, sizeof(hdr), "Backpack (%d/32)", nb);
    ui_header(hdr, ex, by, w - 48);
    by += 30;
    float cs = 62;
    for (int i = 0; i < 32; i++) {
        float cx = ex + (float)(i % 8) * (cs + 6), cy = by + (float)(i / 8) * (cs + 6);
        const InvItem *it = inv_find(INV_BACKPACK, i);
        slot_cell(cx, cy, cs, it, g_ctx.open && it && g_ctx.item.where == INV_BACKPACK && g_ctx.item.slot == i);
        const char *hint = "Left-click: use / equip    Right-click: more";
        if (g_shop.merchant_id) hint = "Left-click: use / equip    Right-click: sell, drop...";
        item_cell_interact(cx, cy, cs, it, hint);
    }
    /*belt*/
    float bey = by + 4 * (cs + 6) + 14;
    ui_header("Quick belt (keys 1-4)", ex, bey, w - 48);
    bey += 28;
    for (int i = 0; i < 4; i++) {
        float cx = ex + (float)i * (cs + 6);
        const InvItem *it = inv_find(INV_BELT, i);
        slot_cell(cx, bey, cs, it, false);
        char k[4];
        snprintf(k, sizeof(k), "%d", i + 1);
        ui_text_outline(FONT_BOLD, 13, cx + 5, bey + 3, k, COL_GOLD, UIC(0, 0, 0, 200), ALIGN_LEFT);
        item_cell_interact(cx, bey, cs, it, "Click: use    Right-click: back to backpack");
    }
    /*weight & gold*/
    float wx = ex + 4 * (cs + 6) + 20, ww = w - (wx - x) - 24;
    char wl[64];
    snprintf(wl, sizeof(wl), "%.1f / %.0f lb", (double)inv.weight_now, (double)inv.weight_max);
    float frac = inv.weight_max > 0 ? inv.weight_now / inv.weight_max : 0;
    ui_text(FONT_BODY, 14, wx, bey + 2, "Load", COL_TEXT_FAINT, ALIGN_LEFT);
    ui_bar(wx, bey + 22, ww, 14, frac, frac > 0.9f ? COL_RED : UIC(160, 140, 100, 255), UIC(90, 76, 50, 255), wl);
    char gl[48];
    snprintf(gl, sizeof(gl), "%llu gold", (unsigned long long)g_my_gold);
    ui_icon(ICON_COIN, wx, bey + 42, 22, COL_WHITE);
    ui_text(FONT_BOLD, 17, wx + 28, bey + 53, gl, COL_GOLD, ALIGN_MIDDLE);
}

static const char *stat_names[6] = {"STR", "DEX", "CON", "INT", "WIS", "CHA"};
static const char *stat_full[6] = {"Strength", "Dexterity", "Constitution", "Intelligence", "Wisdom", "Charisma"};
static const char *stat_cmd[6] = {"str", "dex", "con", "int", "wis", "cha"};
static const char *stat_help[6] = {
    "Melee to-hit and damage, carrying capacity (15 lb per point).",
    "Finesse/ranged to-hit, armor class, disarming traps.",
    "Hit points gained per level, resistance to poison.",
    "Arcane magic (wizards), examining terrain and items.",
    "Divine magic, perception (search), saving throws.",
    "Merchant prices and haggling, charisma casters."};

static void draw_character(float x, float y) {
    float w = 560, h = 700;
    if (!ui_window("Character", x, y, w, h, true)) { g_app.windows &= ~(uint32_t)WIN_CHARACTER; return; }
    char buf[256];
    float cx = x + 28, cy = y + 60;
    const char *race = (g_race_id >= 0 && g_race_id < RACE_COUNT) ? RACES[g_race_id].name : "?";
    const char *cls = (g_class_id >= 0 && g_class_id < CLASS_COUNT) ? CLASSES[g_class_id].name : "?";
    ui_text_shadow(FONT_TITLE, 26, cx, cy, g_my_name, COL_GOLD, ALIGN_LEFT);
    snprintf(buf, sizeof(buf), "Level %d %s %s  -  %s", g_my_level, race, cls,
             (g_alignment >= 0 && g_alignment < ALIGN_COUNT) ? ALIGNMENTS[g_alignment].name : "");
    ui_text(FONT_BODY, 17, cx, cy + 36, buf, COL_TEXT_DIM, ALIGN_LEFT);
    cy += 70;
    snprintf(buf, sizeof(buf), "%d / %d", g_my_hp, g_my_max_hp);
    ui_text(FONT_BODY, 15, cx, cy, "Health", COL_TEXT_FAINT, ALIGN_LEFT);
    ui_bar(cx + 90, cy, w - 146, 18, (float)g_my_hp / (float)(g_my_max_hp > 0 ? g_my_max_hp : 1), UIC(220, 60, 50, 255), UIC(120, 20, 20, 255), buf);
    cy += 28;
    int prev = 0;
    static const int TH[] = {0, 300, 900, 2700, 6500, 14000, 23000, 34000, 48000, 64000, 85000, 100000, 120000, 140000, 165000, 195000, 225000, 265000, 305000, 355000};
    if (g_my_level >= 1 && g_my_level <= 20) prev = TH[g_my_level - 1];
    float xf = g_my_xp_next > prev ? (float)(g_my_xp - prev) / (float)(g_my_xp_next - prev) : 1.0f;
    snprintf(buf, sizeof(buf), g_my_level >= 20 ? "%d XP (max level)" : "%d / %d XP", g_my_xp, g_my_xp_next);
    ui_text(FONT_BODY, 15, cx, cy, "Experience", COL_TEXT_FAINT, ALIGN_LEFT);
    ui_bar(cx + 90, cy, w - 146, 18, xf, UIC(240, 200, 90, 255), UIC(150, 110, 30, 255), buf);
    cy += 40;
    ui_header("Abilities", cx, cy, w - 56);
    cy += 32;
    int sv[6] = {g_str, g_dex, g_con, g_intel, g_wis, g_cha};
    if (g_stat_points > 0) {
        snprintf(buf, sizeof(buf), "%d point%s to spend - click +", g_stat_points, g_stat_points > 1 ? "s" : "");
        ui_text(FONT_BOLD, 15, x + w - 28, cy - 30, buf, COL_GREEN, ALIGN_RIGHT);
    }
    for (int i = 0; i < 6; i++) {
        float ry = cy + (float)i * 40;
        bool hov = ui_hover(cx, ry, w - 56, 36);
        if (hov) { ui_rrect(cx - 6, ry - 2, w - 44, 38, 5, UIC(60, 50, 40, 120)); ui_tooltip(stat_full[i], stat_help[i]); }
        ui_text(FONT_BOLD, 18, cx, ry + 17, stat_names[i], COL_GOLD, ALIGN_MIDDLE);
        ui_text(FONT_BODY, 15, cx + 56, ry + 17, stat_full[i], COL_TEXT_DIM, ALIGN_MIDDLE);
        snprintf(buf, sizeof(buf), "%d", sv[i]);
        ui_text(FONT_BOLD, 22, cx + 300, ry + 17, buf, COL_TEXT, ALIGN_RIGHT | ALIGN_MIDDLE);
        int mod = (sv[i] - 10) / 2 - ((sv[i] < 10 && (sv[i] & 1)) ? 1 : 0);
        snprintf(buf, sizeof(buf), "(%+d)", mod);
        ui_text(FONT_BODY, 16, cx + 310, ry + 17, buf, mod >= 0 ? COL_GREEN : COL_RED, ALIGN_MIDDLE);
        if (g_stat_points > 0 && ui_button("+", x + w - 70, ry + 3, 32, 30, BTN_PRIMARY)) {
            app_send("train %s", stat_cmd[i]);
            audio_sfx(SFX_LEVELUP, 0.4f, 0);
        }
    }
    cy += 6 * 40 + 10;
    ui_header("Combat", cx, cy, w - 56);
    cy += 32;
    snprintf(buf, sizeof(buf), "Armor Class ^7%d^0     To hit ^7%+d^0     Damage ^7%+d^0", g_my_ac, g_to_hit, g_to_dmg);
    ui_text_rich(FONT_BODY, 17, cx, cy, buf, COL_TEXT_DIM, ALIGN_LEFT);
    cy += 26;
    snprintf(buf, sizeof(buf), "Weapon ^7%s^0     Armor ^7%s^0", g_weapon_name, g_armor_name);
    ui_text_rich(FONT_BODY, 16, cx, cy, buf, COL_TEXT_DIM, ALIGN_LEFT);
    cy += 26;
    snprintf(buf, sizeof(buf), "Deepest floor reached ^7%d^0     Gold ^8%llu^0", g_max_floor, (unsigned long long)g_my_gold);
    ui_text_rich(FONT_BODY, 16, cx, cy, buf, COL_TEXT_DIM, ALIGN_LEFT);
    cy += 36;
    ui_header("Guardians defeated", cx, cy, w - 56);
    cy += 32;
    for (int b = 0; b < 10; b++) {
        float bx = cx + (float)b * 49;
        bool d = (g_bosses_defeated & (1u << b)) != 0;
        ui_rrect(bx, cy, 44, 44, 6, d ? UIC(120, 80, 20, 240) : UIC(30, 26, 32, 220));
        ui_rrect_line(bx, cy, 44, 44, 6, 1.2f, d ? COL_GOLD : UIC(70, 60, 50, 200));
        ui_icon(d ? ICON_SKULL : ICON_QUESTION, bx + 8, cy + 4, 28, d ? COL_WHITE : UIC(255, 255, 255, 50));
        snprintf(buf, sizeof(buf), "%d", (b + 1) * 10);
        ui_text(FONT_BOLD, 12, bx + 22, cy + 34, buf, d ? COL_GOLD : COL_TEXT_FAINT, ALIGN_CENTER);
        if (ui_hover(bx, cy, 44, 44)) {
            snprintf(buf, sizeof(buf), "Guardian of floor %d", (b + 1) * 10);
            ui_tooltip(buf, d ? "Defeated - its stairs are open to you." : "Still undefeated. It seals the stairs of its floor.");
        }
    }
    if (ui_button("Details", x + w - 128, y + 58, 100, 30, BTN_NORMAL)) app_send("status");
}

/*--- spells & hotbar ---*/
static char g_hotbar_owner[32];

static void hotbar_path(char *out, size_t cap) {
    char dir[900];
    if (!plat_user_dir(dir, sizeof(dir))) snprintf(dir, sizeof(dir), ".");
    snprintf(out, cap, "%s/hotbar_%s.txt", dir, g_my_name);
}

static void hotbar_load(void) {
    if (strcmp(g_hotbar_owner, g_my_name) == 0) return;
    snprintf(g_hotbar_owner, sizeof(g_hotbar_owner), "%s", g_my_name);
    memset(g_app.spell_hotbar, 0, sizeof(g_app.spell_hotbar));
    char p[1000];
    hotbar_path(p, sizeof(p));
    FILE *f = fopen(p, "r");
    if (!f) return;
    char line[64];
    for (int i = 0; i < HOTBAR_SPELLS && fgets(line, sizeof(line), f); i++) {
        line[strcspn(line, "\r\n")] = '\0';
        snprintf(g_app.spell_hotbar[i], sizeof(g_app.spell_hotbar[i]), "%s", line);
    }
    fclose(f);
}

static void hotbar_save(void) {
    char p[1000];
    hotbar_path(p, sizeof(p));
    FILE *f = fopen(p, "w");
    if (!f) return;
    for (int i = 0; i < HOTBAR_SPELLS; i++) fprintf(f, "%s\n", g_app.spell_hotbar[i]);
    fclose(f);
}

void hud_cast_hotbar(int i) {
    if (i < 0 || i >= HOTBAR_SPELLS) return;
    if (!g_app.spell_hotbar[i][0]) {
        ui_toast("Empty slot: open the Spellbook (K) to assign a spell.", COL_TEXT_DIM);
        return;
    }
    app_send("cast %s", g_app.spell_hotbar[i]);
    audio_sfx(SFX_CLICK, 0.3f, 0);
}

void hud_use_belt(int i) {
    const InvItem *it = inv_find(INV_BELT, i);
    if (!it) {
        ui_toast("Empty belt slot: right-click a potion and choose 'Put on belt'.", COL_TEXT_DIM);
        return;
    }
    app_send("use belt %d", i + 1);
}

static const char *school_tag(const SpellEntry *s) {
    switch (s->effect_type) {
    case 0: return "^1Damage^0";
    case 1: return "^2Healing^0";
    case 2: return "^4Buff^0";
    default: return "^5Hex^0";
    }
}

static void draw_spells(float x, float y) {
    float w = 620, h = 660;
    if (!ui_window("Spellbook", x, y, w, h, true)) { g_app.windows &= ~(uint32_t)WIN_SPELLS; return; }
    hotbar_load();
    pthread_mutex_lock(&g_state_mutex);
    static MsgSpellbook book;
    book = g_spellbook;
    pthread_mutex_unlock(&g_state_mutex);
    char buf[160];
    float cx = x + 24, cy = y + 56;
    /*slots summary*/
    float sx = cx;
    ui_text(FONT_BODY, 14, sx, cy + 8, "Slots:", COL_TEXT_FAINT, ALIGN_MIDDLE);
    sx += 50;
    for (int l = 1; l <= 9; l++) {
        if (g_my_spell_slots_max[l] <= 0) continue;
        snprintf(buf, sizeof(buf), "L%d %d/%d", l, g_my_spell_slots[l], g_my_spell_slots_max[l]);
        float bw = ui_text_width(FONT_BOLD, 14, buf) + 14;
        ui_rrect(sx, cy, bw, 18, 4, g_my_spell_slots[l] > 0 ? UIC(40, 60, 110, 220) : UIC(60, 30, 30, 220));
        ui_text(FONT_BOLD, 14, sx + 7, cy + 9, buf, COL_TEXT, ALIGN_MIDDLE);
        sx += bw + 6;
        if (sx > x + w - 90) break;
    }
    cy += 32;
    if (book.count == 0) {
        ui_text_wrap(FONT_BODY, 17, cx, cy + 10, w - 48,
                     "You do not know any spell or technique yet.\n\n"
                     "^8How to learn:^0 buy a book from the magic shops or the Archive of a Thousand Battles in the "
                     "village, then enter the temple of your class (marked on the map, M) and choose ^8Read^0 on the "
                     "book from your inventory. Each level you gain opens new pages.",
                     COL_TEXT_DIM, 22);
        return;
    }
    float rowh = 44;
    float listh = h - (cy - y) - 20;
    float off = ui_scroll_begin("spells", cx, cy, w - 48, listh, (float)book.count * rowh + 20);
    int last_lv = -1;
    float ry = cy + off;
    for (int i = 0; i < book.count; i++) {
        SpellEntry *s = &book.spells[i];
        if (s->level != last_lv) {
            last_lv = s->level;
            snprintf(buf, sizeof(buf), s->level == 0 ? "Cantrips" : "Level %d", s->level);
            ui_text(FONT_TITLE, 15, cx, ry + 4, buf, COL_GOLD, ALIGN_LEFT);
            ry += 22;
        }
        bool hov = ui_hover(cx, ry, w - 60, rowh - 4);
        ui_rrect(cx, ry, w - 60, rowh - 4, 5, hov ? UIC(70, 56, 40, 200) : UIC(30, 26, 34, 180));
        ui_icon(s->effect_type == 1 ? ICON_HEART : ICON_STAR, cx + 6, ry + 6, 28, COL_WHITE);
        ui_text(FONT_BOLD, 16, cx + 42, ry + 5, s->name, COL_TEXT, ALIGN_LEFT);
        snprintf(buf, sizeof(buf), "%s  ^9range %d%s", school_tag(s), s->range, s->radius > 0 ? "  area" : "");
        ui_text_rich(FONT_BODY, 13, cx + 42, ry + 23, buf, COL_TEXT_DIM, ALIGN_LEFT);
        if (ui_button("Cast", cx + w - 300, ry + 6, 64, 28, BTN_PRIMARY)) {
            app_send("cast %s", s->name);
        }
        for (int k = 0; k < HOTBAR_SPELLS; k++) {
            char kl[4];
            snprintf(kl, sizeof(kl), "%d", (5 + k) % 10);
            bool on = strcmp(g_app.spell_hotbar[k], s->name) == 0;
            if (ui_button(kl, cx + w - 228 + (float)k * 28, ry + 8, 24, 24, on ? BTN_TAB_ACTIVE : BTN_TAB)) {
                if (on) g_app.spell_hotbar[k][0] = '\0';
                else snprintf(g_app.spell_hotbar[k], sizeof(g_app.spell_hotbar[k]), "%s", s->name);
                hotbar_save();
            }
        }
        if (hov && ui_input()->mx < cx + w - 300) ui_tooltip(s->name, "Cast it here or bind it to a hotbar key (5-0) with the small buttons.");
        ry += rowh;
    }
    ui_scroll_end();
}

static void draw_shop(float x, float y) {
    float w = 640, h = 640;
    pthread_mutex_lock(&g_state_mutex);
    static MsgShop shop;
    shop = g_shop;
    pthread_mutex_unlock(&g_state_mutex);
    if (shop.merchant_id == 0) { g_app.windows &= ~(uint32_t)WIN_SHOP; return; }
    if (!ui_window(shop.shop_name[0] ? shop.shop_name : "Merchant", x, y, w, h, true)) {
        g_app.windows &= ~(uint32_t)WIN_SHOP;
        return;
    }
    char buf[96];
    float cx = x + 22, cy = y + 56;
    snprintf(buf, sizeof(buf), "Your gold: ^8%llu^0     Right-click your items to sell them", (unsigned long long)g_my_gold);
    ui_text_rich(FONT_BODY, 15, cx, cy, buf, COL_TEXT_DIM, ALIGN_LEFT);
    cy += 30;
    float rowh = 52;
    float off = ui_scroll_begin("shop", cx, cy, w - 44, h - (cy - y) - 18, (float)shop.count * rowh);
    for (int i = 0; i < shop.count; i++) {
        ShopItem *s = &shop.items[i];
        float ry = cy + off + (float)i * rowh;
        bool hov = ui_hover(cx, ry, w - 56, rowh - 4);
        ui_rrect(cx, ry, w - 56, rowh - 4, 5, hov ? UIC(70, 56, 40, 200) : UIC(30, 26, 34, 180));
        InvItem fake;
        memset(&fake, 0, sizeof(fake));
        snprintf(fake.name, sizeof(fake.name), "%s", s->name);
        fake.category = s->category;
        ui_icon(item_icon(&fake), cx + 6, ry + 6, 36, COL_WHITE);
        ui_text(FONT_BOLD, 16, cx + 50, ry + 6, s->name, COL_TEXT, ALIGN_LEFT);
        snprintf(buf, sizeof(buf), "%s - %s - stock %d", s->type, s->stats, s->stock);
        ui_text(FONT_BODY, 13, cx + 50, ry + 27, buf, COL_TEXT_FAINT, ALIGN_LEFT);
        bool afford = (uint64_t)s->cost <= g_my_gold && s->stock > 0;
        snprintf(buf, sizeof(buf), "%u gp", s->cost);
        ui_text(FONT_BOLD, 16, cx + w - 250, ry + rowh * 0.5f - 2, buf, afford ? COL_GOLD : COL_RED, ALIGN_RIGHT | ALIGN_MIDDLE);
        if (ui_button("Buy", cx + w - 236, ry + 9, 64, 30, afford ? BTN_PRIMARY : BTN_NORMAL)) {
            if (afford) app_send("buy %d", i + 1);
            else ui_toast(s->stock <= 0 ? "Out of stock." : "Not enough gold.", COL_RED);
        }
        if (ui_button("Haggle", cx + w - 166, ry + 9, 74, 30, BTN_NORMAL)) app_send("haggle buy %d", i + 1);
        if (ui_button("?", cx + w - 86, ry + 9, 30, 30, BTN_NORMAL)) app_send("browse %d", i + 1);
        if (hov && ui_input()->mx < cx + w - 250) ui_tooltip(s->name, "Buy it, try to haggle (Charisma) or read its details (?).");
    }
    ui_scroll_end();
}

/*--- map ---*/
static uint32_t tile_color(TileType t) {
    switch (t) {
    case VOXEL_WALL: return UIC(150, 140, 130, 255);
    case VOXEL_ROCK: return UIC(40, 36, 34, 255);
    case VOXEL_OBSIDIAN: return UIC(60, 40, 80, 255);
    case VOXEL_GOLD_VEIN: return UIC(200, 170, 60, 255);
    case VOXEL_DOOR: return UIC(170, 110, 50, 255);
    case VOXEL_STAIRS_DOWN: return UIC(255, 200, 60, 255);
    case VOXEL_STAIRS_UP: return UIC(90, 220, 255, 255);
    case VOXEL_GRASS: return UIC(60, 110, 50, 255);
    case VOXEL_WOOD: return UIC(100, 76, 50, 255);
    case VOXEL_WATER: return UIC(40, 90, 170, 255);
    case VOXEL_COBBLE: return UIC(90, 88, 84, 255);
    case VOXEL_TRAP: return UIC(200, 50, 40, 255);
    case VOXEL_LAVA: return UIC(230, 90, 20, 255);
    case VOXEL_ICE: return UIC(160, 200, 230, 255);
    case VOXEL_SAND: return UIC(170, 150, 100, 255);
    case VOXEL_ASH: return UIC(70, 70, 70, 255);
    case VOXEL_MUD: return UIC(80, 60, 40, 255);
    case VOXEL_MARBLE: return UIC(200, 200, 200, 255);
    case VOXEL_MUSHROOM_GLOW: return UIC(60, 220, 150, 255);
    default:
        if (t >= VOXEL_CRYSTAL_BLUE) {
            uint32_t c = crystal_color(t);
            return c;
        }
        return UIC(64, 60, 56, 255);
    }
}

/*Draw a window of the explored map, merging horizontal runs.*/
static void draw_map_area(float x, float y, float w, float h, int cx, int cy, float px_per_tile, bool full) {
    const uint8_t *ex = scene_explored();
    int half_w = (int)(w / px_per_tile / 2) + 1, half_h = (int)(h / px_per_tile / 2) + 1;
    ui_push_clip(x, y, w, h);
    for (int ty = cy - half_h; ty <= cy + half_h; ty++) {
        if (ty < 0 || ty >= MAP_HEIGHT) continue;
        int run_start = -1;
        uint32_t run_col = 0;
        for (int tx = cx - half_w; tx <= cx + half_w + 1; tx++) {
            uint32_t c = 0;
            bool ok = tx >= 0 && tx < MAP_WIDTH && tx <= cx + half_w && ex[ty * MAP_WIDTH + tx];
            if (ok) {
                TileType t = g_app.snap->map[ty][tx];
                c = tile_color(t);
                if (t == VOXEL_ROCK) ok = false;
            }
            if (!ok) c = 0;
            if (c != run_col || !ok) {
                if (run_start >= 0 && run_col) {
                    float rx = x + w * 0.5f + ((float)(run_start - cx) - 0.5f) * px_per_tile;
                    float ry = y + h * 0.5f + ((float)(ty - cy) - 0.5f) * px_per_tile;
                    ui_rect(rx, ry, (float)(tx - run_start) * px_per_tile + 0.3f, px_per_tile + 0.3f, run_col);
                }
                run_start = ok ? tx : -1;
                run_col = ok ? c : 0;
            }
        }
    }
    /*entities (within sight)*/
    for (int i = 0; i < CLIENT_MAX_ENTITIES; i++) {
        const Entity *e = &g_app.snap->entities[i];
        if (!e->active) continue;
        float ex2 = x + w * 0.5f + (float)(e->x - cx) * px_per_tile, ey2 = y + h * 0.5f + (float)(e->y - cy) * px_per_tile;
        uint32_t c = e->is_player ? COL_GREEN : (e->is_merchant ? COL_GOLD : (e->is_tombstone ? COL_PURPLE : COL_RED));
        if (e->kind == KIND_CHEST || e->kind == KIND_GOLD) c = UIC(255, 230, 120, 255);
        float r = fmaxf(2.5f, px_per_tile * 0.55f);
        ui_circle(ex2, ey2, r, UIC(0, 0, 0, 200));
        ui_circle(ex2, ey2, r * 0.75f, c);
        if (full && e->is_merchant && ui_hover(ex2 - r, ey2 - r, r * 2, r * 2)) ui_tooltip(e->username, "Merchant");
    }
    /*player arrow*/
    float px = x + w * 0.5f + (float)(g_app.snap->my_x - cx) * px_per_tile;
    float py = y + h * 0.5f + (float)(g_app.snap->my_y - cy) * px_per_tile;
    float yaw = scene_cam_yaw();
    float fx = -sinf(yaw), fy = -cosf(yaw);
    float s = fmaxf(6.0f, px_per_tile * 1.4f);
    ui_tri(px + fx * s, py + fy * s, px - fx * s * 0.6f - fy * s * 0.6f, py - fy * s * 0.6f + fx * s * 0.6f,
           px - fx * s * 0.6f + fy * s * 0.6f, py - fy * s * 0.6f - fx * s * 0.6f, COL_WHITE);
    ui_ring(px, py, s * 1.2f + 2.0f * sinf((float)g_app.time * 4), 1.5f, UIC(255, 255, 255, 120));
    ui_pop_clip();
}

static void find_known_stairs(int *dx, int *dy, int *ux, int *uy) {
    *dx = *dy = *ux = *uy = -1;
    const uint8_t *ex = scene_explored();
    for (int y = 0; y < MAP_HEIGHT; y++)
        for (int x = 0; x < MAP_WIDTH; x++) {
            if (!ex[y * MAP_WIDTH + x]) continue;
            TileType t = g_app.snap->map[y][x];
            if (t == VOXEL_STAIRS_DOWN) { *dx = x; *dy = y; }
            else if (t == VOXEL_STAIRS_UP) { *ux = x; *uy = y; }
        }
}

static void draw_map_window(void) {
    float W = ui_width(), H = ui_height();
    float w = fminf(W - 120, 1100), h = fminf(H - 120, 860), x = (W - w) * 0.5f, y = (H - h) * 0.5f;
    char tb[64];
    if (!ui_window(floor_title(g_app.snap->my_floor, tb, sizeof(tb)), x, y, w, h, true)) {
        g_app.windows &= ~(uint32_t)WIN_MAP;
        return;
    }
    static float zoom = 3.0f;
    static float pan_x, pan_y;
    float mx = x + 16, my = y + 52, mw = w - 32, mh = h - 110;
    ui_rect(mx, my, mw, mh, UIC(8, 6, 10, 255));
    if (ui_hover(mx, my, mw, mh)) {
        if (ui_input()->wheel != 0) zoom = clampf(zoom * powf(1.2f, ui_input()->wheel), 1.0f, 12.0f);
        static float lx, ly;
        if (ui_input()->down[0] && !ui_input()->pressed[0]) {
            pan_x -= (ui_input()->mx - lx) / zoom;
            pan_y -= (ui_input()->my - ly) / zoom;
        }
        lx = ui_input()->mx;
        ly = ui_input()->my;
    }
    int cx = g_app.snap->my_x + (int)pan_x, cy = g_app.snap->my_y + (int)pan_y;
    draw_map_area(mx, my, mw, mh, cx, cy, zoom, true);
    /*landmarks*/
    int sdx, sdy, sux, suy;
    find_known_stairs(&sdx, &sdy, &sux, &suy);
    if (sdx >= 0) {
        float sx = mx + mw * 0.5f + (float)(sdx - cx) * zoom, sy = my + mh * 0.5f + (float)(sdy - cy) * zoom;
        ui_icon(ICON_STAIRS, sx - 12, sy - 26, 24, COL_GOLD);
        ui_text_outline(FONT_BOLD, 13, sx, sy + 6, "Stairs down", COL_GOLD, UIC(0, 0, 0, 220), ALIGN_CENTER);
    }
    if (sux >= 0 && g_app.snap->my_floor > 0) {
        float sx = mx + mw * 0.5f + (float)(sux - cx) * zoom, sy = my + mh * 0.5f + (float)(suy - cy) * zoom;
        ui_text_outline(FONT_BOLD, 13, sx, sy + 6, "Stairs up", UIC(120, 220, 255, 255), UIC(0, 0, 0, 220), ALIGN_CENTER);
    }
    if (g_app.snap->my_floor == 0) {
        static const struct { int cls, x0, x1, y0, y1; const char *name; } T[] = {
            {11, 145, 155, 101, 111, "Arcane Academy"}, {6, 176, 186, 113, 123, "Cathedral of Justice"},
            {2, 189, 199, 145, 155, "Great Temple"}, {9, 176, 186, 176, 186, "Spire of Blood"},
            {10, 145, 155, 189, 199, "Altar of Pacts"}, {1, 113, 123, 176, 186, "Conservatory"},
            {3, 101, 111, 145, 155, "Sacred Grove"}, {7, 113, 123, 113, 123, "Hunter's Outpost"},
            {4, 145, 155, 67, 77, "Gladiator's Arena"}, {0, 223, 233, 145, 155, "Fighting Pit"},
            {8, 145, 155, 223, 233, "Den of Shadows"}, {5, 67, 77, 145, 155, "Lotus Dojo"},
        };
        for (size_t i = 0; i < sizeof(T) / sizeof(T[0]); i++) {
            float tx = (float)(T[i].x0 + T[i].x1) * 0.5f, ty = (float)(T[i].y0 + T[i].y1) * 0.5f;
            float sx = mx + mw * 0.5f + (tx - (float)cx) * zoom, sy = my + mh * 0.5f + (ty - (float)cy) * zoom;
            bool mine = T[i].cls == g_class_id;
            if (sx < mx || sy < my || sx > mx + mw || sy > my + mh) continue;
            if (!scene_explored()[(int)ty * MAP_WIDTH + (int)tx] && !mine) continue; /*not discovered yet*/
            ui_text_outline(FONT_BOLD, mine ? 15 : 12, sx, sy, T[i].name, mine ? COL_GOLD : COL_TEXT_DIM, UIC(0, 0, 0, 230), ALIGN_CENTER | ALIGN_MIDDLE);
            if (mine) ui_text_outline(FONT_BODY, 12, sx, sy + 16, "(your temple)", COL_GOLD, UIC(0, 0, 0, 230), ALIGN_CENTER | ALIGN_MIDDLE);
        }
    }
    /*right click on an explored tile: travel there*/
    if (ui_hover(mx, my, mw, mh)) {
        int tx = cx + (int)floorf((ui_input()->mx - (mx + mw * 0.5f)) / zoom + 0.5f);
        int ty = cy + (int)floorf((ui_input()->my - (my + mh * 0.5f)) / zoom + 0.5f);
        if (tx >= 0 && ty >= 0 && tx < MAP_WIDTH && ty < MAP_HEIGHT && scene_explored()[ty * MAP_WIDTH + tx]) {
            float hx = mx + mw * 0.5f + (float)(tx - cx) * zoom, hy = my + mh * 0.5f + (float)(ty - cy) * zoom;
            ui_ring(hx, hy, fmaxf(4.0f, zoom * 0.7f), 1.5f, UIC(255, 220, 140, 200));
            if (ui_input()->pressed[1] && app_travel_to(tx, ty)) g_app.windows &= ~(uint32_t)WIN_MAP;
        }
    }
    ui_text(FONT_BODY, 14, x + 20, y + h - 40, "Wheel: zoom   Drag: pan   Right click: travel there   M / Esc: close", COL_TEXT_FAINT,
            ALIGN_LEFT);
    if (sdx >= 0 && ui_button("Go to the stairs", x + w - 330, y + h - 50, 190, 34, BTN_PRIMARY)) {
        if (app_travel_to(sdx, sdy)) g_app.windows &= ~(uint32_t)WIN_MAP;
    }
    if (ui_button("Center", x + w - 130, y + h - 50, 110, 34, BTN_NORMAL)) { pan_x = pan_y = 0; }
}

/* ------------------------------------------------------------------ */
/* Help & settings (also used by the menus)                            */
/* ------------------------------------------------------------------ */

static const char *HELP_TABS[] = {"Basics", "Controls", "Combat", "Village", "Magic", "Commands"};

int hud_help_tabs(const char ***names) {
    *names = HELP_TABS;
    return 6;
}

static const char *HELP_TEXT[] = {
    /*Basics*/
    "^8Your goal^0\n"
    "Beneath the village lies the Dragon's Maw: 100 floors, each deeper and deadlier than the last. "
    "Every 10th floor is sealed by a ^8Guardian^0: defeat it to open the stairs. Wandering merchants trade on every 5th floor.\n\n"
    "^8Finding the way^0\n"
    "Each floor has ONE staircase down. Look for the ^3golden beam of light^0 rising from it in the darkness, and for the "
    "golden rune circle on the ground. Walk onto the stairs to descend; the cyan circle leads back up.\n\n"
    "^8Staying alive^0\n"
    "Your torch shows a limited radius: what you have seen stays on the map (M). When your health is low, use a potion "
    "(belt keys 1-4), ^8pray^0 (F when hurt) or climb back to the village: the ^8sacred fountain^0 heals you fully and the "
    "^8crystal statues^0 restore spells and cure hunger and exhaustion.\n\n"
    "^8Death^0\n"
    "If you fall, you wake up in the village without your equipment: a tombstone marks the spot where you died. "
    "Go back and retrieve your belongings!",
    /*Controls*/
    "^8Movement^0  W A S D or arrow keys (relative to the camera), numpad for diagonals, or ^8click on the ground^0.\n"
    "^8Camera^0  hold the right mouse button and drag to orbit, mouse wheel to zoom, Q / E to turn by 45 degrees, Home to reset.\n"
    "^8Attack^0  click a monster (you walk up to it and keep attacking), or Space for the nearest enemy, Tab to cycle targets.\n"
    "^8Interact^0  F: talk to merchants, drink from the fountain, meditate, disarm traps (the prompt above the hotbar tells you what F does).\n"
    "^8Items^0  G pick up, 1-4 use the quick belt, I inventory (left-click to use/equip, right-click for more).\n"
    "^8Magic^0  K spellbook, 5-0 hotbar spells.\n"
    "^8Windows^0  C character, M map, H or F1 this guide, Esc menu.\n"
    "^8Other^0  R rest, L look around, T search for traps, N toggle all name plates, Enter chat/commands, F12 screenshot.",
    /*Combat*/
    "^8How fights work^0\n"
    "Combat follows tabletop rules: every attack rolls a d20 plus your bonus against the target's Armor Class. "
    "Natural 20s are ^3critical hits^0 that double the damage dice. Hover a creature to see its level and health.\n\n"
    "^8Getting stronger^0\n"
    "Kills give experience: each level raises your maximum health and gives ^83 stat points^0 to spend in the Character sheet (C). "
    "Buy better weapons and armor in the village and equip them from the inventory.\n\n"
    "^8Conditions^0\n"
    "Poison, burning, bleeding, paralysis and more appear as icons under your health bar: hover them to see what they do. "
    "The temple can cure poison and curses. Watch your ^8hunger^0: eat rations (use them from the inventory).\n\n"
    "^8Traps^0\n"
    "Hidden traps lurk on many floors. Search (T) reveals adjacent ones, F disarms a revealed trap next to you.",
    /*Village*/
    "^8The Dragon's Maw Village (floor 0)^0 is a safe haven.\n\n"
    "^8Merchants^0  walk into a merchant (or press F next to them) to open the shop window. Prices change with stock; "
    "Charisma helps when you ^8haggle^0.\n"
    "^8Temples^0  every class has its own temple (shown on the map, M): read your spell books there to learn new spells.\n"
    "^8Fountain & statues^0  the sacred fountain heals, the crystal monuments restore your spell slots and your strength.\n"
    "^8The stairs^0  the way down into the Maw starts in the village too: follow the golden beam.",
    /*Magic*/
    "^8Learning spells^0\n"
    "Spell books (and the martial manuals for fighters, barbarians, monks and rogues) are sold in the village. "
    "Take the book to the temple of your class and choose ^8Read^0 on it in the inventory: you learn the spells your level allows.\n\n"
    "^8Casting^0\n"
    "Open the Spellbook (K) and press Cast, or bind spells to the hotbar keys 5-0. Each spell spends a slot of its level; "
    "cantrips are free. Slots refill when you rest in the village or meditate at the crystal statues.",
    /*Commands*/
    "Everything can also be typed: press Enter and write a command. A few useful ones:\n\n"
    "^8look^0  describe surroundings     ^8examine^0  study rock and minerals\n"
    "^8inventory^0  list items     ^8status^0  full character sheet\n"
    "^8say <text>^0  talk to players nearby     ^8msg <name> <text>^0  private message     ^8who^0  players online\n"
    "^8party^0 / ^8join^0 / ^8leave^0  groups     ^8top^0  leaderboard\n"
    "^8tunnel <n|s|e|w|d>^0  dig with a pick     ^8open^0 / ^8close <dir>^0  doors\n"
    "^8disarm^0 [salvage|magic]  traps     ^8fill lantern^0  refuel your light\n"
    "^8cure^0 / ^8uncurse^0  temple services     ^8identify <n>^0 / ^8repair <n>^0  shop services\n\n"
    "^9Type 'help' in the chat for the complete list from the server. While typing, suggestions appear above the input: Tab completes.",
};

void hud_help_content(float x, float y, float w, float h, int tab) {
    if (tab < 0 || tab >= 6) tab = 0;
    float th = ui_text_wrap_height(FONT_BODY, 17, w - 20, HELP_TEXT[tab], 24);
    float off = ui_scroll_begin("help", x, y, w, h, th + 10);
    ui_text_wrap(FONT_BODY, 17, x, y + off, w - 20, HELP_TEXT[tab], COL_TEXT, 24);
    ui_scroll_end();
}

void hud_settings_content(float x, float y, float w, float h) {
    (void)h;
    static int tab;
    const char *tabs[] = {"Video", "Audio", "Controls", "Interface"};
    float tx = x;
    for (int i = 0; i < 4; i++) {
        float bw = ui_text_width(FONT_BOLD, 16, tabs[i]) + 34;
        if (ui_button(tabs[i], tx, y, bw, 34, tab == i ? BTN_TAB_ACTIVE : BTN_TAB)) tab = i;
        tx += bw + 6;
    }
    float cy = y + 56;
    char buf[128];
    if (tab == 0) {
        ui_text(FONT_BODY, 17, x, cy + 12, "Renderer", COL_TEXT_DIM, ALIGN_MIDDLE);
        if (ui_button("OpenGL", x + 220, cy, 140, 30, g_settings.backend == 0 ? BTN_TAB_ACTIVE : BTN_TAB)) g_settings.backend = 0;
        bool vk = gfx_vk_available();
        if (ui_button(vk ? "Vulkan" : "Vulkan (n/a)", x + 366, cy, 160, 30, g_settings.backend == 1 ? BTN_TAB_ACTIVE : BTN_TAB) && vk)
            g_settings.backend = 1;
        if ((g_settings.backend == 1) != (g_app.gfx == &gfx_vk_backend))
            ui_text(FONT_BODY, 14, x + 540, cy + 15, "applies after a restart", COL_ORANGE, ALIGN_MIDDLE);
        cy += 50;
        if (ui_checkbox("Fullscreen", x, cy, &g_settings.fullscreen)) app_apply_video_settings();
        cy += 36;
        if (ui_checkbox("Vertical sync", x, cy, &g_settings.vsync)) app_apply_video_settings();
        cy += 46;
        ui_text(FONT_BODY, 17, x, cy + 12, "Quality", COL_TEXT_DIM, ALIGN_MIDDLE);
        const char *q[3] = {"Low", "Medium", "High"};
        for (int i = 0; i < 3; i++)
            if (ui_button(q[i], x + 220 + (float)i * 110, cy, 104, 30, g_settings.quality == i ? BTN_TAB_ACTIVE : BTN_TAB)) g_settings.quality = i;
        cy += 50;
        ui_text(FONT_BODY, 17, x, cy + 9, "Brightness", COL_TEXT_DIM, ALIGN_MIDDLE);
        ui_slider("bright", x + 220, cy, 300, &g_settings.brightness, 0.6f, 1.8f);
        cy += 46;
        ui_text(FONT_BODY, 17, x, cy + 9, "Interface size", COL_TEXT_DIM, ALIGN_MIDDLE);
        ui_slider("uiscale", x + 220, cy, 300, &g_settings.ui_scale, 0.7f, 1.6f);
        snprintf(buf, sizeof(buf), "%d%%", (int)(g_settings.ui_scale * 100 + 0.5f));
        ui_text(FONT_BODY, 15, x + 540, cy + 9, buf, COL_TEXT, ALIGN_MIDDLE);
        cy += 56;
        ui_text_wrap(FONT_BODY, 14, x, cy, w, g_app.gfx->device_name(), COL_TEXT_FAINT, 18);
    } else if (tab == 1) {
        struct { const char *l, *id; float *v; } S[] = {
            {"Master volume", "vm", &g_settings.vol_master}, {"Music", "vmu", &g_settings.vol_music}, {"Sound effects", "vs", &g_settings.vol_sfx}};
        for (int i = 0; i < 3; i++) {
            ui_text(FONT_BODY, 17, x, cy + 9, S[i].l, COL_TEXT_DIM, ALIGN_MIDDLE);
            if (ui_slider(S[i].id, x + 220, cy, 300, S[i].v, 0, 1) && i == 2) audio_sfx(SFX_CLICK, 0.6f, 0);
            snprintf(buf, sizeof(buf), "%d%%", (int)(*S[i].v * 100 + 0.5f));
            ui_text(FONT_BODY, 15, x + 540, cy + 9, buf, COL_TEXT, ALIGN_MIDDLE);
            cy += 46;
        }
        ui_checkbox("Mute when the window is in the background", x, cy, &g_settings.mute_unfocused);
        cy += 46;
        ui_text(FONT_BODY, 15, x, cy, audio_available() ? "The whole soundtrack is synthesized live by the game." : "No audio device found: the game runs silent.",
                COL_TEXT_FAINT, ALIGN_LEFT);
    } else if (tab == 2) {
        ui_text(FONT_BODY, 17, x, cy + 9, "Camera sensitivity", COL_TEXT_DIM, ALIGN_MIDDLE);
        ui_slider("camsens", x + 220, cy, 300, &g_settings.cam_sensitivity, 0.3f, 2.5f);
        cy += 46;
        ui_checkbox("Movement keys follow the camera direction", x, cy, &g_settings.cam_relative_move);
        cy += 36;
        ui_checkbox("Click on the ground to walk (click-to-move)", x, cy, &g_settings.click_to_move);
        cy += 36;
        ui_checkbox("Invert mouse-wheel zoom", x, cy, &g_settings.invert_zoom);
        cy += 50;
        ui_text_wrap(FONT_BODY, 15, x, cy, w, HELP_TEXT[1], COL_TEXT_DIM, 20);
    } else {
        ui_checkbox("Show damage numbers", x, cy, &g_settings.show_damage_numbers);
        cy += 36;
        ui_checkbox("Always show names and health bars (N)", x, cy, &g_settings.show_all_names);
        cy += 36;
        ui_checkbox("Show tips for new players", x, cy, &g_settings.show_hints);
        cy += 36;
        ui_checkbox("Show the minimap", x, cy, &g_settings.show_minimap);
    }
}

/* ------------------------------------------------------------------ */
/* HUD pieces                                                          */
/* ------------------------------------------------------------------ */

static const struct { int bit; const char *name, *desc; uint32_t col; } STATUSES[] = {
    {0, "Poisoned", "Loses health over time. The temple can cure it ('cure').", UIC(120, 220, 80, 255)},
    {1, "Blinded", "Attacks at a disadvantage.", UIC(160, 160, 160, 255)},
    {2, "Paralyzed", "Cannot move or act.", UIC(240, 220, 90, 255)},
    {3, "Stunned", "Cannot move or act for a moment.", UIC(240, 200, 120, 255)},
    {4, "Unconscious", "Helpless!", UIC(200, 120, 120, 255)},
    {5, "Burning", "Fire damage every turn.", UIC(255, 130, 40, 255)},
    {6, "Bleeding", "Moving reopens your wounds.", UIC(220, 40, 40, 255)},
    {7, "Petrified", "Turned to stone.", UIC(170, 160, 150, 255)},
    {8, "Cursed", "Bad luck follows you. The temple can lift it ('uncurse').", UIC(170, 90, 220, 255)},
    {9, "Frozen", "Cannot move; extra damage taken.", UIC(140, 210, 255, 255)},
    {10, "Exhausted", "Weakened. Rest or meditate at the crystal statues.", UIC(200, 170, 120, 255)},
    {11, "Ready to study", "New spells/techniques can be learned in your temple.", UIC(120, 180, 255, 255)},
};

static void draw_portrait(void) {
    float x = 18, y = 16, w = 420;
    ui_block(x, y, w, 128);
    ui_rrect_grad(x, y, w, 112, 10, UIC(24, 20, 28, 210), UIC(12, 10, 14, 200));
    ui_rrect_line(x, y, w, 112, 10, 1.2f, UIC(176, 138, 70, 220));
    /*emblem*/
    float ex = x + 54, ey = y + 56;
    ui_circle(ex, ey, 40, UIC(40, 30, 24, 255));
    ui_ring(ex, ey, 40, 3, COL_GOLD);
    static const int cls_icon[CLASS_COUNT] = {ICON_AXE, ICON_STAR, ICON_SUN, ICON_GEM, ICON_SWORD, ICON_HAND,
                                              ICON_SHIELD, ICON_BOW, ICON_DAGGER, ICON_MANA, ICON_EYE, ICON_STAFF};
    ui_icon(g_class_id >= 0 && g_class_id < CLASS_COUNT ? cls_icon[g_class_id] : ICON_SWORD, ex - 24, ey - 26, 48, COL_WHITE);
    char buf[96];
    snprintf(buf, sizeof(buf), "%d", g_my_level);
    ui_circle(ex + 28, ey + 28, 14, UIC(120, 80, 20, 255));
    ui_ring(ex + 28, ey + 28, 14, 2, COL_GOLD);
    ui_text(FONT_BOLD, 15, ex + 28, ey + 28, buf, COL_WHITE, ALIGN_CENTER | ALIGN_MIDDLE);
    if (ui_hover(ex - 40, ey - 40, 80, 80)) ui_tooltip("Character (C)", "Level, abilities, stat points and guardians defeated.");
    if (ui_clicked(ex - 40, ey - 40, 80, 80, 0)) hud_toggle(WIN_CHARACTER);
    float tx = x + 108;
    ui_text_shadow(FONT_BOLD, 20, tx, y + 10, g_my_name, COL_GOLD, ALIGN_LEFT);
    const char *cls = (g_class_id >= 0 && g_class_id < CLASS_COUNT) ? CLASSES[g_class_id].name : "";
    const char *race = (g_race_id >= 0 && g_race_id < RACE_COUNT) ? RACES[g_race_id].name : "";
    snprintf(buf, sizeof(buf), "%s %s", race, cls);
    ui_text(FONT_BODY, 14, x + w - 14, y + 14, buf, COL_TEXT_DIM, ALIGN_RIGHT);
    float bw = w - 124;
    snprintf(buf, sizeof(buf), "%d / %d", g_my_hp, g_my_max_hp);
    float hpf = (float)g_my_hp / (float)(g_my_max_hp > 0 ? g_my_max_hp : 1);
    static float shown_hp = 1;
    shown_hp = damp(shown_hp, hpf, 3.0f, g_app.dt);
    ui_rrect(tx, y + 40, bw * clampf(shown_hp, 0, 1), 20, 6, UIC(255, 220, 200, 120));
    ui_bar(tx, y + 40, bw, 20, hpf, hpf < 0.25f ? UIC(255, 70, 50, 255) : UIC(210, 50, 45, 255), UIC(110, 18, 18, 255), buf);
    int prev = 0;
    static const int TH[] = {0, 300, 900, 2700, 6500, 14000, 23000, 34000, 48000, 64000, 85000, 100000, 120000, 140000, 165000, 195000, 225000, 265000, 305000, 355000};
    if (g_my_level >= 1 && g_my_level <= 20) prev = TH[g_my_level - 1];
    float xpf = g_my_xp_next > prev ? (float)(g_my_xp - prev) / (float)(g_my_xp_next - prev) : 1.0f;
    ui_bar(tx, y + 66, bw, 9, xpf, UIC(250, 210, 90, 255), UIC(150, 110, 30, 255), NULL);
    if (ui_hover(tx, y + 64, bw, 12)) {
        snprintf(buf, sizeof(buf), "%d / %d experience", g_my_xp, g_my_xp_next);
        ui_tooltip("Experience", buf);
    }
    /*status chips*/
    float sx = tx;
    float sy = y + 84;
    const char *hunger = NULL;
    uint32_t hc = COL_TEXT;
    if (g_hunger_level >= 1850) { hunger = "Fainting"; hc = COL_RED; }
    else if (g_hunger_level >= 1600) { hunger = "Weak"; hc = COL_ORANGE; }
    else if (g_hunger_level >= 1200) { hunger = "Hungry"; hc = UIC(240, 220, 90, 255); }
    if (hunger) {
        float cw = ui_text_width(FONT_BOLD, 13, hunger) + 30;
        ui_rrect(sx, sy, cw, 20, 5, UIC(40, 30, 20, 230));
        ui_icon(ICON_FOOD, sx + 3, sy + 2, 16, COL_WHITE);
        ui_text(FONT_BOLD, 13, sx + 22, sy + 10, hunger, hc, ALIGN_MIDDLE);
        if (ui_hover(sx, sy, cw, 20)) ui_tooltip(hunger, "Eat something: right-click food in the inventory, or type 'eat'.");
        sx += cw + 4;
    }
    for (size_t i = 0; i < sizeof(STATUSES) / sizeof(STATUSES[0]); i++) {
        if (!(g_status_icons & (1u << STATUSES[i].bit))) continue;
        float cw = ui_text_width(FONT_BOLD, 13, STATUSES[i].name) + 14;
        if (sx + cw > x + w - 6) break;
        ui_rrect(sx, sy, cw, 20, 5, UIC(30, 24, 34, 230));
        ui_rrect_line(sx, sy, cw, 20, 5, 1, STATUSES[i].col);
        ui_text(FONT_BOLD, 13, sx + 7, sy + 10, STATUSES[i].name, STATUSES[i].col, ALIGN_MIDDLE);
        if (ui_hover(sx, sy, cw, 20)) ui_tooltip_color(STATUSES[i].name, STATUSES[i].col, STATUSES[i].desc);
        sx += cw + 4;
    }
    if (g_stat_points > 0) {
        snprintf(buf, sizeof(buf), "+%d stat points (C)", g_stat_points);
        float pulse = 0.6f + 0.4f * sinf((float)g_app.time * 4);
        ui_text_shadow(FONT_BOLD, 14, x + 12, y + 120, buf, ui_alpha(COL_GREEN, pulse), ALIGN_LEFT);
    }
}

static void draw_location(void) {
    float W = ui_width();
    char tb[64], buf[128];
    const char *title = floor_title(g_app.snap->my_floor, tb, sizeof(tb));
    float tw = fmaxf(ui_text_width(FONT_TITLE, 20, title), 220) + 70;
    float x = W * 0.5f - tw * 0.5f, y = 10;
    ui_rect_hgrad(x - 60, y, tw * 0.5f + 60, 54, UIC(10, 8, 12, 0), UIC(10, 8, 12, 190));
    ui_rect_hgrad(x + tw * 0.5f, y, tw * 0.5f + 60, 54, UIC(10, 8, 12, 190), UIC(10, 8, 12, 0));
    ui_text_shadow(FONT_TITLE, 20, W * 0.5f, y + 5, title, COL_GOLD, ALIGN_CENTER);
    bool day = g_app.snap->game_h >= 6 && g_app.snap->game_h < 20;
    if (g_app.snap->my_floor == 0)
        snprintf(buf, sizeof(buf), "%02d:%02d  -  %s", g_app.snap->game_h, g_app.snap->game_m, day ? "Day" : "Night");
    else
        snprintf(buf, sizeof(buf), "Floor %d  -  %d ft  -  %02d:%02d", g_app.snap->my_floor, g_app.snap->my_floor * 50,
                 g_app.snap->game_h, g_app.snap->game_m);
    ui_icon(day ? ICON_SUN : ICON_MOON, W * 0.5f - ui_text_width(FONT_BODY, 14, buf) * 0.5f - 24, y + 31, 18, COL_WHITE);
    ui_text(FONT_BODY, 14, W * 0.5f, y + 33, buf, COL_TEXT_DIM, ALIGN_CENTER);
    /*gold top-right of the banner*/
}

static void draw_minimap(void) {
    if (!g_settings.show_minimap) return;
    float W = ui_width();
    float s = 240, x = W - s - 18, y = 18;
    ui_block(x, y, s, s + 34);
    ui_circle(x + s * 0.5f, y + s * 0.5f, s * 0.5f + 6, UIC(10, 8, 12, 220));
    ui_rect(x, y, s, s, UIC(0, 0, 0, 0));
    ui_rrect(x, y, s, s, 12, UIC(8, 6, 10, 210));
    draw_map_area(x + 4, y + 4, s - 8, s - 8, g_app.snap->my_x, g_app.snap->my_y, 4.0f, false);
    /*known stairs indicator at the border*/
    int sdx, sdy, sux, suy;
    find_known_stairs(&sdx, &sdy, &sux, &suy);
    if (sdx >= 0) {
        float dx = (float)(sdx - g_app.snap->my_x) * 4.0f, dy = (float)(sdy - g_app.snap->my_y) * 4.0f;
        float lim = s * 0.5f - 14;
        float m = fmaxf(fabsf(dx), fabsf(dy));
        if (m > lim) { dx *= lim / m; dy *= lim / m; }
        ui_icon(ICON_STAIRS, x + s * 0.5f + dx - 10, y + s * 0.5f + dy - 10, 20, COL_GOLD);
    }
    ui_rrect_line(x, y, s, s, 12, 1.5f, UIC(176, 138, 70, 230));
    if (ui_hover(x, y, s, s)) ui_tooltip("Minimap", "Click (or press M) for the full map.");
    if (ui_clicked(x, y, s, s, 0)) hud_toggle(WIN_MAP);
    /*gold under the minimap*/
    char buf[48];
    snprintf(buf, sizeof(buf), "%llu", (unsigned long long)g_my_gold);
    ui_rrect(x, y + s + 6, s, 28, 6, UIC(12, 10, 14, 200));
    ui_icon(ICON_COIN, x + 8, y + s + 9, 22, COL_WHITE);
    ui_text(FONT_BOLD, 17, x + 36, y + s + 20, buf, COL_GOLD, ALIGN_MIDDLE);
    snprintf(buf, sizeof(buf), "AC %d", g_my_ac);
    ui_icon(ICON_SHIELD, x + s - 86, y + s + 9, 22, COL_WHITE);
    ui_text(FONT_BOLD, 17, x + s - 60, y + s + 20, buf, COL_TEXT, ALIGN_MIDDLE);
}

static void draw_objective(void) {
    float W = ui_width();
    float x = W - 258 - 18, y = 18 + 240 + 44, w = 258;
    char line[160];
    const char *head = "Objective";
    int f = g_app.snap->my_floor;
    if (f == 0 && g_max_floor == 0) snprintf(line, sizeof(line), "Gear up in the village, then find the stairs down (golden beam).");
    else if (f > 0 && f % 10 == 0 && !(g_bosses_defeated & (1u << (f / 10 - 1))))
        snprintf(line, sizeof(line), "Defeat the Guardian of floor %d to unseal the stairs.", f);
    else if (g_status_icons & (1u << 11))
        snprintf(line, sizeof(line), "You can learn new spells: read your books in your temple (village).");
    else if (f == 0) snprintf(line, sizeof(line), "Your deepest floor: %d. The stairs down lead back into the Maw.", g_max_floor);
    else snprintf(line, sizeof(line), "Find the staircase down to floor %d.", f + 1);
    float h = ui_text_wrap_height(FONT_BODY, 15, w - 24, line, 19) + 34;
    ui_rrect(x, y, w, h, 8, UIC(12, 10, 14, 170));
    ui_text(FONT_TITLE, 14, x + 12, y + 7, head, COL_GOLD, ALIGN_LEFT);
    ui_text_wrap(FONT_BODY, 15, x + 12, y + 26, w - 24, line, COL_TEXT_DIM, 19);
}

static void draw_hotbar(void) {
    float W = ui_width(), H = ui_height();
    float cs = 58, gap = 6;
    int n = 4 + HOTBAR_SPELLS;
    float bw = (float)n * (cs + gap) + 30 + 6 * 46;
    float x = W * 0.5f - bw * 0.5f, y = H - cs - 22;
    ui_block(x - 10, y - 10, bw + 20, cs + 26);
    ui_rrect_grad(x - 10, y - 10, bw + 20, cs + 22, 10, UIC(26, 22, 30, 215), UIC(12, 10, 14, 225));
    ui_rrect_line(x - 10, y - 10, bw + 20, cs + 22, 10, 1.2f, UIC(176, 138, 70, 220));
    hotbar_load();
    for (int i = 0; i < 4; i++) {
        float cx = x + (float)i * (cs + gap);
        const InvItem *it = inv_find(INV_BELT, i);
        slot_cell(cx, y, cs, it, false);
        char k[4];
        snprintf(k, sizeof(k), "%d", i + 1);
        ui_text_outline(FONT_BOLD, 13, cx + 5, y + 3, k, COL_GOLD, UIC(0, 0, 0, 220), ALIGN_LEFT);
        if (it) item_cell_interact(cx, y, cs, it, "Click or press the number key to use");
        else if (ui_hover(cx, y, cs, cs)) ui_tooltip("Quick belt", "Right-click a potion or food in the inventory and choose 'Put on belt'.");
    }
    float sx = x + 4 * (cs + gap) + 14;
    for (int i = 0; i < HOTBAR_SPELLS; i++) {
        float cx = sx + (float)i * (cs + gap);
        bool has = g_app.spell_hotbar[i][0] != 0;
        ui_rrect_grad(cx, y, cs, cs, 6, UIC(26, 30, 50, 235), UIC(14, 14, 26, 235));
        ui_rrect_line(cx, y, cs, cs, 6, 1.2f, has ? UIC(120, 150, 255, 255) : UIC(60, 60, 90, 200));
        char k[4];
        snprintf(k, sizeof(k), "%d", (5 + i) % 10);
        ui_text_outline(FONT_BOLD, 13, cx + 5, y + 3, k, UIC(150, 180, 255, 255), UIC(0, 0, 0, 220), ALIGN_LEFT);
        if (has) {
            ui_icon(ICON_STAR, cx + cs * 0.22f, y + cs * 0.14f, cs * 0.56f, COL_WHITE);
            char abbr[8];
            snprintf(abbr, sizeof(abbr), "%.6s", g_app.spell_hotbar[i]);
            ui_text_outline(FONT_BOLD, 11, cx + cs * 0.5f, y + cs - 15, abbr, COL_TEXT, UIC(0, 0, 0, 220), ALIGN_CENTER);
        }
        if (ui_hover(cx, y, cs, cs)) ui_tooltip(has ? g_app.spell_hotbar[i] : "Spell slot", has ? "Click or press the key to cast." : "Assign spells from the Spellbook (K).");
        if (ui_clicked(cx, y, cs, cs, 0)) hud_cast_hotbar(i);
    }
    /*window buttons*/
    float bx = sx + (float)HOTBAR_SPELLS * (cs + gap) + 10, bs = 40;
    struct { int icon; const char *tip; uint32_t win; } B[] = {
        {ICON_BAG, "Inventory (I)", WIN_INVENTORY}, {ICON_HELM, "Character (C)", WIN_CHARACTER},
        {ICON_BOOK, "Spellbook (K)", WIN_SPELLS},   {ICON_MAP, "Map (M)", WIN_MAP},
        {ICON_QUESTION, "Guide (H)", WIN_HELP},    {ICON_GEAR, "Menu (Esc)", WIN_PAUSE},
    };
    for (int i = 0; i < 6; i++) {
        float cx = bx + (float)(i % 3) * (bs + 6), cy = y - 6 + (float)(i / 3) * (bs - 8 + 6);
        if (ui_button_icon(B[i].icon, B[i].tip, cx, cy, bs - 8, (g_app.windows & B[i].win) != 0)) hud_toggle(B[i].win);
    }
}

/*--- context action ---*/
static int g_ctx_kind;
enum { CA_NONE, CA_MERCHANT, CA_FOUNTAIN, CA_STATUE, CA_TRAP, CA_PRAY, CA_LOOT };
static int g_ctx_target;

const char *hud_context_action(int *icon) {
    static char label[96];
    FrameSnapshot *s = g_app.snap;
    g_ctx_kind = CA_NONE;
    int px = s->my_x, py = s->my_y;
    for (int i = 0; i < CLIENT_MAX_ENTITIES; i++) {
        const Entity *e = &s->entities[i];
        if (!e->active || abs(e->x - px) > 1 || abs(e->y - py) > 1 || e->id == s->my_entity_id) continue;
        if (e->is_merchant) {
            snprintf(label, sizeof(label), "Trade with %s", e->username[0] ? e->username : "the merchant");
            g_ctx_kind = CA_MERCHANT; g_ctx_target = e->id; *icon = ICON_COIN;
            return label;
        }
        if (e->kind == KIND_CHEST || e->kind == KIND_GOLD) {
            snprintf(label, sizeof(label), e->kind == KIND_CHEST ? "Open the chest" : "Collect the gold");
            g_ctx_kind = CA_LOOT; g_ctx_target = e->id; *icon = ICON_CHEST;
            return label;
        }
    }
    bool water = false, crystal = false, trap = false;
    for (int dy = -3; dy <= 3; dy++)
        for (int dx = -3; dx <= 3; dx++) {
            int x = px + dx, y = py + dy;
            if (x < 0 || y < 0 || x >= MAP_WIDTH || y >= MAP_HEIGHT) continue;
            TileType t = s->map[y][x];
            if (t == VOXEL_WATER || t == VOXEL_ICE) water = true;
            if (t >= VOXEL_CRYSTAL_BLUE && t != VOXEL_GOLD_VEIN && t != VOXEL_OBSIDIAN) crystal = true;
            if (t == VOXEL_TRAP && abs(dx) <= 1 && abs(dy) <= 1) trap = true;
        }
    if (trap) { g_ctx_kind = CA_TRAP; *icon = ICON_HAND; return "Disarm the trap"; }
    if (s->my_floor == 0 && crystal) { g_ctx_kind = CA_STATUE; *icon = ICON_GEM; return "Meditate at the crystal statue"; }
    if (s->my_floor == 0 && water && s->hp < s->max_hp) { g_ctx_kind = CA_FOUNTAIN; *icon = ICON_HEART; return "Drink from the sacred fountain"; }
    if (s->my_floor > 0 && s->max_hp > 0 && s->hp * 10 < s->max_hp * 3) { g_ctx_kind = CA_PRAY; *icon = ICON_SUN; return "Pray to the gods for help"; }
    return NULL;
}

void hud_do_context_action(void) {
    int icon;
    if (!hud_context_action(&icon)) return;
    FrameSnapshot *s = g_app.snap;
    switch (g_ctx_kind) {
    case CA_MERCHANT:
    case CA_LOOT:
        for (int i = 0; i < CLIENT_MAX_ENTITIES; i++) {
            const Entity *e = &s->entities[i];
            if (e->active && e->id == g_ctx_target) { client_send_move(e->x - s->my_x, e->y - s->my_y); break; }
        }
        break;
    case CA_FOUNTAIN: app_send("fountain"); audio_sfx(SFX_HEAL, 0.5f, 0); break;
    case CA_STATUE: app_send("statue"); audio_sfx(SFX_SPELL, 0.5f, 0); break;
    case CA_TRAP: app_send("disarm"); break;
    case CA_PRAY: app_send("pray"); break;
    default: break;
    }
}

static void draw_context_prompt(void) {
    int icon = 0;
    const char *a = hud_context_action(&icon);
    if (!a) return;
    float W = ui_width(), H = ui_height();
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", a);
    float tw = ui_text_width(FONT_BOLD, 18, buf) + 90;
    float x = W * 0.5f - tw * 0.5f, y = H - 150;
    float pulse = 0.75f + 0.25f * sinf((float)g_app.time * 3);
    ui_rrect(x, y, tw, 40, 8, UIC(20, 16, 22, 220));
    ui_rrect_line(x, y, tw, 40, 8, 1.4f, ui_alpha(COL_GOLD, pulse));
    ui_rrect(x + 8, y + 7, 26, 26, 5, UIC(90, 66, 30, 255));
    ui_text(FONT_BOLD, 16, x + 21, y + 20, "F", COL_WHITE, ALIGN_CENTER | ALIGN_MIDDLE);
    ui_icon(icon, x + 40, y + 8, 24, COL_WHITE);
    ui_text_shadow(FONT_BOLD, 18, x + 70, y + 20, buf, COL_TEXT, ALIGN_MIDDLE);
    ui_block(x, y, tw, 40);
    if (ui_clicked(x, y, tw, 40, 0)) hud_do_context_action();
}

/*--- name plates, target frame, floating text ---*/
static void draw_plates(void) {
    const SceneLabel *L;
    int n = scene_labels(&L);
    for (int i = 0; i < n; i++) {
        const SceneLabel *l = &L[i];
        bool focus = l->id == g_app.hover_id || l->id == g_app.target_id;
        bool show_name = focus || g_settings.show_all_names || l->is_player || l->is_merchant || l->is_boss;
        bool show_bar = l->hostile && (focus || g_settings.show_all_names || l->hp < l->max_hp || l->is_boss);
        if (!show_name && !show_bar) continue;
        float a = clampf(1.6f - l->dist / 14.0f, 0.35f, 1.0f);
        if (focus) a = 1;
        float y = l->sy - 8;
        if (show_bar) {
            float bw = l->is_boss ? 110 : 70;
            ui_rect(l->sx - bw * 0.5f - 1, y - 1, bw + 2, 8, UIC(0, 0, 0, (int)(200 * a)));
            ui_rect_grad(l->sx - bw * 0.5f, y, bw * l->hp_frac, 6, ui_alpha(UIC(240, 70, 60, 255), a), ui_alpha(UIC(140, 20, 20, 255), a));
            y -= 4;
        }
        if (show_name && l->name[0]) {
            uint32_t c = l->is_player ? UIC(140, 240, 140, 255) : (l->is_merchant ? COL_GOLD : (l->is_boss ? UIC(255, 120, 80, 255) : COL_TEXT));
            char nm[64];
            if (l->hostile && l->level > 0) snprintf(nm, sizeof(nm), "%s  ^9Lv %d", l->name, l->level);
            else snprintf(nm, sizeof(nm), "%s", l->name);
            float fs = focus ? 16 : 14;
            float tw = ui_text_width(FONT_BOLD, fs, nm);
            ui_rrect(l->sx - tw * 0.5f - 6, y - fs - 6, tw + 12, fs + 6, 4, UIC(0, 0, 0, (int)(120 * a)));
            ui_text_rich(FONT_BOLD, fs, l->sx, y - fs - 4, nm, ui_alpha(c, a), ALIGN_CENTER);
        }
    }
}

static void draw_target_frame(void) {
    int id = g_app.hover_id ? g_app.hover_id : g_app.target_id;
    if (!id) return;
    const SceneLabel *L;
    int n = scene_labels(&L);
    const SceneLabel *l = NULL;
    for (int i = 0; i < n; i++)
        if (L[i].id == id) l = &L[i];
    if (!l || !l->hostile) return;
    float W = ui_width();
    float w = 380, x = W * 0.5f - w * 0.5f, y = 72;
    ui_rrect_grad(x, y, w, 58, 8, UIC(30, 20, 22, 220), UIC(14, 10, 12, 220));
    ui_rrect_line(x, y, w, 58, 8, 1.2f, l->is_boss ? UIC(255, 120, 80, 255) : UIC(150, 70, 60, 230));
    static const char *arch[] = {"Warrior", "Spellcaster", "Assassin", "Brute", "Dragon", "GUARDIAN", "", "", "", "Swarm"};
    char buf[96];
    snprintf(buf, sizeof(buf), "%s", l->name);
    ui_text(FONT_BOLD, 17, x + 14, y + 7, buf, l->is_boss ? UIC(255, 140, 90, 255) : COL_TEXT, ALIGN_LEFT);
    snprintf(buf, sizeof(buf), "Lv %d  %s", l->level, (l->archetype >= 0 && l->archetype < 10) ? arch[l->archetype] : "");
    ui_text(FONT_BODY, 14, x + w - 14, y + 9, buf, COL_TEXT_DIM, ALIGN_RIGHT);
    snprintf(buf, sizeof(buf), "%d / %d", l->hp, l->max_hp);
    ui_bar(x + 14, y + 32, w - 28, 16, l->hp_frac, UIC(230, 60, 50, 255), UIC(120, 20, 20, 255), buf);
}

static void draw_float_texts(void) {
    if (!g_settings.show_damage_numbers) return;
    ScreenText st[64];
    int n = scene_screen_texts(st, 64);
    for (int i = 0; i < n; i++) {
        uint32_t c;
        float fs = 20;
        switch (st[i].type) {
        case FT_DAMAGE_IN: c = UIC(255, 80, 70, 255); fs = 22; break;
        case FT_DAMAGE_OUT: c = UIC(255, 210, 120, 255); break;
        case FT_HEAL: c = UIC(110, 255, 120, 255); break;
        case FT_CRIT: c = UIC(255, 240, 60, 255); fs = 30; break;
        case FT_XP: c = UIC(140, 200, 255, 255); fs = 16; break;
        case FT_GOLD: c = UIC(255, 215, 80, 255); fs = 18; break;
        case FT_MISS: c = UIC(200, 200, 210, 255); fs = 16; break;
        case FT_LEVEL: c = UIC(255, 230, 120, 255); fs = 34; break;
        default: c = COL_TEXT; break;
        }
        ui_text_outline(st[i].type == FT_LEVEL ? FONT_TITLE : FONT_BOLD, fs * st[i].scale, st[i].sx, st[i].sy, st[i].text,
                        ui_alpha(c, st[i].alpha), UIC(0, 0, 0, (int)(220 * st[i].alpha)), ALIGN_CENTER | ALIGN_MIDDLE);
    }
}

/*--- chat / log ---*/
static const struct { const char *cmd, *desc; } CMDS[] = {
    {"look", "describe what is around you"}, {"examine", "study rock, veins and minerals nearby"},
    {"inventory", "list everything you carry"}, {"status", "full character sheet"},
    {"get", "pick up items here"}, {"search", "look for hidden traps"},
    {"rest", "rest to recover"}, {"pray", "beg the gods for help"},
    {"say ", "talk to players nearby"}, {"msg ", "private message: msg <name> <text>"},
    {"who", "players online"}, {"party", "party status / invite"},
    {"join", "join a party"}, {"leave", "leave the party"},
    {"top", "leaderboard"}, {"spells", "list known spells"},
    {"cast ", "cast <spell name>"}, {"use ", "use <n|name>"},
    {"wear ", "equip <n|name>"}, {"takeoff ", "remove <slot|name>"},
    {"drop ", "drop <n>"}, {"belt ", "put <n> on the quick belt"},
    {"read ", "read a book or scroll"}, {"lore ", "inspect an item"},
    {"buy ", "buy <n> from a merchant"}, {"sell ", "sell <n>"},
    {"haggle buy ", "negotiate a price"}, {"browse ", "item details in a shop"},
    {"list", "merchant stock"}, {"talk", "talk to a merchant"},
    {"identify ", "identify an item (shop)"}, {"repair ", "repair an item (smith)"},
    {"cure", "cure poison/disease (temple)"}, {"uncurse", "remove curses (temple)"},
    {"fountain", "drink from the sacred fountain"}, {"statue", "meditate at the crystal statue"},
    {"tunnel ", "dig: tunnel <n|s|e|w|d>"}, {"open ", "open a door <dir>"},
    {"close ", "close a door <dir>"}, {"disarm", "disarm an adjacent trap"},
    {"train ", "spend stat points: train <str|dex|...>"}, {"fill lantern", "refuel your lantern"},
    {"throw ", "throw <n> <dir>"}, {"retrieve", "recover your lost belongings"},
    {"block ", "block a player"}, {"unblock ", "unblock a player"},
    {"save", "save the game"}, {"help", "server command list"},
};

static void draw_chat(void) {
    float H = ui_height();
    float x = 18, w = 640;
    bool open = g_app.chat_open;
    int lines = open ? 16 : 8;
    float lh = 21;
    float h = (float)lines * lh + 16;
    float y = H - h - (open ? 62 : 22);
    bool hov = ui_hover(x, y, w, h);
    static double last_activity;
    pthread_mutex_lock(&g_state_mutex);
    uint32_t total = g_log_total;
    pthread_mutex_unlock(&g_state_mutex);
    static uint32_t seen_total;
    if (total != seen_total) { seen_total = total; last_activity = g_app.time; }
    float idle = (float)(g_app.time - last_activity);
    float panel_a = open || hov ? 1.0f : clampf(1.0f - (idle - 10.0f) / 3.0f, 0.0f, 1.0f);
    if (open || hov) ui_rrect(x, y, w, h, 8, UIC(8, 6, 10, 180));
    else if (panel_a > 0.01f) ui_rect_hgrad(x, y + h * 0.45f, w, h * 0.55f, UIC(8, 6, 10, (int)(110 * panel_a)), UIC(8, 6, 10, 0));
    if (open || hov) ui_block(x, y, w, h);
    static int scroll;
    if ((open || hov) && ui_input()->wheel != 0) scroll += (int)ui_input()->wheel * 3;
    int maxscroll = (int)(total > LOG_CAP ? LOG_CAP : total) - lines;
    if (maxscroll < 0) maxscroll = 0;
    if (scroll > maxscroll) scroll = maxscroll;
    if (scroll < 0) scroll = 0;
    if (!open && !hov) scroll = 0;
    ui_push_clip(x + 6, y + 6, w - 12, h - 12);
    for (int i = 0; i < lines; i++) {
        long idx = (long)total - 1 - scroll - i;
        if (idx < 0 || (uint32_t)idx + LOG_CAP < total) break;
        LogLine l;
        pthread_mutex_lock(&g_state_mutex);
        l = g_log[(uint32_t)idx % LOG_CAP];
        pthread_mutex_unlock(&g_state_mutex);
        float age = (float)(g_app.time - l.time);
        float a = open || hov ? 1.0f : clampf(1.0f - (age - 12.0f) / 3.0f, 0.0f, 1.0f);
        if (a <= 0.01f) continue;
        float ly = y + h - 10 - (float)(i + 1) * lh;
        ui_text_shadow(FONT_BODY, 16, x + 12, ly, l.text, ui_alpha(l.color, a), ALIGN_LEFT);
    }
    ui_pop_clip();
    if (scroll > 0) ui_text(FONT_BODY, 13, x + w - 12, y + 6, "scrolled - wheel down for latest", COL_TEXT_FAINT, ALIGN_RIGHT);

    /*input*/
    const UIInput *in = ui_input();
    if (!open) {
        if (in->keys[UIK_ENTER] && !ui_keyboard_captured() && !(g_app.windows & WIN_PAUSE)) {
            g_app.chat_open = true;
            g_app.chat[0] = '\0';
            ui_focus("chat");
            g_app.history_pos = g_app.nhistory;
        }
        if (!ui_keyboard_captured() && glfwGetKey(g_app.win, GLFW_KEY_SLASH) == GLFW_PRESS && !g_app.chat_open) {
            g_app.chat_open = true;
            g_app.chat[0] = '\0';
            ui_focus("chat");
        }
        return;
    }
    float iy = y + h + 8;
    bool submit = ui_text_field("chat", x, iy, w, 40, g_app.chat, sizeof(g_app.chat), TF_FOCUS,
                                "Type a command (look, say hello, cast ...) - Enter sends, Esc cancels");
    /*strip a leading slash typed to open*/
    if (g_app.chat[0] == '/') memmove(g_app.chat, g_app.chat + 1, strlen(g_app.chat));
    /*history*/
    if (in->keys[UIK_UP] && g_app.nhistory > 0) {
        if (g_app.history_pos > 0) g_app.history_pos--;
        snprintf(g_app.chat, sizeof(g_app.chat), "%s", g_app.history[g_app.history_pos % 32]);
    }
    if (in->keys[UIK_DOWN] && g_app.history_pos < g_app.nhistory) {
        g_app.history_pos++;
        if (g_app.history_pos < g_app.nhistory) snprintf(g_app.chat, sizeof(g_app.chat), "%s", g_app.history[g_app.history_pos % 32]);
        else g_app.chat[0] = '\0';
    }
    /*suggestions*/
    size_t cl = strlen(g_app.chat);
    if (cl > 0 && !strchr(g_app.chat, ' ')) {
        int shown = 0;
        float sy = iy - 8;
        const char *first = NULL;
        for (size_t i = 0; i < sizeof(CMDS) / sizeof(CMDS[0]) && shown < 6; i++) {
            if (strncasecmp(CMDS[i].cmd, g_app.chat, cl) != 0) continue;
            if (!first) first = CMDS[i].cmd;
            sy -= 26;
            ui_layer(1);
            ui_rrect(x + 4, sy, w - 8, 24, 4, UIC(30, 24, 34, 240));
            char sb[160];
            snprintf(sb, sizeof(sb), "^8%s^9 - %s", CMDS[i].cmd, CMDS[i].desc);
            ui_text_rich(FONT_BODY, 15, x + 14, sy + 4, sb, COL_TEXT, ALIGN_LEFT);
            ui_layer(0);
            shown++;
        }
        if (first && in->keys[UIK_TAB]) snprintf(g_app.chat, sizeof(g_app.chat), "%s", first);
    }
    if (submit) {
        if (g_app.chat[0]) {
            snprintf(g_app.history[g_app.nhistory % 32], 256, "%s", g_app.chat);
            g_app.nhistory++;
            char echo[300];
            snprintf(echo, sizeof(echo), "> %s", g_app.chat);
            client_log_add_color(echo, UIC(150, 170, 200, 255));
            if (strcmp(g_app.chat, "quit") == 0 || strcmp(g_app.chat, "exit") == 0) g_app.windows |= WIN_PAUSE;
            else app_send("%s", g_app.chat);
        }
        g_app.chat_open = false;
        g_app.chat[0] = '\0';
        ui_release_focus();
    }
    if (in->keys[UIK_ESCAPE]) {
        g_app.chat_open = false;
        ui_release_focus();
    }
}

/*--- banners & overlays ---*/
static void draw_banners(void) {
    float W = ui_width(), H = ui_height();
    float t = (float)(g_app.time - g_app.banner_time);
    if (g_app.banner[0] && t < 5.0f) {
        float a = clampf(t * 2.0f, 0, 1) * clampf((5.0f - t) * 1.2f, 0, 1);
        float y = H * 0.24f;
        ui_rect_hgrad(W * 0.5f - 420, y - 10, 420, 104, UIC(8, 6, 10, 0), UIC(8, 6, 10, (int)(180 * a)));
        ui_rect_hgrad(W * 0.5f, y - 10, 420, 104, UIC(8, 6, 10, (int)(180 * a)), UIC(8, 6, 10, 0));
        ui_text_shadow(FONT_TITLE, 46, W * 0.5f, y, g_app.banner, ui_alpha(COL_GOLD, a), ALIGN_CENTER);
        ui_rect_hgrad(W * 0.5f - 300, y + 58, 300, 1.2f, UIC(222, 184, 96, 0), ui_alpha(COL_GOLD, a));
        ui_rect_hgrad(W * 0.5f, y + 58, 300, 1.2f, ui_alpha(COL_GOLD, a), UIC(222, 184, 96, 0));
        ui_text_shadow(FONT_BODY, 19, W * 0.5f, y + 66, g_app.banner_sub, ui_alpha(COL_TEXT, a), ALIGN_CENTER);
    }
    float lt = (float)(g_app.time - g_app.levelup_time);
    if (g_app.levelup_level > 0 && lt < 4.0f) {
        float a = clampf(lt * 3.0f, 0, 1) * clampf((4.0f - lt) * 1.5f, 0, 1);
        float sc = 1.0f + 0.15f * (1.0f - clampf(lt * 3.0f, 0, 1));
        char buf[64];
        snprintf(buf, sizeof(buf), "LEVEL %d", g_app.levelup_level);
        ui_text_outline(FONT_TITLE, 64 * sc, W * 0.5f, H * 0.36f, buf, ui_alpha(UIC(255, 225, 120, 255), a), UIC(60, 30, 0, (int)(200 * a)), ALIGN_CENTER);
        ui_text_shadow(FONT_BODY, 20, W * 0.5f, H * 0.36f + 76, "Health restored - 3 stat points to spend (C)", ui_alpha(COL_TEXT, a), ALIGN_CENTER);
    }
}

static void draw_death(void) {
    if (!g_app.dead) return;
    float W = ui_width(), H = ui_height();
    float t = (float)(g_app.time - g_app.dead_time);
    float a = clampf(t * 0.8f, 0, 1);
    ui_rect(0, 0, W, H, UIC(20, 0, 0, (int)(170 * a)));
    ui_block(0, 0, W, H);
    ui_text_outline(FONT_TITLE, 84, W * 0.5f, H * 0.3f, "YOU HAVE FALLEN", ui_alpha(UIC(220, 50, 40, 255), a), UIC(0, 0, 0, (int)(220 * a)), ALIGN_CENTER);
    ui_text_wrap(FONT_BODY, 20, W * 0.5f - 360, H * 0.3f + 120, 720,
                 "The Arcane carries your spirit back to the village. Your equipment stays where you fell, "
                 "marked by a tombstone: return to it to recover your belongings.", ui_alpha(COL_TEXT, a), 26);
    if (t > 1.5f && ui_button("Rise again", W * 0.5f - 110, H * 0.3f + 230, 220, 50, BTN_PRIMARY)) {
        g_app.dead = false;
        audio_sfx(SFX_HEAL, 0.8f, 0);
    }
}

static void draw_pause(void) {
    float W = ui_width(), H = ui_height();
    ui_rect(0, 0, W, H, UIC(0, 0, 0, 120));
    ui_block(0, 0, W, H);
    float w = 400, h = 470, x = (W - w) * 0.5f, y = (H - h) * 0.5f;
    if (!ui_window("Menu", x, y, w, h, true)) { g_app.windows &= ~(uint32_t)WIN_PAUSE; return; }
    float by = y + 64, bx = x + 50, bw = w - 100;
    if (ui_button("Resume", bx, by, bw, 46, BTN_PRIMARY)) g_app.windows &= ~(uint32_t)WIN_PAUSE;
    by += 58;
    if (ui_button("Settings", bx, by, bw, 46, BTN_NORMAL)) { g_app.windows &= ~(uint32_t)WIN_PAUSE; g_app.windows |= WIN_SETTINGS; }
    by += 58;
    if (ui_button("Guide", bx, by, bw, 46, BTN_NORMAL)) { g_app.windows &= ~(uint32_t)WIN_PAUSE; g_app.windows |= WIN_HELP; }
    by += 58;
    if (ui_button("Save game", bx, by, bw, 46, BTN_NORMAL)) { app_send("save"); ui_toast("Game saved.", COL_GOLD); }
    by += 58;
    if (ui_button("Leave to title", bx, by, bw, 46, BTN_NORMAL)) {
        app_send("save");
        app_disconnect(NULL);
        return;
    }
    by += 58;
    if (ui_button("Quit game", bx, by, bw, 46, BTN_DANGER)) {
        app_send("save");
        g_app.request_quit = true;
    }
}

/* ------------------------------------------------------------------ */

void hud_draw(void) {
    float W = ui_width(), H = ui_height();
    draw_plates();
    draw_float_texts();
    draw_portrait();
    draw_location();
    draw_minimap();
    draw_objective();
    draw_target_frame();
    draw_hotbar();
    if (!g_app.dead) draw_context_prompt();
    draw_chat();
    draw_banners();

    /*windows: inventory on the right, others on the left*/
    float left_x = 40, top = 150;
    if (g_app.windows & WIN_SHOP) draw_shop(left_x, top);
    else if (g_app.windows & WIN_CHARACTER) draw_character(left_x, top);
    else if (g_app.windows & WIN_SPELLS) draw_spells(left_x, top);
    if ((g_app.windows & WIN_SHOP) && (g_app.windows & WIN_CHARACTER)) draw_character(left_x + 660, top);
    if (g_app.windows & WIN_INVENTORY) draw_inventory(W - 600 - 300, top);
    if (g_app.windows & WIN_MAP) draw_map_window();
    if (g_app.windows & WIN_HELP) {
        float w = 1000, h = 740, x = (W - w) * 0.5f, y = (H - h) * 0.5f;
        if (!ui_window("Guide", x, y, w, h, true)) g_app.windows &= ~(uint32_t)WIN_HELP;
        else {
            const char **names;
            int n = hud_help_tabs(&names);
            float tx = x + 24;
            for (int i = 0; i < n; i++) {
                float bw = ui_text_width(FONT_BOLD, 16, names[i]) + 30;
                if (ui_button(names[i], tx, y + 54, bw, 34, g_app.help_tab == i ? BTN_TAB_ACTIVE : BTN_TAB)) g_app.help_tab = i;
                tx += bw + 6;
            }
            hud_help_content(x + 24, y + 104, w - 48, h - 128, g_app.help_tab);
        }
    }
    if (g_app.windows & WIN_SETTINGS) {
        float w = 900, h = 700, x = (W - w) * 0.5f, y = (H - h) * 0.5f;
        if (!ui_window("Settings", x, y, w, h, true)) { g_app.windows &= ~(uint32_t)WIN_SETTINGS; settings_save(); }
        else hud_settings_content(x + 24, y + 56, w - 48, h - 80);
    }
    ctx_menu_draw();
    draw_death();
    if (g_app.windows & WIN_PAUSE) draw_pause();

    /*Escape: close the topmost thing, else open the menu*/
    if (ui_input()->keys[UIK_ESCAPE] && !g_app.chat_open && !g_ctx.open) {
        if (g_app.windows & (WIN_PAUSE | WIN_SETTINGS | WIN_HELP | WIN_MAP)) {
            if (g_app.windows & WIN_SETTINGS) settings_save();
            g_app.windows &= ~(uint32_t)(WIN_PAUSE | WIN_SETTINGS | WIN_HELP | WIN_MAP);
        } else if (g_app.windows & (WIN_INVENTORY | WIN_CHARACTER | WIN_SPELLS | WIN_SHOP)) {
            g_app.windows &= ~(uint32_t)(WIN_INVENTORY | WIN_CHARACTER | WIN_SPELLS | WIN_SHOP);
        } else if (g_app.target_id || g_app.path_len) {
            g_app.target_id = 0;
            g_app.path_len = 0;
            g_app.path_target_id = 0;
        } else {
            g_app.windows |= WIN_PAUSE;
        }
    }
}
