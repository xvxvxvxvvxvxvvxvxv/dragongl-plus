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

/*Out-of-game screens: title, solo/multiplayer login, character
 * creation, connection progress, settings, how-to-play and credits.
 * Behind every menu the client renders a procedurally generated dungeon
 * (the same generator the server uses), slowly orbited by the camera.*/

#include "app.h"
#include "audio.h"
#include "settings.h"
#include "platform.h"
#include "species.h"
#include "classes.h"
#include "alignment.h"
#include "net.h"
#include "rules.h"
#include <GLFW/glfw3.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DRAGONGL_VERSION_STR "2026.10"

extern void hud_help_content(float x, float y, float w, float h, int tab);
extern int hud_help_tabs(const char ***names);
extern void hud_settings_content(float x, float y, float w, float h);

static Map g_demo_map;
static int g_prev_x, g_prev_y;

int screens_preview_spot(int *x, int *y) {
    *x = g_prev_x;
    *y = g_prev_y;
    return 1;
}

void screens_title_snapshot_init(void) {
    /*Demo floor for the menu background: a crystal/mushroom-rich floor
     * of the real generator, without the printouts.*/
    srand(7);
    generate_procedural_dungeon(&g_demo_map, 23);
    FrameSnapshot *s = g_app.title_snap;
    memcpy(s->map, g_demo_map.data[0], sizeof(s->map));
    s->map_version = 1;
    /*Centre the camera on an open area full of features*/
    int bx = MAP_CENTER_X, by = MAP_CENTER_Y, best = -1;
    for (int y = 40; y < MAP_HEIGHT - 40; y += 6)
        for (int x = 40; x < MAP_WIDTH - 40; x += 6) {
            /*an open hall (mostly floor) dotted with glowing features*/
            int score = 0, solid = 0, near_solid = 0;
            for (int dy = -8; dy <= 8; dy++)
                for (int dx = -8; dx <= 8; dx++) {
                    TileType t = s->map[y + dy][x + dx];
                    bool sol = t == VOXEL_WALL || t == VOXEL_ROCK || t == VOXEL_OBSIDIAN || t == VOXEL_GOLD_VEIN;
                    if (sol) {
                        solid++;
                        if (dx * dx + dy * dy <= 9) near_solid++;
                    } else score += 1;
                    if (t >= VOXEL_CRYSTAL_BLUE || t == VOXEL_MUSHROOM_GLOW) score += 3;
                    else if (t == VOXEL_LAVA) score -= 2;
                    else if (t == VOXEL_WATER) score += 1;
                }
            if (solid > 17 * 17 * 45 / 100 || near_solid > 0) continue;
            if (score > best) {
                best = score; bx = x; by = y;
            }
        }
    s->my_x = bx;
    s->my_y = by;
    /*Character-creation preview: the open tile (no wall within 6 tiles,
     * so the close-up camera never ends up inside rock) nearest to the
     * menu camera's focus.*/
    g_prev_x = bx;
    g_prev_y = by;
    int bestd = 1 << 30;
    for (int y = 20; y < MAP_HEIGHT - 20; y++)
        for (int x = 20; x < MAP_WIDTH - 20; x++) {
            int d = (x - bx) * (x - bx) + (y - by) * (y - by);
            if (d >= bestd) continue;
            bool open = true;
            for (int dy = -6; dy <= 6 && open; dy++)
                for (int dx = -6; dx <= 6 && open; dx++) {
                    TileType t = s->map[y + dy][x + dx];
                    if (t == VOXEL_WALL || t == VOXEL_ROCK || t == VOXEL_OBSIDIAN || t == VOXEL_GOLD_VEIN ||
                        t == VOXEL_WATER || t == VOXEL_LAVA || t >= VOXEL_CRYSTAL_BLUE)
                        open = false;
                }
            if (open) { bestd = d; g_prev_x = x; g_prev_y = y; }
        }
    s->my_entity_id = -1;
    /*Developer aid: DRAGONGL_GALLERY=<page> lines up 5 playable races
     * per page (page 0 = races 0-4, ...) in front of the menu camera,
     * to check the character models.*/
    const char *gal = getenv("DRAGONGL_GALLERY");
    if (gal) {
        int page = atoi(gal);
        s->my_x = g_prev_x;
        s->my_y = g_prev_y;
        /*DRAGONGL_GALLERY=d<n> : frame the n-th door of the demo floor*/
        if (gal[0] == 'd') {
            int want = atoi(gal + 1), n = 0;
            for (int y = 1; y < MAP_HEIGHT - 1 && n <= want; y++)
                for (int x = 1; x < MAP_WIDTH - 1 && n <= want; x++)
                    if (s->map[y][x] == VOXEL_DOOR && n++ == want) { s->my_x = x; s->my_y = y; }
            scene_set_gallery(true);
            s->my_floor = 23;
            return;
        }
        /*DRAGONGL_GALLERY=r<race> : that race alone, close up*/
        bool single = gal[0] == 'r';
        if (single) page = atoi(gal + 1);
        for (int k = 0; k < (single ? 1 : 5); k++) {
            int race = single ? page : page * 5 + k;
            if (race >= RACE_COUNT) break;
            Entity *e = &s->entities[k];
            memset(e, 0, sizeof(*e));
            e->active = true;
            e->id = 1000 + race;
            e->x = single ? g_prev_x : g_prev_x - 4 + k * 2;
            e->y = g_prev_y;
            e->hp = e->max_hp = 10;
            e->is_player = true;
            e->kind = KIND_PLAYER;
            e->race_id = race;
            e->class_id = (race * 5) % 12;
            e->look_seed = (uint32_t)race * 2654435761u;
            snprintf(e->username, sizeof(e->username), "%s", RACES[race].name);
        }
        scene_set_gallery(true);
    }
    s->my_floor = 23;
    s->vision_radius = 40;
    s->movement_cooldown = 0.2f;
    s->game_h = 22;
}

