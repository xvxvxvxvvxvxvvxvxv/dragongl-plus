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

/*Network receive loop: reads the server's frames (protocol v5 over TLS)
 * and applies them to the shared client state (client_state.h) and the
 * event queue.
 *
 * The socket is non-blocking; the thread waits for data (100 ms, so a
 * stop request is noticed quickly), appends what TLS decrypted to a
 * receive buffer and handles every complete frame in it. Any malformed
 * frame (wrong version, a type or length the server may not send, a
 * payload wire.c rejects) ends the session: the stream can no longer
 * be trusted.*/

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <pthread.h>
#include "client_state.h"
#include "client_tls.h"
#include "session.h"
#include "platform.h"
#include "protocol.h"
#include "wire.h"
#include "net.h"

static void fail(NetStatus st, const char *why) {
    pthread_mutex_lock(&g_state_mutex);
    copy_str(g_net_error, why, sizeof(g_net_error));
    pthread_mutex_unlock(&g_state_mutex);
    atomic_store(&g_net_status, st);
}

static void push_simple(int kind, int value, int x, int y) {
    ClientEvent e;
    memset(&e, 0, sizeof(e));
    e.kind = kind;
    e.value = value;
    e.x = x;
    e.y = y;
    evq_push(&e);
}

static void push_gev(int type, int src, int dst, int value, int x, int y) {
    ClientEvent e;
    memset(&e, 0, sizeof(e));
    e.kind = CEV_SERVER;
    e.ev.type = type;
    e.ev.src_id = src;
    e.ev.dst_id = dst;
    e.ev.value = value;
    e.ev.x = (int16_t)x;
    e.ev.y = (int16_t)y;
    e.value = value;
    e.x = x;
    e.y = y;
    evq_push(&e);
}

/* ------------------------------------------------------------------ */
/* What a server without the DragonGL+ extension does not send          */
/* ------------------------------------------------------------------ */

static int xp_threshold(int level) {
    static const int T[20] = {0, 300, 900, 2700, 6500, 14000, 23000, 34000, 48000, 64000,
                              85000, 100000, 120000, 140000, 165000, 195000, 225000, 265000, 305000, 355000};
    if (level < 1) return 0;
    if (level > 20) return 355000;
    return T[level - 1];
}

/*Identity of an entity from its plain v5 state: players, merchants and
 * tombstones say what they are; for creatures only the hit points are
 * known (treasure chests and gold piles use 9999).*/
static void entity_kind_from_state(Entity *e, const MsgState *s) {
    e->archetype = 0;
    e->level = 0;
    e->race_id = 0;
    e->class_id = 0;
    e->look_seed = (uint32_t)s->entity_id * 2654435761u;
    if (s->is_tombstone) {
        e->kind = KIND_TOMBSTONE;
    } else if (s->is_player) {
        e->kind = KIND_PLAYER;
    } else if (s->is_merchant) {
        e->kind = KIND_MERCHANT;
        if (!e->username[0]) copy_str(e->username, "Merchant", sizeof(e->username));
    } else if (s->max_hp == 9999) {
        e->kind = KIND_CHEST;
        if (!e->username[0]) copy_str(e->username, "Treasure", sizeof(e->username));
    } else {
        e->kind = KIND_HUMANOID;
        if (!e->username[0]) copy_str(e->username, "Creature", sizeof(e->username));
    }
}

/*Without game events: derive them from HP changes so damage numbers,
 * hit flashes and death animations still play.*/
static void hp_change_events(Entity *e, int new_hp) {
    if (!client_classic()) return;
    if (e->is_player || e->is_merchant || e->is_tombstone || e->kind == KIND_CHEST) return;
    if (new_hp < e->hp && new_hp > 0)
        push_gev(GEV_HURT, 0, e->id, e->hp - new_hp, e->x, e->y);
    if (new_hp <= 0 && e->hp > 0 && !e->killed_at_ms) {
        bool near_me = abs(e->x - g_my_x) <= 1 && abs(e->y - g_my_y) <= 1;
        push_gev(GEV_KILL, near_me ? g_my_entity_id : 0, e->id, 0, e->x, e->y);
        e->killed_at_ms = plat_time_ms();
    }
}

