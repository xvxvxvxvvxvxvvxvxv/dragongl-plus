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

/*Client game-state globals and the helpers that touch them.*/

#include "client_state.h"
#include "net.h"
#include "platform.h"
#include "ui.h"
#include <stdio.h>
#include <string.h>
#include <GLFW/glfw3.h>

atomic_int g_net_status = NET_IDLE;
char g_net_error[160];

int g_server_sock = -1;
int g_my_entity_id = -1;
int g_my_x = 0, g_my_y = 0;
int g_my_hp = 20, g_my_max_hp = 20;
Entity g_entities[CLIENT_MAX_ENTITIES];
int g_game_h = 8, g_game_m = 0, g_total_turns = 0;
int g_str = 10, g_dex = 10, g_con = 10, g_intel = 10, g_wis = 10, g_cha = 10;
int g_equipped_mask = 0;
int g_my_level = 1, g_my_xp = 0, g_my_floor = 0;
int g_my_xp_next = 300, g_stat_points = 0, g_max_floor = 0;
uint64_t g_my_gold = 0;
int g_my_ac = 10;
int g_vision_radius = 4;
float g_movement_cooldown = 0.2f;
char g_weapon_name[32] = "Unarmed";
char g_armor_name[32] = "None";
int g_to_hit = 0, g_to_dmg = 0;
int g_race_id = 0, g_subrace_id = -1, g_class_id = 0, g_alignment = 0;
uint32_t g_bosses_defeated = 0;
uint32_t g_status_icons = 0;
int g_hunger_level = 0;
int g_my_spell_slots[10];
int g_my_spell_slots_max[10];
char g_my_name[32];
TileType g_local_map[MAP_HEIGHT][MAP_WIDTH];

MsgInventory g_inventory;
uint32_t g_inventory_version;
MsgSpellbook g_spellbook;
uint32_t g_spellbook_version;
MsgShop g_shop;
uint32_t g_shop_version;

LogLine g_log[LOG_CAP];
uint32_t g_log_total;

ClientEvent g_evq[EVQ_CAP];
uint32_t g_evq_head;

pthread_mutex_t g_state_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t g_net_mutex = PTHREAD_MUTEX_INITIALIZER;
atomic_bool g_running = true;
atomic_bool g_map_dirty = false;
atomic_uint g_map_version = 1;

void evq_push(const ClientEvent *e) {
    g_evq[g_evq_head % EVQ_CAP] = *e;
    g_evq_head++;
}

int evq_read(uint32_t *cursor, ClientEvent *out, int n) {
    int k = 0;
    pthread_mutex_lock(&g_state_mutex);
    /*A consumer that fell more than a full ring behind skips ahead.*/
    if (g_evq_head - *cursor > EVQ_CAP) *cursor = g_evq_head - EVQ_CAP;
    while (*cursor != g_evq_head && k < n) {
        out[k++] = g_evq[*cursor % EVQ_CAP];
        (*cursor)++;
    }
    pthread_mutex_unlock(&g_state_mutex);
    return k;
}

void frame_snapshot_acquire(FrameSnapshot *snap) {
    pthread_mutex_lock(&g_state_mutex);
    snap->my_x = g_my_x;
    snap->my_y = g_my_y;
    snap->my_entity_id = g_my_entity_id;
    snap->my_floor = g_my_floor;
    snap->vision_radius = g_vision_radius;
    snap->movement_cooldown = g_movement_cooldown;
    snap->hp = g_my_hp;
    snap->max_hp = g_my_max_hp;
    snap->game_h = g_game_h;
    snap->game_m = g_game_m;
    unsigned mv = atomic_load(&g_map_version);
    if (snap->map_version != mv) {
        memcpy(snap->map, g_local_map, sizeof(snap->map));
        snap->map_version = mv;
    }
    uint64_t now = plat_time_ms();
    for (int i = 0; i < CLIENT_MAX_ENTITIES; i++) {
        Entity *e = &g_entities[i];
        if (e->active && e->killed_at_ms && now - e->killed_at_ms > 3000) {
            e->active = false; /*the corpse animation is long over*/
            e->id = 0;
        }
    }
    memcpy(snap->entities, g_entities, sizeof(snap->entities));
    for (int i = 0; i < CLIENT_MAX_ENTITIES; i++) {
        Entity *e = &snap->entities[i];
        e->killed = e->active && e->killed_at_ms != 0;
        if (e->killed) e->active = false; /*invisible to targeting, plates, models...*/
    }
    pthread_mutex_unlock(&g_state_mutex);
}