/* ------------------------------------------------------------------ */

static void title_logo(float cx, float y, float a) {
    float pulse = 0.85f + 0.15f * sinf((float)g_app.time * 1.4f);
    uint32_t glow = UIC(255, 140, 40, (int)(70 * a * pulse));
    for (int i = 4; i >= 1; i--)
        ui_text(FONT_TITLE, 96, cx + (float)i * 0.7f, y + (float)i * 0.7f, "DRAGON GL", glow, ALIGN_CENTER);
    ui_text_shadow(FONT_TITLE, 96, cx, y, "DRAGON GL", ui_alpha(UIC(240, 200, 120, 255), a), ALIGN_CENTER);
    float lw = 520;
    ui_rect_hgrad(cx - lw, y + 118, lw, 1.5f, UIC(222, 184, 96, 0), ui_alpha(COL_GOLD, a));
    ui_rect_hgrad(cx, y + 118, lw, 1.5f, ui_alpha(COL_GOLD, a), UIC(222, 184, 96, 0));
    ui_diamond(cx, y + 119, 6, ui_alpha(COL_GOLD, a));
    ui_text_shadow(FONT_TITLE, 24, cx, y + 132, "THE LONG DESCENT INTO THE DRAGON'S MAW", ui_alpha(COL_TEXT_DIM, a), ALIGN_CENTER);
}

static bool menu_button(const char *label, const char *sub, float x, float y, float w, int icon) {
    float h = 64;
    bool hov = ui_hover(x, y, w, h);
    ui_block(x, y, w, h);
    static double last_hover_sound;
    static uint32_t last_hover_id;
    uint32_t hid = (uint32_t)(size_t)label;
    if (hov && last_hover_id != hid && g_app.time - last_hover_sound > 0.05) {
        audio_sfx(SFX_HOVER, 0.25f, 0);
        last_hover_sound = g_app.time;
        last_hover_id = hid;
    }
    if (!hov && last_hover_id == hid) last_hover_id = 0;
    float slide = hov ? 14.0f : 0.0f;
    ui_rect_hgrad(x, y, w, h, hov ? UIC(120, 80, 30, 200) : UIC(20, 16, 24, 170), UIC(20, 16, 24, 0));
    ui_rect(x, y, 3, h, hov ? COL_GOLD : UIC(140, 110, 60, 200));
    if (icon) ui_icon(icon, x + 18 + slide, y + 14, 36, hov ? COL_WHITE : UIC(220, 210, 190, 230));
    ui_text_shadow(FONT_TITLE, 28, x + 68 + slide, y + 8, label, hov ? UIC(255, 225, 150, 255) : COL_TEXT, ALIGN_LEFT);
    if (sub) ui_text(FONT_BODY, 15, x + 70 + slide, y + 40, sub, COL_TEXT_FAINT, ALIGN_LEFT);
    if (hov && ui_input()->pressed[0]) {
        audio_sfx(SFX_CLICK, 0.6f, 0);
        return true;
    }
    return false;
}

static void error_box(float x, float y, float w) {
    if (!g_app.err[0]) return;
    float h = ui_text_wrap_height(FONT_BODY, 16, w - 30, g_app.err, 20) + 24;
    ui_rrect(x, y, w, h, 6, UIC(70, 16, 16, 220));
    ui_rrect_line(x, y, w, h, 6, 1.2f, UIC(220, 90, 70, 255));
    ui_text_wrap(FONT_BODY, 16, x + 15, y + 12, w - 30, g_app.err, UIC(255, 210, 200, 255), 20);
}

