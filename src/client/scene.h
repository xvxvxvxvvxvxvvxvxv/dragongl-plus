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

/*The 3D scene: camera, world, creatures, effects and lighting, turned
 * into a renderer-independent SceneFrame every frame.*/
#ifndef SCENE_H
#define SCENE_H

#include "client_state.h"
#include "gfx.h"
#include "mathx.h"

typedef struct {
    int id;
    int slot;
    float sx, sy;     /*screen position of the head (logical px)*/
    float dist;       /*distance from the player (tiles)*/
    float hp_frac;
    int hp, max_hp;
    int kind, level, archetype;
    bool is_player, is_merchant, hostile, is_boss;
    char name[32];
} SceneLabel;

typedef struct {
    bool title_mode;          /*menu background: slow orbit, no player*/
    int quality;              /*0..2*/
    float brightness;
    int hover_id, target_id;  /*entity ids (0 = none)*/
    int hover_tx, hover_ty;   /*hovered tile (-1 = none)*/
    const int *path;          /*click-to-move path as x,y pairs*/
    int path_len;
    /*own appearance*/
    const char *weapon_name;
    bool shield, helm, cape;
    int race_id, class_id;
    float ui_scale;
    /*character creation: show the hero model in the menu scene*/
    bool preview;
    int preview_race, preview_class;
    float preview_offset_x;   /*shift the model sideways on screen*/
    int preview_x, preview_y; /*open tile where the model stands*/
} SceneInput;

void scene_init(void);
void scene_new_floor(int floor);
void scene_frame(const FrameSnapshot *snap, const SceneInput *in, float dt, double time,
                 int fb_w, int fb_h, SceneFrame *out);
void scene_handle_event(const ClientEvent *e, const FrameSnapshot *snap);

int scene_labels(const SceneLabel **out);
/*Logical screen px -> tile (ray vs ground plane).*/
bool scene_screen_to_tile(float lx, float ly, int *tx, int *ty);
/*Entity under the cursor (0 if none).*/
int scene_entity_at(float lx, float ly);
bool scene_world_to_screen(v3 p, float *lx, float *ly);
v3 scene_player_pos(void);

void scene_cam_orbit(float dyaw, float dpitch);
void scene_cam_zoom(float factor);
void scene_cam_reset(void);
float scene_cam_yaw(void);

/*Explored tiles of the current floor (MAP_WIDTH x MAP_HEIGHT bytes).*/
const uint8_t *scene_explored(void);
/*Developer aid (DRAGONGL_GALLERY): fixed close camera on the menu.*/
void scene_set_gallery(bool on);
bool scene_tile_explored(int x, int y);
/*Floating combat texts projected to the screen (for the UI).*/
typedef struct { float sx, sy, alpha, scale; int type; const char *text; } ScreenText;
int scene_screen_texts(ScreenText *out, int max);

#endif