/* ------------------------------------------------------------------ */
/* Handlers (payload already decoded and validated by wire.c)           */
/* ------------------------------------------------------------------ */

static int s_last_floor = -1;

static void clear_floor_locked(void) {
    for (int y = 0; y < MAP_HEIGHT; y++)
        for (int x = 0; x < MAP_WIDTH; x++) g_local_map[y][x] = VOXEL_ROCK;
    for (int i = 0; i < CLIENT_MAX_ENTITIES; i++) {
        g_entities[i].active = false;
        g_entities[i].id = 0;
        g_entities[i].floor_id = -1;
    }
    atomic_fetch_add(&g_map_version, 1);
    g_map_dirty = true;
}

static void entity_from_state(Entity *e, const MsgState *s) {
    bool same = e->active && e->id == s->entity_id;
    e->id = s->entity_id;
    e->x = s->x;
    e->y = s->y;
    e->hp = s->hp;
    e->max_hp = s->max_hp > 0 ? s->max_hp : (s->hp > 0 ? s->hp : 1);
    e->floor_id = s->floor_id;
    e->active = true;
    e->killed_at_ms = 0;
    e->is_merchant = s->is_merchant != 0;
    e->shop_spec = s->shop_spec;
    e->is_tombstone = s->is_tombstone != 0;
    e->is_player = s->is_player != 0;
    copy_str(e->username, s->username, sizeof(e->username));
    /*the extension (ENTITY_INFO) may already have told who this is*/
    if (!same || !e->has_info) {
        e->has_info = false;
        entity_kind_from_state(e, s);
    }
}

static void handle_own_state(const MsgState *s) {
    /*Deltas for feedback (sounds, floating numbers)*/
    if (g_my_entity_id >= 0) {
        long long dg = (long long)s->gold - (long long)g_my_gold;
        if (dg != 0) push_simple(CEV_GOLD_DELTA, (int)dg, s->x, s->y);
        int dh = s->hp - g_my_hp;
        if (dh != 0 && s->floor_id == g_my_floor) push_simple(CEV_HP_DELTA, dh, s->x, s->y);
        int dx = s->xp - g_my_xp;
        if (dx > 0) push_simple(CEV_XP_DELTA, dx, s->x, s->y);
    }
    g_my_x = s->x;
    g_my_y = s->y;
    g_my_hp = s->hp;
    g_my_max_hp = s->max_hp;
    g_game_h = s->game_hour;
    g_game_m = s->game_min;
    g_total_turns = s->total_turns;
    g_str = s->str; g_dex = s->dex; g_con = s->con;
    g_intel = s->intel; g_wis = s->wis; g_cha = s->cha;
    g_movement_cooldown = s->movement_cooldown;
    g_equipped_mask = s->equipped_mask;
    g_my_level = s->level;
    g_my_xp = s->xp;
    g_my_gold = s->gold;
    g_my_ac = s->ac;
    g_my_floor = s->floor_id;
    g_vision_radius = s->vision_radius;
    copy_str(g_weapon_name, s->weapon_name, sizeof(g_weapon_name));
    copy_str(g_armor_name, s->armor_name, sizeof(g_armor_name));
    g_to_hit = s->to_hit;
    g_to_dmg = s->to_dmg;
    g_bosses_defeated = s->bosses_defeated;
    g_status_icons = s->status_icons;
    g_hunger_level = s->hunger_level;
    for (int i = 0; i < 10; i++) {
        g_my_spell_slots[i] = s->spell_slots[i];
        g_my_spell_slots_max[i] = s->spell_slots_max[i];
    }
    if (s->username[0]) copy_str(g_my_name, s->username, sizeof(g_my_name));
    if (client_classic()) {
        /*the extension (PLAYER_EXT) sends these; otherwise derive them*/
        g_my_xp_next = xp_threshold(s->level + 1);
        if (s->floor_id > g_max_floor) g_max_floor = s->floor_id;
    }
}