static void draw_title(void) {
    float W = ui_width(), H = ui_height();
    if (getenv("DRAGONGL_GALLERY")) return; /*models only*/
    float a = clampf(g_app.screen_t * 1.5f, 0, 1);
    ui_rect_hgrad(0, 0, W * 0.55f, H, UIC(6, 4, 10, 200), UIC(6, 4, 10, 0));
    ui_rect_grad(0, 0, W, H * 0.34f, UIC(6, 4, 10, 215), UIC(6, 4, 10, 0));
    ui_shadow(W * 0.5f - 420, H * 0.10f + 10, 840, 150, 60, 70, 0.85f);
    title_logo(W * 0.5f, H * 0.10f, a);
    float x = 120, y = H * 0.40f, w = 520;
    if (menu_button("Play Solo", "Your own private world - start right now", x, y, w, ICON_SWORD)) {
        app_set_screen(SCR_SOLO);
    }
    y += 76;
    if (menu_button("Join a Server", "Adventure with friends over the network", x, y, w, ICON_SPEECH)) {
        app_set_screen(SCR_JOIN);
    }
    y += 76;
    if (menu_button("How to Play", "Controls, goals and tips for new heroes", x, y, w, ICON_BOOK)) {
        app_set_screen(SCR_HOWTO);
    }
    y += 76;
    if (menu_button("Settings", "Graphics, audio, controls and interface", x, y, w, ICON_GEAR)) {
        app_set_screen(SCR_SETTINGS);
    }
    y += 76;
    if (menu_button("Credits", NULL, x, y, w, ICON_STAR)) app_set_screen(SCR_CREDITS);
    y += 76;
    if (menu_button("Quit", NULL, x, y, w, ICON_SKULL)) g_app.request_quit = true;
    error_box(x, y + 90, w);

    ui_rect_grad(0, H - 70, W, 70, UIC(6, 4, 10, 0), UIC(6, 4, 10, 210));
    char foot[160];
    snprintf(foot, sizeof(foot), "DragonGL+ %s  |  %s  |  F12: screenshot", DRAGONGL_VERSION_STR, g_app.gfx->device_name());
    ui_text(FONT_BODY, 14, W - 20, H - 28, foot, COL_TEXT_FAINT, ALIGN_RIGHT);
    ui_text(FONT_BODY, 14, 20, H - 28, "GPL-3.0  -  (C) 2026 Nicola Taibi", COL_TEXT_FAINT, ALIGN_LEFT);
}

/* ------------------------------------------------------------------ */
/* Solo                                                                */
/* ------------------------------------------------------------------ */

static void scan_solo_heroes(void) {
    g_app.solo_nheroes = 0;
    char dir[1100];
    snprintf(dir, sizeof(dir), "%s/saves", local_server_dir());
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && g_app.solo_nheroes < 16) {
        size_t n = strlen(e->d_name);
        if (n > 5 && n < 37 && strcmp(e->d_name + n - 5, ".save") == 0) {
            memcpy(g_app.solo_heroes[g_app.solo_nheroes], e->d_name, n - 5);
            g_app.solo_heroes[g_app.solo_nheroes][n - 5] = '\0';
            g_app.solo_nheroes++;
        }
    }
    closedir(d);
}

#define SOLO_PASSWORD "dragongl-solo"

static void solo_login(const char *name, bool is_new) {
    LoginInfo li;
    memset(&li, 0, sizeof(li));
    copy_str(li.user, name, sizeof(li.user));
    copy_str(li.pass, SOLO_PASSWORD, sizeof(li.pass));
    li.is_new = is_new;
    if (!is_new) app_start_session(&li, true);
    else {
        g_app.login = li;
        g_app.create_from_solo = true;
        app_set_screen(SCR_CREATE);
    }
}

