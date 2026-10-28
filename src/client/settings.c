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

#include "settings.h"
#include "platform.h"
#include "net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Settings g_settings;
static char s_path[1024];

void settings_defaults(Settings *s) {
    memset(s, 0, sizeof(*s));
    s->backend = 0;
    s->fullscreen = false;
    s->vsync = true;
    s->ui_scale = 1.0f;
    s->quality = 2;
    s->brightness = 1.0f;
    s->win_w = 1600;
    s->win_h = 900;
    s->vol_master = 0.8f;
    s->vol_music = 0.6f;
    s->vol_sfx = 0.8f;
    s->mute_unfocused = true;
    s->cam_sensitivity = 1.0f;
    s->cam_relative_move = true;
    s->show_damage_numbers = true;
    s->show_all_names = false;
    s->show_hints = true;
    s->show_minimap = true;
    s->click_to_move = true;
    copy_str(s->last_host, "127.0.0.1", sizeof(s->last_host));
    s->last_port = 8080;
}

const char *settings_path(void) {
    if (!s_path[0]) {
        char dir[900];
        if (!plat_user_dir(dir, sizeof(dir))) snprintf(dir, sizeof(dir), ".");
        snprintf(s_path, sizeof(s_path), "%s/settings.ini", dir);
    }
    return s_path;
}

#define KEYS(X)                                                        \
    X(backend, "%d", INT) X(fullscreen, "%d", BOOL) X(vsync, "%d", BOOL) \
    X(ui_scale, "%f", FLOAT) X(quality, "%d", INT) X(brightness, "%f", FLOAT) \
    X(win_w, "%d", INT) X(win_h, "%d", INT)                            \
    X(vol_master, "%f", FLOAT) X(vol_music, "%f", FLOAT) X(vol_sfx, "%f", FLOAT) \
    X(mute_unfocused, "%d", BOOL) X(cam_sensitivity, "%f", FLOAT)      \
    X(cam_relative_move, "%d", BOOL) X(invert_zoom, "%d", BOOL)        \
    X(show_damage_numbers, "%d", BOOL) X(show_all_names, "%d", BOOL)   \
    X(show_hints, "%d", BOOL) X(show_minimap, "%d", BOOL)              \
    X(click_to_move, "%d", BOOL) X(last_port, "%d", INT)               \
    X(tutorial_done, "%d", BOOL)

void settings_load(void) {
    settings_defaults(&g_settings);
    FILE *f = fopen(settings_path(), "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = line, *v = eq + 1;
        v[strcspn(v, "\r\n")] = '\0';
#define INT(field) g_settings.field = atoi(v)
#define BOOL(field) g_settings.field = atoi(v) != 0
#define FLOAT(field) g_settings.field = (float)atof(v)
#define LOAD(field, fmt, T) if (strcmp(k, #field) == 0) { T(field); continue; }
        KEYS(LOAD)
#undef LOAD
#undef INT
#undef BOOL
#undef FLOAT
        if (strcmp(k, "last_host") == 0) copy_str(g_settings.last_host, v, sizeof(g_settings.last_host));
        else if (strcmp(k, "last_user") == 0) copy_str(g_settings.last_user, v, sizeof(g_settings.last_user));
        else if (strcmp(k, "solo_user") == 0) copy_str(g_settings.solo_user, v, sizeof(g_settings.solo_user));
    }
    fclose(f);
    if (g_settings.ui_scale < 0.6f || g_settings.ui_scale > 2.5f) g_settings.ui_scale = 1.0f;
    if (g_settings.quality < 0 || g_settings.quality > 2) g_settings.quality = 2;
    if (g_settings.win_w < 800) g_settings.win_w = 1600;
    if (g_settings.win_h < 600) g_settings.win_h = 900;
}

void settings_save(void) {
    FILE *f = fopen(settings_path(), "w");
    if (!f) return;
    fprintf(f, "# DragonGL client settings\n");
#define INT(field) fprintf(f, "%s=%d\n", #field, g_settings.field)
#define BOOL(field) fprintf(f, "%s=%d\n", #field, g_settings.field ? 1 : 0)
#define FLOAT(field) fprintf(f, "%s=%.3f\n", #field, (double)g_settings.field)
#define SAVE(field, fmt, T) T(field);
    KEYS(SAVE)
#undef SAVE
#undef INT
#undef BOOL
#undef FLOAT
    fprintf(f, "last_host=%s\n", g_settings.last_host);
    fprintf(f, "last_user=%s\n", g_settings.last_user);
    fprintf(f, "solo_user=%s\n", g_settings.solo_user);
    fclose(f);
}
