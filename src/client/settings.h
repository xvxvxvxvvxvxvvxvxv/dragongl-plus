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

/*Persistent client settings (settings.ini in the user profile:
 * ~/.config/dragongl on Linux, %APPDATA%\DragonGL on Windows).*/
#ifndef SETTINGS_H
#define SETTINGS_H

#include <stdbool.h>

typedef struct {
    /*video*/
    int backend;          /*0 = OpenGL, 1 = Vulkan*/
    bool fullscreen;
    bool vsync;
    float ui_scale;       /*multiplier over the automatic scale*/
    int quality;          /*0 low, 1 medium, 2 high (particles, lights)*/
    float brightness;
    int win_w, win_h;
    /*audio*/
    float vol_master, vol_music, vol_sfx;
    bool mute_unfocused;
    /*controls*/
    float cam_sensitivity;
    bool cam_relative_move;  /*WASD relative to the camera*/
    bool invert_zoom;
    /*gameplay / interface*/
    bool show_damage_numbers;
    bool show_all_names;
    bool show_hints;
    bool show_minimap;
    bool click_to_move;
    /*last login*/
    char last_host[128];
    int last_port;
    char last_user[32];
    char solo_user[32];
    bool tutorial_done;
} Settings;

extern Settings g_settings;

void settings_defaults(Settings *s);
void settings_load(void);
void settings_save(void);
const char *settings_path(void);

#endif
