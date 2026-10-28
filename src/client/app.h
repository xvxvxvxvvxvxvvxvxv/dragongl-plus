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

/*Application shell: window, main loop, screens and the state they share
 * (app.c = loop/input, screens.c = menus, hud.c = in-game interface).*/
#ifndef APP_H
#define APP_H

#include <stdbool.h>
#include "gfx.h"
#include "client_state.h"
#include "session.h"
#include "scene.h"
#include "ui.h"

typedef enum {
    SCR_TITLE = 0,
    SCR_SOLO,
    SCR_JOIN,
    SCR_CREATE,
    SCR_CONNECTING,
    SCR_GAME,
    SCR_SETTINGS,
    SCR_CREDITS,
    SCR_HOWTO,
} Screen;

/*In-game windows (bit flags)*/
enum {
    WIN_INVENTORY = 1 << 0,
    WIN_CHARACTER = 1 << 1,
    WIN_SPELLS = 1 << 2,
    WIN_MAP = 1 << 3,
    WIN_HELP = 1 << 4,
    WIN_SHOP = 1 << 5,
    WIN_PAUSE = 1 << 6,
    WIN_SETTINGS = 1 << 7,
};

#define HOTBAR_SPELLS 6

typedef struct {
    GLFWwindow *win;
    const GfxBackend *gfx;
    int fb_w, fb_h;
    float ui_scale;
    double time;
    float dt;
    bool focused;

    Screen screen, prev_screen;
    float screen_t;            /*seconds since the screen opened (transitions)*/
    int screen_frames;         /*frames since the screen opened*/

    /*login / creation*/
    LoginInfo login;
    bool create_from_solo;
    char err[256];
    bool solo;                 /*current session is the solo server*/
    char solo_heroes[16][32];
    int solo_nheroes;

    /*game*/
    FrameSnapshot *snap;       /*large: heap*/
    FrameSnapshot *title_snap; /*demo world for the menus*/
    uint32_t windows;          /*WIN_* open*/
    char chat[256];
    bool chat_open;
    char history[32][256];
    int nhistory, history_pos;
    int hover_id, target_id;
    int hover_tx, hover_ty;
    int path[2048];            /*x,y pairs*/
    int path_len, path_pos;
    bool traveling;            /*long auto-walk started from the map*/
    int path_target_id;        /*bump this entity at the end of the path*/
    double next_step_time;
    char spell_hotbar[HOTBAR_SPELLS][40];
    bool dead;
    double dead_time;
    double banner_time;
    char banner[96], banner_sub[128];
    double levelup_time;
    int levelup_level;
    float damage_flash;
    uint32_t ev_cursor_scene, ev_cursor_audio, ev_cursor_hud;
    double last_step_sound;
    int map_zoom;
    bool want_screenshot;
    double shot_time;
    char shot_path[512];
    int inv_tab;
    int char_tab;
    int help_tab;
    int pending_drop_idx;
    double last_log_view;
    bool request_quit;
    float combat_intensity;
    double last_combat;
    int last_floor_seen;
    bool tip_shown[16];
    double tip_time;
} App;

extern App g_app;

/*app.c*/
void app_set_screen(Screen s);
void app_start_session(const LoginInfo *li, bool solo);
void app_disconnect(const char *reason);
void app_send(const char *fmt, ...);
void app_apply_video_settings(void);
/*Auto-walk to an explored tile (map window): stops when enemies show up.*/
bool app_travel_to(int tx, int ty);

/*screens.c*/
void screens_draw(void);
void screens_title_snapshot_init(void);

/*hud.c*/
void hud_draw(void);
void hud_handle_events(void);
bool hud_game_input_allowed(void);
void hud_toggle(uint32_t win);
void hud_on_floor_change(int floor);
void hud_cast_hotbar(int i);
void hud_use_belt(int i);
const char *hud_context_action(int *icon); /*text of the [E] action*/
void hud_do_context_action(void);

#endif