void client_send_move(int dx, int dy) {
    MsgHeader hdr = msg_hdr(MSG_MOVE, (int)sizeof(MsgMove));
    MsgMove m;
    if (g_my_entity_id == -1 || g_server_sock < 0) return;
    memset(&m, 0, sizeof(m));
    m.entity_id = g_my_entity_id;
    m.dx = dx;
    m.dy = dy;
    pthread_mutex_lock(&g_net_mutex);
    net_send(g_server_sock, &hdr, sizeof(MsgHeader));
    net_send(g_server_sock, &m, sizeof(MsgMove));
    pthread_mutex_unlock(&g_net_mutex);
}

void client_send_text_cmd(const char *cmd) {
    MsgHeader hdr = msg_hdr(MSG_TEXT_CMD, (int)sizeof(MsgTextCmd));
    MsgTextCmd m;
    if (g_server_sock < 0) return;
    memset(&m, 0, sizeof(m));
    copy_str(m.cmd, cmd, sizeof(m.cmd));
    pthread_mutex_lock(&g_net_mutex);
    net_send(g_server_sock, &hdr, sizeof(MsgHeader));
    net_send(g_server_sock, &m, sizeof(MsgTextCmd));
    pthread_mutex_unlock(&g_net_mutex);
}

/*Colour a log line from its [TAG] prefix.*/
static uint32_t log_color_for(const char *t) {
    while (*t == ' ') t++;
    static const struct { const char *tag; uint32_t col; } tags[] = {
        {"[COMBAT]", UIC(240, 110, 100, 255)},  {"[DANGER]", UIC(240, 90, 80, 255)},
        {"[ERROR]", UIC(230, 80, 70, 255)},     {"[VICTORY]", UIC(130, 230, 120, 255)},
        {"[LEVEL]", UIC(255, 215, 90, 255)},    {"[HEROIC", UIC(255, 205, 80, 255)},
        {"[MAGIC]", UIC(196, 140, 255, 255)},   {"[KI]", UIC(196, 140, 255, 255)},
        {"[GRIMORY]", UIC(196, 140, 255, 255)}, {"[MSG]", UIC(120, 220, 240, 255)},
        {"[SOCIAL]", UIC(120, 220, 240, 255)},  {"[PARTY]", UIC(120, 220, 240, 255)},
        {"[SAY]", UIC(150, 230, 250, 255)},     {"[HELP]", UIC(190, 180, 160, 255)},
        {"[TIP]", UIC(190, 200, 140, 255)},     {"[EVENT]", UIC(250, 160, 70, 255)},
        {"[WARNING]", UIC(250, 160, 70, 255)},  {"[CHARACTER]", UIC(222, 184, 96, 255)},
        {"[SYSTEM]", UIC(222, 200, 150, 255)},  {"[ACTION]", UIC(236, 226, 205, 255)},
        {"[DM]", UIC(255, 120, 200, 255)},      {"[TRAP]", UIC(250, 130, 60, 255)},
        {">", UIC(150, 140, 125, 255)},
    };
    for (size_t i = 0; i < sizeof(tags) / sizeof(tags[0]); i++)
        if (strncmp(t, tags[i].tag, strlen(tags[i].tag)) == 0) return tags[i].col;
    if (t[0] == '[') return UIC(200, 190, 170, 255);
    return UIC(190, 182, 166, 255);
}

void client_log_add_color(const char *text, uint32_t color) {
    pthread_mutex_lock(&g_state_mutex);
    LogLine *l = &g_log[g_log_total % LOG_CAP];
    copy_str(l->text, text, sizeof(l->text));
    l->color = color;
    l->time = glfwGetTime();
    g_log_total++;
    pthread_mutex_unlock(&g_state_mutex);
}

void client_log_add(const char *text) { client_log_add_color(text, log_color_for(text)); }

void client_state_reset(void) {
    pthread_mutex_lock(&g_state_mutex);
    g_my_entity_id = -1;
    memset(g_entities, 0, sizeof(g_entities));
    for (int y = 0; y < MAP_HEIGHT; y++)
        for (int x = 0; x < MAP_WIDTH; x++) g_local_map[y][x] = VOXEL_ROCK;
    memset(&g_inventory, 0, sizeof(g_inventory));
    memset(&g_spellbook, 0, sizeof(g_spellbook));
    memset(&g_shop, 0, sizeof(g_shop));
    g_inventory_version++;
    g_spellbook_version++;
    g_shop_version++;
    g_my_floor = 0;
    g_my_gold = 0;
    g_my_xp = 0;
    g_my_level = 1;
    g_bosses_defeated = 0;
    g_status_icons = 0;
    atomic_fetch_add(&g_map_version, 1);
    g_map_dirty = true;
    pthread_mutex_unlock(&g_state_mutex);
}