static void on_state(const MsgState *s) {
    pthread_mutex_lock(&g_state_mutex);
    if (s->entity_id == g_my_entity_id) {
        if (s_last_floor != -1 && s_last_floor != s->floor_id) {
            clear_floor_locked();
            push_simple(CEV_FLOOR_CHANGE, s->floor_id, s->x, s->y);
        }
        s_last_floor = s->floor_id;
        handle_own_state(s);
    } else if (s->floor_id == g_my_floor) {
        int free_slot = -1, slot = -1;
        for (int i = 0; i < CLIENT_MAX_ENTITIES; i++) {
            if (g_entities[i].active && g_entities[i].id == s->entity_id) { slot = i; break; }
            if (free_slot < 0 && !g_entities[i].active) free_slot = i;
        }
        if (s->hp <= 0 && slot >= 0) hp_change_events(&g_entities[slot], s->hp);
        if (s->hp <= 0) {
            /*died or left the floor; a creature just killed stays for its
             * death animation (killed_at_ms, dropped by the snapshot)*/
            if (slot >= 0 && !g_entities[slot].killed_at_ms) {
                g_entities[slot].active = false;
                g_entities[slot].id = 0;
                g_entities[slot].floor_id = -1;
            }
        } else {
            if (slot < 0) slot = free_slot;
            /*FEATURE LIMIT: with every cache slot in use a new entity is
             * not drawn; the cache is flushed on every floor change.*/
            if (slot >= 0) entity_from_state(&g_entities[slot], s);
        }
    }
    pthread_mutex_unlock(&g_state_mutex);
}

static void on_entity_update(const EntityUpdateRec *recs, int count) {
    pthread_mutex_lock(&g_state_mutex);
    for (int k = 0; k < count; k++)
        for (int i = 0; i < CLIENT_MAX_ENTITIES; i++)
            if (g_entities[i].active && g_entities[i].id == recs[k].entity_id) {
                hp_change_events(&g_entities[i], recs[k].hp);
                g_entities[i].x = recs[k].x;
                g_entities[i].y = recs[k].y;
                g_entities[i].hp = recs[k].hp;
                if (recs[k].hp > g_entities[i].max_hp) g_entities[i].max_hp = recs[k].hp;
                if (recs[k].hp <= 0 && !g_entities[i].is_tombstone && !g_entities[i].killed_at_ms)
                    g_entities[i].killed_at_ms = plat_time_ms();
                break;
            }
    pthread_mutex_unlock(&g_state_mutex);
}

static void on_map_chunk(const MsgMapChunk *mc) {
    pthread_mutex_lock(&g_state_mutex);
    /*The server re-sends the window around the player on every step:
     * mark the map dirty (mesh rebuild) only if a tile changed.*/
    bool changed = false;
    int i = 0;
    for (int cy = mc->start_y; cy < mc->start_y + mc->height; cy++)
        for (int cx = mc->start_x; cx < mc->start_x + mc->width; cx++, i++)
            if (cx < MAP_WIDTH && cy < MAP_HEIGHT && g_local_map[cy][cx] != mc->tiles[i]) {
                g_local_map[cy][cx] = (TileType)mc->tiles[i];
                changed = true;
            }
    if (changed) {
        atomic_fetch_add(&g_map_version, 1);
        g_map_dirty = true;
    }
    pthread_mutex_unlock(&g_state_mutex);
}

