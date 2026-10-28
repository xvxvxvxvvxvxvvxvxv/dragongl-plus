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

/*Game state mirrored from the server.
 *
 * Threading model (unchanged in spirit from the original client): the
 * NET thread writes everything below under g_state_mutex; the render /
 * UI thread takes a short snapshot under the same lock once per frame
 * (frame_snapshot_acquire) and then works on its private copy.*/
#ifndef CLIENT_STATE_H
#define CLIENT_STATE_H

#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include "map.h"
#include "protocol.h"

/*Client-side entity cache size (see the FEATURE LIMIT note in the
 * original code: on a crowded floor, entities beyond this are dropped;
 * the cache is flushed on every floor change).*/
#define CLIENT_MAX_ENTITIES 512

typedef struct {
    int id;
    int x, y, hp, max_hp;
    int floor_id;
    bool active;
    bool is_merchant;
    int shop_spec;      /*SHOP_SPEC_* from protocol.h*/
    bool is_tombstone;
    bool is_player;
    char username[32];  /*player name or NPC display name*/
    /*v4*/
    int kind;           /*KIND_* (protocol.h)*/
    int archetype;
    int level;
    int race_id, class_id;
    uint32_t look_seed;
    /*death: the server only reports the kill event; the entity is hidden
     * at once (the scene plays the fall as a corpse) and dropped shortly
     * after, unless it comes back (respawn) with a fresh state.*/
    uint64_t killed_at_ms;  /*0 = alive*/
    bool killed;            /*snapshot only: dead, kept for the death animation*/
} Entity;

/*Connection life cycle (driven by session.c / net_thread.c)*/
typedef enum {
    NET_IDLE = 0,
    NET_CONNECTING,
    NET_LOGGING_IN,
    NET_ONLINE,
    NET_FAILED,       /*connect or auth failed: g_net_error explains*/
    NET_DISCONNECTED  /*the server closed the connection*/
} NetStatus;

extern atomic_int g_net_status;
extern char g_net_error[160];

extern int g_server_sock;
extern int g_my_entity_id;
extern int g_my_x, g_my_y;
extern Entity g_entities[CLIENT_MAX_ENTITIES];
extern int g_my_hp, g_my_max_hp;
extern int g_game_h, g_game_m, g_total_turns;
extern int g_str, g_dex, g_con, g_intel, g_wis, g_cha;
extern int g_equipped_mask;
extern int g_my_level, g_my_xp, g_my_floor;
extern int g_my_xp_next, g_stat_points, g_max_floor;
extern uint64_t g_my_gold;
extern int g_my_ac;
extern int g_vision_radius;
extern float g_movement_cooldown;
extern char g_weapon_name[32];
extern char g_armor_name[32];
extern int g_to_hit, g_to_dmg;
extern int g_race_id, g_subrace_id, g_class_id, g_alignment;
extern uint32_t g_bosses_defeated;
extern uint32_t g_status_icons;
extern int g_hunger_level;
extern int g_my_spell_slots[10];
extern int g_my_spell_slots_max[10];
extern char g_my_name[32];
extern TileType g_local_map[MAP_HEIGHT][MAP_WIDTH];

/*Structured UI data (protocol v4); versions bump on every update.*/
extern MsgInventory g_inventory;
extern uint32_t g_inventory_version;
extern MsgSpellbook g_spellbook;
extern uint32_t g_spellbook_version;
extern MsgShop g_shop;            /*merchant_id == 0: no shop open*/
extern uint32_t g_shop_version;

/*--- Message log (ring) ---*/
#define LOG_CAP 400
typedef struct {
    char text[256];
    uint32_t color;   /*UIC packed colour chosen from the [TAG]*/
    double time;      /*glfwGetTime() when received (0 if unknown)*/
} LogLine;
extern LogLine g_log[LOG_CAP];
extern uint32_t g_log_total;  /*monotonic count of lines ever added*/

/*--- Event queue (net thread -> scene/audio/UI) ---
 * Server game events plus a few client-detected ones (gold delta,
 * HP delta, floor change...). Consumers keep their own read cursor.*/
enum {
    CEV_SERVER = 0,     /*payload: MsgGameEvent*/
    CEV_TEXT,           /*a log line arrived (payload text index)*/
    CEV_VFX,            /*spell VFX (MsgSpellVFX copied in ev.vfx)*/
    CEV_FLOOR_CHANGE,   /*value = new floor*/
    CEV_GOLD_DELTA,     /*value = delta*/
    CEV_HP_DELTA,       /*value = delta (own player)*/
    CEV_XP_DELTA,
    CEV_INV_CHANGED,
    CEV_WELCOME,
    CEV_SHOP_OPEN,
    CEV_SHOP_CLOSE,
};
typedef struct {
    int kind;
    MsgGameEvent ev;
    MsgSpellVFX vfx;
    int value;
    int x, y;  /*world tile where it happened (best effort)*/
} ClientEvent;
#define EVQ_CAP 256
extern ClientEvent g_evq[EVQ_CAP];
extern uint32_t g_evq_head; /*monotonic write index*/
void evq_push(const ClientEvent *e); /*caller holds g_state_mutex*/
/*Copy events in [*cursor, head) into out (max n); returns count.*/
int evq_read(uint32_t *cursor, ClientEvent *out, int n);

/*Per-frame snapshot (copied under a short lock, see above).*/
typedef struct {
    int my_x, my_y;
    int my_entity_id;
    int my_floor;
    int vision_radius;
    float movement_cooldown;
    int hp, max_hp;
    int game_h, game_m;
    TileType map[MAP_HEIGHT][MAP_WIDTH];
    Entity entities[CLIENT_MAX_ENTITIES];
    uint32_t map_version;
} FrameSnapshot;
void frame_snapshot_acquire(FrameSnapshot *snap); /*lock, copy, unlock*/

extern pthread_mutex_t g_state_mutex;
extern pthread_mutex_t g_net_mutex;
extern atomic_bool g_running;   /*the whole application*/
extern atomic_bool g_map_dirty;
extern atomic_uint g_map_version;

void client_send_move(int dx, int dy);
void client_send_text_cmd(const char *cmd);
/*Adds a line to the log (takes the lock itself).*/
void client_log_add(const char *text);
void client_log_add_color(const char *text, uint32_t color);
/*Reset all per-session state (before a new login).*/
void client_state_reset(void);

#endif // CLIENT_STATE_H