static void draw_solo(void) {
    float W = ui_width(), H = ui_height();
    static double scanned;
    if (g_app.screen_frames <= 1 || g_app.time - scanned > 3.0) {
        scan_solo_heroes();
        scanned = g_app.time;
    }
    float w = 640, h = 560, x = (W - w) * 0.5f, y = (H - h) * 0.5f;
    if (!ui_window("Play Solo", x, y, w, h, true) || ui_input()->keys[UIK_ESCAPE]) {
        app_set_screen(SCR_TITLE);
        return;
    }
    float cy = y + 60;
    ui_text_wrap(FONT_BODY, 17, x + 30, cy, w - 60,
                 "A private server runs on this computer: your world and your heroes are saved in your user profile. "
                 "No password, no network setup.", COL_TEXT_DIM, 22);
    cy += 70;
    ui_header("Your heroes", x + 30, cy, w - 60);
    cy += 34;
    if (g_app.solo_nheroes == 0) {
        ui_text(FONT_BODY, 17, x + 30, cy, "No hero yet - create your first one below.", COL_TEXT_FAINT, ALIGN_LEFT);
        cy += 40;
    } else {
        float lh = 46;
        float listh = fminf(4.5f * lh, (float)g_app.solo_nheroes * lh);
        /*the scroll area (clip + bar) reaches into the right margin, so
         * the buttons keep the same margin on both sides*/
        float sw = w - 60 + 14;
        float off = ui_scroll_begin("solo_list", x + 30, cy, sw, listh, (float)g_app.solo_nheroes * lh);
        for (int i = 0; i < g_app.solo_nheroes; i++) {
            float ry = cy + off + (float)i * lh;
            char label[80];
            snprintf(label, sizeof(label), "Continue as %s", g_app.solo_heroes[i]);
            /*full width: same margin left and right (the thin scroll bar,
             * when there is one, overlays the right edge)*/
            float bw = w - 60;
            if (ui_button(label, x + 30, ry, bw, lh - 8, i == 0 ? BTN_PRIMARY : BTN_NORMAL)) {
                audio_sfx(SFX_CLICK, 0.6f, 0);
                solo_login(g_app.solo_heroes[i], false);
            }
        }
        ui_scroll_end();
        cy += listh + 16;
    }
    ui_header("New hero", x + 30, cy, w - 60);
    cy += 36;
    static char newname[17];
    if (g_app.screen_frames <= 2) ui_focus("solo_name"); /*type right away*/
    ui_text(FONT_BODY, 16, x + 30, cy + 20, "Name", COL_TEXT_DIM, ALIGN_MIDDLE);
    bool enter = ui_text_field("solo_name", x + 110, cy, w - 140, 40, newname, sizeof(newname), 0, "letters, digits, _ (max 16)");
    cy += 56;
    bool valid = newname[0] != '\0';
    for (const char *p = newname; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_')) valid = false;
    bool exists = false;
    for (int i = 0; i < g_app.solo_nheroes; i++)
        if (strcmp(g_app.solo_heroes[i], newname) == 0) exists = true;
    if (newname[0] && !valid) ui_text(FONT_BODY, 15, x + 110, cy - 10, "Only letters, digits and _ are allowed.", COL_RED, ALIGN_LEFT);
    else if (exists) ui_text(FONT_BODY, 15, x + 110, cy - 10, "A hero with this name already exists.", COL_ORANGE, ALIGN_LEFT);
    if ((ui_button("Create Hero", x + w - 250, y + h - 70, 220, 46, BTN_PRIMARY) || enter) && valid && !exists) {
        audio_sfx(SFX_CLICK, 0.6f, 0);
        solo_login(newname, true);
    }
    if (ui_button("Back", x + 30, y + h - 70, 140, 46, BTN_NORMAL)) app_set_screen(SCR_TITLE);
}

/* ------------------------------------------------------------------ */
/* Join                                                                */
/* ------------------------------------------------------------------ */

static void draw_join(void) {
    float W = ui_width(), H = ui_height();
    static char host[128], port[8], spass[64], user[17], pass[32];
    static bool init;
    if (!init) {
        snprintf(host, sizeof(host), "%s", g_settings.last_host);
        snprintf(port, sizeof(port), "%d", g_settings.last_port > 0 ? g_settings.last_port : 8080);
        snprintf(user, sizeof(user), "%s", g_settings.last_user);
        init = true;
    }
    float w = 640, h = 560, x = (W - w) * 0.5f, y = (H - h) * 0.5f;
    if (!ui_window("Join a Server", x, y, w, h, true) || ui_input()->keys[UIK_ESCAPE]) {
        app_set_screen(SCR_TITLE);
        return;
    }
    static const char *ids[] = {"j_host", "j_port", "j_spass", "j_user", "j_pass"};
    float cy = y + 64, lx = x + 30, fx = x + 230, fw = w - 260;
    struct { const char *label, *ph; char *buf; int cap, flags; } F[] = {
        {"Server address", "IP or host name", host, sizeof(host), 0},
        {"Port", "8080", port, sizeof(port), TF_DIGITS},
        {"Server password", "asked by the server owner", spass, sizeof(spass), TF_PASSWORD},
        {"Hero name", "your character", user, sizeof(user), 0},
        {"Hero password", "protects your character", pass, sizeof(pass), TF_PASSWORD},
    };
    bool submit = false;
    if (g_app.screen_frames <= 2) ui_focus(user[0] ? "j_pass" : "j_host");
    for (int i = 0; i < 5; i++) {
        ui_text(FONT_BODY, 17, lx, cy + 20, F[i].label, COL_TEXT_DIM, ALIGN_MIDDLE);
        if (ui_text_field(ids[i], fx, cy, fw, 40, F[i].buf, F[i].cap, F[i].flags, F[i].ph)) submit = true;
        if (ui_text_field_focused(ids[i]) && ui_input()->keys[UIK_TAB]) {
            ui_focus(ids[(i + (ui_input()->shift ? 4 : 1)) % 5]);
            ui_input();
        }
        cy += 58;
    }
    ui_text_wrap(FONT_BODY, 14, lx, cy, w - 60,
                 "^9Hosting: run ^8dragongl_server --password <pwd>^9 on the host computer and share its address. "
                 "The default port is 8080. Passwords travel unencrypted: do not reuse important ones.", COL_TEXT_FAINT, 18);
    cy += 60;
    error_box(lx, cy, w - 60);
    LoginInfo li;
    memset(&li, 0, sizeof(li));
    copy_str(li.host, host, sizeof(li.host));
    li.port = atoi(port) > 0 ? atoi(port) : 8080;
    copy_str(li.server_pass, spass, sizeof(li.server_pass));
    copy_str(li.user, user, sizeof(li.user));
    copy_str(li.pass, pass, sizeof(li.pass));
    bool ok = host[0] && user[0];
    if (ui_button("Back", lx, y + h - 70, 120, 46, BTN_NORMAL)) app_set_screen(SCR_TITLE);
    if (ui_button("New Hero...", x + w - 420, y + h - 70, 180, 46, BTN_NORMAL) && ok) {
        g_app.login = li;
        g_app.create_from_solo = false;
        app_set_screen(SCR_CREATE);
    }
    if ((ui_button("Enter the Maw", x + w - 230, y + h - 70, 200, 46, BTN_PRIMARY) || submit) && ok) {
        audio_sfx(SFX_CLICK, 0.6f, 0);
        app_start_session(&li, false);
    }
    if (!ok) ui_text(FONT_BODY, 14, x + w - 30, y + h - 92, "Server address and hero name are required.", COL_TEXT_FAINT, ALIGN_RIGHT);
}

/* ------------------------------------------------------------------ */
/* Character creation                                                  */
/* ------------------------------------------------------------------ */

static int c_race, c_sub = -1, c_class = CLASS_FIGHTER, c_align = ALIGN_TRUE_NEUTRAL;
static char c_sex = 'M';

int screens_preview(int *race, int *cls) {
    if (g_app.screen != SCR_CREATE) return 0;
    *race = c_race;
    *cls = c_class;
    return 1;
}

static void bonus_line(char *out, size_t cap, int s, int d, int c, int i, int w, int ch) {
    out[0] = '\0';
    const char *n[6] = {"STR", "DEX", "CON", "INT", "WIS", "CHA"};
    int v[6] = {s, d, c, i, w, ch};
    for (int k = 0; k < 6; k++)
        if (v[k]) {
            char t[16];
            snprintf(t, sizeof(t), "%s%s %+d", out[0] ? ", " : "", n[k], v[k]);
            strncat(out, t, cap - strlen(out) - 1);
        }
    if (!out[0]) snprintf(out, cap, "flexible");
}

static void draw_create(void) {
    float W = ui_width(), H = ui_height();
    ui_text_shadow(FONT_TITLE, 40, W * 0.5f, 26, "Forge Your Hero", COL_GOLD, ALIGN_CENTER);
    /*--- races ---*/
    float lx = 40, ly = 90, lw = 420, lh = H - 200;
    ui_panel(lx, ly, lw, lh);
    ui_text(FONT_TITLE, 20, lx + 20, ly + 14, "Ancestry", COL_GOLD, ALIGN_LEFT);
    float rowh = 34;
    float off = ui_scroll_begin("races", lx + 14, ly + 50, lw - 28, lh - 64, (float)RACE_COUNT * rowh);
    for (int i = 0; i < RACE_COUNT; i++) {
        float ry = ly + 50 + off + (float)i * rowh;
        bool sel = c_race == i;
        bool hov = ui_hover(lx + 14, ry, lw - 40, rowh - 4);
        if (sel || hov) ui_rrect(lx + 14, ry, lw - 40, rowh - 4, 4, sel ? UIC(110, 80, 36, 230) : UIC(60, 50, 40, 140));
        ui_text(FONT_BOLD, 17, lx + 26, ry + (rowh - 4) * 0.5f, RACES[i].name, sel ? COL_GOLD : COL_TEXT, ALIGN_MIDDLE);
        if (hov && ui_input()->pressed[0]) { c_race = i; c_sub = -1; audio_sfx(SFX_CLICK, 0.4f, 0); }
    }
    ui_scroll_end();

    /*--- classes ---*/
    float rx = W - 500, rw = 460;
    ui_panel(rx, 90, rw, H - 200);
    ui_text(FONT_TITLE, 20, rx + 20, 104, "Profession", COL_GOLD, ALIGN_LEFT);
    static const int cls_icon[CLASS_COUNT] = {ICON_AXE, ICON_STAR, ICON_SUN, ICON_GEM, ICON_SWORD, ICON_HAND,
                                              ICON_SHIELD, ICON_BOW, ICON_DAGGER, ICON_MANA, ICON_EYE, ICON_STAFF};
    float gx = rx + 20, gy = 144, cw = (rw - 50) / 3.0f, ch = 74;
    for (int i = 0; i < CLASS_COUNT; i++) {
        float bx = gx + (float)(i % 3) * (cw + 5), by = gy + (float)(i / 3) * (ch + 6);
        bool sel = c_class == i, hov = ui_hover(bx, by, cw, ch);
        ui_rrect(bx, by, cw, ch, 6, sel ? UIC(110, 80, 36, 240) : (hov ? UIC(60, 50, 44, 220) : UIC(30, 26, 34, 220)));
        ui_rrect_line(bx, by, cw, ch, 6, 1.2f, sel ? COL_GOLD : UIC(90, 76, 58, 255));
        ui_icon(cls_icon[i], bx + cw * 0.5f - 18, by + 8, 36, COL_WHITE);
        ui_text(FONT_BOLD, 15, bx + cw * 0.5f, by + 50, CLASSES[i].name, sel ? COL_GOLD : COL_TEXT, ALIGN_CENTER);
        if (hov && ui_input()->pressed[0]) { c_class = i; audio_sfx(SFX_CLICK, 0.4f, 0); }
    }
    float dy = gy + 4 * (ch + 6) + 10;
    char buf[256];
    snprintf(buf, sizeof(buf), "^8%s^0 - hit die d%d, key ability: %s", CLASSES[c_class].name, CLASSES[c_class].hit_die,
             CLASSES[c_class].primary_ability);
    ui_text_rich(FONT_BODY, 16, rx + 20, dy, buf, COL_TEXT, ALIGN_LEFT);
    dy += 26;
    dy += ui_text_wrap(FONT_BODY, 16, rx + 20, dy, rw - 40, CLASSES[c_class].description, COL_TEXT_DIM, 21) + 12;
    static const char *tips[CLASS_COUNT] = {
        "Tough melee fighter. Great first choice.", "Support caster with charm and songs.",
        "Healer and holy caster - sturdy and versatile.", "Nature magic and shapes of the wild.",
        "Master of arms. The easiest class to learn.", "Fast unarmed striker with ki techniques.",
        "Holy knight: heavy armor plus healing magic.", "Archer and tracker with some nature spells.",
        "Stealthy striker: daggers, traps and tricks.", "Raw innate magic, fragile but explosive.",
        "Pact magic from dark patrons.", "Scholar of the arcane: the widest spellbook.",
    };
    ui_text_rich(FONT_BODY, 15, rx + 20, dy, "^3Tip:^0", COL_TEXT, ALIGN_LEFT);
    ui_text_wrap(FONT_BODY, 15, rx + 60, dy, rw - 80, tips[c_class], COL_TEXT_DIM, 19);

    /*--- centre: race details, subrace, alignment, sex, name ---*/
    float cx = lx + lw + 30, cw2 = rx - cx - 30;
    float by = H - 330;
    ui_panel(cx, by, cw2, 220);
    float ty = by + 16;
    ui_text(FONT_TITLE, 22, cx + 20, ty, RACES[c_race].name, COL_GOLD, ALIGN_LEFT);
    char bon[96];
    bonus_line(bon, sizeof(bon), RACES[c_race].str_bonus, RACES[c_race].dex_bonus, RACES[c_race].con_bonus,
               RACES[c_race].int_bonus, RACES[c_race].wis_bonus, RACES[c_race].cha_bonus);
    ui_text(FONT_BODY, 15, cx + cw2 - 20, ty + 6, bon, UIC(160, 220, 140, 255), ALIGN_RIGHT);
    ty += 34;
    ty += ui_text_wrap(FONT_BODY, 15, cx + 20, ty, cw2 - 40, RACES[c_race].description, COL_TEXT_DIM, 19) + 4;
    snprintf(buf, sizeof(buf), "^9Traits: ^0%s", RACES[c_race].traits);
    ty += ui_text_wrap(FONT_BODY, 14, cx + 20, ty, cw2 - 40, buf, COL_TEXT, 18) + 6;
    /*subraces*/
    float sx = cx + 20;
    bool any_sub = false;
    for (int i = 0; i < SUBRACE_COUNT; i++) {
        if ((int)SUBRACES[i].parent_race != c_race) continue;
        if (!any_sub) {
            ui_text(FONT_BODY, 14, sx, ty + 13, "Lineage:", COL_TEXT_FAINT, ALIGN_MIDDLE);
            sx += 64;
            any_sub = true;
        }
        float bw = ui_text_width(FONT_BOLD, 14, SUBRACES[i].name) + 22;
        if (sx + bw > cx + cw2 - 20) break;
        if (ui_button(SUBRACES[i].name, sx, ty, bw, 26, c_sub == i ? BTN_TAB_ACTIVE : BTN_TAB)) c_sub = (c_sub == i) ? -1 : i;
        if (ui_hover(sx, ty, bw, 26)) ui_tooltip(SUBRACES[i].name, SUBRACES[i].description);
        sx += bw + 6;
    }

    /*--- bottom bar: alignment, sex, name, buttons ---*/
    float bb = H - 96;
    ui_panel(40, bb, W - 80, 76);
    float ax = 60;
    ui_text(FONT_BODY, 15, ax, bb + 14, "Alignment", COL_TEXT_FAINT, ALIGN_LEFT);
    if (ui_button("<", ax, bb + 34, 30, 30, BTN_NORMAL)) c_align = (c_align + ALIGN_COUNT - 1) % ALIGN_COUNT;
    ui_text(FONT_BOLD, 16, ax + 110, bb + 49, ALIGNMENTS[c_align].name, COL_TEXT, ALIGN_CENTER | ALIGN_MIDDLE);
    if (ui_button(">", ax + 190, bb + 34, 30, 30, BTN_NORMAL)) c_align = (c_align + 1) % ALIGN_COUNT;
    ax += 250;
    ui_text(FONT_BODY, 15, ax, bb + 14, "Appearance", COL_TEXT_FAINT, ALIGN_LEFT);
    if (ui_button("Male", ax, bb + 34, 80, 30, c_sex == 'M' ? BTN_TAB_ACTIVE : BTN_TAB)) c_sex = 'M';
    if (ui_button("Female", ax + 86, bb + 34, 90, 30, c_sex == 'F' ? BTN_TAB_ACTIVE : BTN_TAB)) c_sex = 'F';
    ax += 200;
    ui_text(FONT_BODY, 15, ax, bb + 14, "Name", COL_TEXT_FAINT, ALIGN_LEFT);
    ui_text_field("c_name", ax, bb + 32, 260, 34, g_app.login.user, 17, 0, "hero name");
    if (ui_button("Back", W - 400, bb + 18, 120, 42, BTN_NORMAL) || ui_input()->keys[UIK_ESCAPE])
        app_set_screen(g_app.create_from_solo ? SCR_SOLO : SCR_JOIN);
    if (ui_button("Begin the Descent", W - 270, bb + 18, 210, 42, BTN_PRIMARY) && g_app.login.user[0]) {
        audio_sfx(SFX_CLICK, 0.7f, 0);
        LoginInfo li = g_app.login;
        li.is_new = true;
        li.race_id = c_race;
        li.subrace_id = c_sub;
        li.class_id = c_class;
        li.alignment = c_align;
        li.sex = c_sex;
        app_start_session(&li, g_app.create_from_solo);
    }
    ui_text(FONT_BODY, 14, W * 0.5f, bb - 22, "Ability scores are rolled by the server when you enter the world.",
            COL_TEXT_FAINT, ALIGN_CENTER);
}

/* ------------------------------------------------------------------ */
/* Connecting                                                          */
/* ------------------------------------------------------------------ */

static void draw_connecting(void) {
    float W = ui_width(), H = ui_height();
    int st = atomic_load(&g_net_status);
    float w = 620, h = 260, x = (W - w) * 0.5f, y = (H - h) * 0.5f;
    ui_panel(x, y, w, h);
    if (st == NET_FAILED || st == NET_DISCONNECTED) {
        ui_text_shadow(FONT_TITLE, 26, x + w * 0.5f, y + 24, "The way is barred", COL_RED, ALIGN_CENTER);
        ui_text_wrap(FONT_BODY, 17, x + 30, y + 74, w - 60, g_net_error[0] ? g_net_error : "Unknown error.", COL_TEXT, 22);
        if (ui_button("Back", x + w * 0.5f - 80, y + h - 66, 160, 44, BTN_PRIMARY) || ui_input()->keys[UIK_ESCAPE]) {
            session_stop();
            if (g_app.solo) local_server_stop();
            app_set_screen(g_app.solo ? SCR_SOLO : SCR_JOIN);
        }
        return;
    }
    float t = (float)g_app.time;
    ui_arc(x + w * 0.5f, y + 80, 34, 5, t * 3.0f, t * 3.0f + 4.2f, COL_GOLD);
    ui_arc(x + w * 0.5f, y + 80, 24, 3, -t * 2.0f, -t * 2.0f + 3.0f, UIC(200, 120, 60, 200));
    const char *stt = session_status_text();
    ui_text_shadow(FONT_TITLE, 22, x + w * 0.5f, y + 136, stt[0] ? stt : "Connecting...", COL_TEXT, ALIGN_CENTER);
    if (g_app.solo)
        ui_text(FONT_BODY, 15, x + w * 0.5f, y + 172, "The very first launch forges all 100 floors: it can take a little while.",
                COL_TEXT_FAINT, ALIGN_CENTER);
    if (ui_button("Cancel", x + w * 0.5f - 70, y + h - 56, 140, 40, BTN_NORMAL) || ui_input()->keys[UIK_ESCAPE]) {
        session_stop();
        if (g_app.solo) local_server_stop();
        app_set_screen(SCR_TITLE);
    }
}

/* ------------------------------------------------------------------ */

static void draw_settings_screen(void) {
    float W = ui_width(), H = ui_height();
    float w = 900, h = 720, x = (W - w) * 0.5f, y = (H - h) * 0.5f;
    if (!ui_window("Settings", x, y, w, h, true) || ui_input()->keys[UIK_ESCAPE]) {
        settings_save();
        app_set_screen(SCR_TITLE);
        return;
    }
    hud_settings_content(x + 24, y + 56, w - 48, h - 80);
}

static void draw_howto(void) {
    float W = ui_width(), H = ui_height();
    float w = 1000, h = 760, x = (W - w) * 0.5f, y = (H - h) * 0.5f;
    if (!ui_window("How to Play", x, y, w, h, true) || ui_input()->keys[UIK_ESCAPE]) {
        app_set_screen(SCR_TITLE);
        return;
    }
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

static void draw_credits(void) {
    float W = ui_width(), H = ui_height();
    float w = 760, h = 640, x = (W - w) * 0.5f, y = (H - h) * 0.5f;
    if (!ui_window("Credits", x, y, w, h, true) || ui_input()->keys[UIK_ESCAPE]) {
        app_set_screen(SCR_TITLE);
        return;
    }
    const char *text =
        "^8Dragon GL^0 - The Long Descent into the Dragon's Maw\n"
        "Created by ^8Nicola Taibi^0. Licensed under the GNU GPL v3 or later.\n\n"
        "^8DragonGL+^0\n"
        "New graphical client: menus, character creation, interface and HUD, procedural textures, "
        "animated creatures, dynamic lighting, an original procedurally synthesized soundtrack and "
        "sound effects, click-to-move, portable builds for Linux and Windows.\n\n"
        "^8Third-party components^0\n"
        "GLFW (zlib) - volk & Vulkan headers (MIT/Apache-2.0) - miniaudio (public domain) - "
        "stb_truetype / stb_image_write (public domain) - wepoll (BSD-2) - cJSON (MIT).\n"
        "Fonts: Cinzel and Alegreya Sans (SIL Open Font License 1.1).\n\n"
        "^8Music^0\n"
        "Every note is synthesized live by the game: \"The Long Descent\", \"Hearth of the Maw\", "
        "\"Echoes Below\", \"The Abyssal Choir\", \"Guardian of the Seal\".\n\n"
        "^9Thank you for playing. May your torch never go out.";
    ui_text_wrap(FONT_BODY, 17, x + 34, y + 64, w - 68, text, COL_TEXT, 23);
}

void screens_draw(void) {
    switch (g_app.screen) {
    case SCR_TITLE: draw_title(); break;
    case SCR_SOLO: draw_solo(); break;
    case SCR_JOIN: draw_join(); break;
    case SCR_CREATE: draw_create(); break;
    case SCR_CONNECTING: draw_connecting(); break;
    case SCR_SETTINGS: draw_settings_screen(); break;
    case SCR_HOWTO: draw_howto(); break;
    case SCR_CREDITS: draw_credits(); break;
    default: break;
    }
}