/*What the server's floating combat text says about the own hero.*/
static void on_combat_event(const MsgCombatEvent *ev) {
    pthread_mutex_lock(&g_state_mutex);
    switch (ev->kind) {
    case COMBAT_EV_MISS_TAKEN:
        push_gev(GEV_ATTACK_MISS, 0, g_my_entity_id, 0, g_my_x, g_my_y);
        break;
    case COMBAT_EV_LEVEL_UP:
        if (client_classic()) push_gev(GEV_LEVEL_UP, g_my_entity_id, g_my_entity_id, ev->amount, g_my_x, g_my_y);
        break;
    default:
        /*damage, heal and gold reach the hero through its state (HP and
         * gold deltas) and the entity updates of the creatures*/
        break;
    }
    pthread_mutex_unlock(&g_state_mutex);
}

/*Decode and apply one frame. False = malformed payload.*/
static bool handle_frame(MsgType type, const uint8_t *p, size_t n) {
    switch (type) {
    case MSG_WELCOME: {
        MsgWelcome w;
        if (!wire_get_welcome(p, n, &w)) return false;
        pthread_mutex_lock(&g_state_mutex);
        g_my_entity_id = w.entity_id;
        g_my_x = w.x;
        g_my_y = w.y;
        g_my_hp = w.hp;
        g_my_max_hp = w.max_hp;
        g_my_gold = w.gold;
        g_my_level = w.level;
        g_race_id = w.race_id;
        g_subrace_id = w.subrace_id;
        g_class_id = w.class_id;
        g_alignment = w.alignment;
        push_simple(CEV_WELCOME, w.is_new, w.x, w.y);
        pthread_mutex_unlock(&g_state_mutex);
        atomic_store(&g_net_status, NET_ONLINE);
        return true;
    }
    case MSG_AUTH_FAIL: {
        MsgAuthFail f;
        if (!wire_get_auth_fail(p, n, &f)) return false;
        fail(NET_FAILED, f.reason);
        return true;
    }
    case MSG_STATE: {
        static MsgState s; /*~1.3 KB: not on the thread's stack every time*/
        if (!wire_get_state(p, n, &s)) return false;
        on_state(&s);
        return true;
    }
    case MSG_ENTITY_UPDATE: {
        static MsgEntityUpdate eu; /*48 KiB*/
        if (!wire_get_entity_update(p, n, &eu)) return false;
        on_entity_update(eu.recs, eu.count);
        return true;
    }
    case MSG_MAP_CHUNK: {
        static MsgMapChunk mc;
        if (!wire_get_map_chunk(p, n, &mc)) return false;
        on_map_chunk(&mc);
        return true;
    }
    case MSG_SPELL_VFX: {
        ClientEvent e;
        memset(&e, 0, sizeof(e));
        if (!wire_get_spell_vfx(p, n, &e.vfx)) return false;
        e.kind = CEV_VFX;
        e.x = e.vfx.target_x;
        e.y = e.vfx.target_y;
        pthread_mutex_lock(&g_state_mutex);
        evq_push(&e);
        pthread_mutex_unlock(&g_state_mutex);
        return true;
    }
    case MSG_TEXT: {
        MsgText t;
        if (!wire_get_text(p, n, &t)) return false;
        client_log_add(t.text);
        pthread_mutex_lock(&g_state_mutex);
        push_simple(CEV_TEXT, (int)(g_log_total - 1), g_my_x, g_my_y);
        /*without game events the hero's death is told only as text*/
        if (client_classic() && (strstr(t.text, "[SYSTEM] You died") || strstr(t.text, "[DEATH] You died")))
            push_gev(GEV_PLAYER_DIED, 0, g_my_entity_id, 0, g_my_x, g_my_y);
        pthread_mutex_unlock(&g_state_mutex);
        return true;
    }
    case MSG_COMBAT_EVENT: {
        MsgCombatEvent ev;
        if (!wire_get_combat_event(p, n, &ev)) return false;
        on_combat_event(&ev);
        return true;
    }
    case MSG_TIME_SYNC: {
        MsgTimeSync ts;
        if (!wire_get_time_sync(p, n, &ts)) return false;
        pthread_mutex_lock(&g_state_mutex);
        g_game_h = ts.game_hour;
        g_game_m = ts.game_min;
        g_total_turns = ts.total_turns;
        pthread_mutex_unlock(&g_state_mutex);
        return true;
    }
    case MSG_TOMBSTONE_REMOVE: {
        MsgTombstoneRemove rm;
        if (!wire_get_tombstone_remove(p, n, &rm)) return false;
        pthread_mutex_lock(&g_state_mutex);
        for (int i = 0; i < CLIENT_MAX_ENTITIES; i++)
            if (g_entities[i].active && g_entities[i].id == rm.entity_id && g_entities[i].is_tombstone) {
                g_entities[i].active = false;
                g_entities[i].is_tombstone = false;
                break;
            }
        pthread_mutex_unlock(&g_state_mutex);
        return true;
    }
    default:
        return false;
    }
}

/*Handle every complete frame at the start of buf; returns the bytes
 * consumed, or -1 if the stream is broken (session ended).*/
static long consume_frames(const uint8_t *buf, size_t len) {
    size_t off = 0;
    while (len - off >= WIRE_HEADER_SIZE) {
        MsgHeader h;
        wire_read_header(buf + off, &h);
        if (h.version != PROTOCOL_VERSION) {
            char why[160];
            snprintf(why, sizeof(why),
                     "Protocol mismatch: the server speaks version %u, this game %u. Update both to the same release.",
                     (unsigned)h.version, (unsigned)PROTOCOL_VERSION);
            fail(NET_FAILED, why);
            return -1;
        }
        uint32_t max = wire_server_payload_max(h.type);
        if (max == 0 || h.length > max) {
            fail(NET_FAILED, "Protocol error (unexpected message) - disconnected.");
            return -1;
        }
        if (len - off < WIRE_HEADER_SIZE + (size_t)h.length) break; /*rest not arrived yet*/
        atomic_store(&g_net_rx_any, true);
        if (!handle_frame(h.type, buf + off + WIRE_HEADER_SIZE, h.length)) {
            fail(NET_FAILED, "Protocol error (malformed message) - disconnected.");
            return -1;
        }
        off += WIRE_HEADER_SIZE + (size_t)h.length;
        if (atomic_load(&g_net_status) == NET_FAILED) return -1; /*authentication refused*/
    }
    return (long)off;
}

void *net_thread_loop(void *arg) {
    (void)arg;
    static uint8_t rx[WIRE_HEADER_SIZE + WIRE_MAX_PAYLOAD];
    size_t rx_len = 0;
    s_last_floor = -1;
    while (g_running && !session_stop_requested()) {
        /*Bytes TLS already decrypted are not visible to poll().*/
        if (!client_tls_pending(g_tls)) {
            int r = plat_poll_read(g_server_sock, 100);
            if (r == 0) continue;
            if (r < 0) {
                fail(NET_DISCONNECTED, "Connection lost.");
                break;
            }
        }
        char why[160] = "";
        long got = client_tls_recv(g_tls, rx + rx_len, sizeof(rx) - rx_len, why, sizeof(why));
        if (got == 0) continue;
        if (got == -1) {
            if (atomic_load(&g_net_status) != NET_FAILED)
                fail(NET_DISCONNECTED, "The server closed the connection.");
            break;
        }
        if (got < 0) {
            char msg[200];
            snprintf(msg, sizeof(msg), "Connection lost (%s).", why);
            fail(NET_DISCONNECTED, msg);
            break;
        }
        rx_len += (size_t)got;
        long used = consume_frames(rx, rx_len);
        if (used < 0) break;
        memmove(rx, rx + used, rx_len - (size_t)used);
        rx_len -= (size_t)used;
    }
    return NULL;
}
