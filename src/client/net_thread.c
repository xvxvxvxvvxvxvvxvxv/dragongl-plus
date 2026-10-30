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

/*Network receive loop: parses every server message into the shared
 * client state (client_state.h) and the event queue.
 *
 * Unchanged invariants from the original client: header version and
 * type/length are validated BEFORE any payload is read (a mismatched
 * or malicious peer is dropped instead of desynchronising the stream),
 * and every server-provided size is bounds-checked before allocating.*/

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <pthread.h>
#include "client_state.h"
#include "session.h"
#include "platform.h"
#include "protocol.h"
#include "net.h"

/*Size of MsgState on the wire: classic servers end it at the v4 fields.*/
static int wire_state_size(void) {
    return client_classic() ? (int)offsetof(MsgState, kind) : (int)sizeof(MsgState);
}

static bool valid_msg(MsgType type, int length) {
    if (client_classic() && type >= MSG_INVENTORY) return false; /*v4-only messages*/
    switch (type) {
    case MSG_WELCOME:          return length == sizeof(MsgWelcome);
    case MSG_STATE:            return length == wire_state_size();
    case MSG_MAP_CHUNK:        return (size_t)length >= sizeof(MsgMapChunk);
    case MSG_TEXT_CMD:         return length == sizeof(MsgTextCmd);
    case MSG_TEXT:             return length == sizeof(MsgText);
    case MSG_AUTH_FAIL:        return length == sizeof(MsgAuthFail);
    case MSG_SPELL_VFX:        return length == sizeof(MsgSpellVFX);
    case MSG_TOMBSTONE_REMOVE: return length == sizeof(MsgTombstoneRemove);
    case MSG_TIME_SYNC:        return length == sizeof(MsgTimeSync);
    case MSG_ENTITY_UPDATE:    return length >= (int)sizeof(int32_t);
    case MSG_INVENTORY:        return length == sizeof(MsgInventory);
    case MSG_SHOP:             return length == sizeof(MsgShop);
    case MSG_SPELLBOOK:        return length == sizeof(MsgSpellbook);
    case MSG_GAME_EVENT:       return length == sizeof(MsgGameEvent);
    default:                   return false;
    }
}

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

/* ------------------------------------------------------------------ */
/* Classic servers (original Dragon GL, protocol v3)                    */
/* ------------------------------------------------------------------ */

static int xp_threshold(int level) {
    static const int T[20] = {0, 300, 900, 2700, 6500, 14000, 23000, 34000, 48000, 64000,
                              85000, 100000, 120000, 140000, 165000, 195000, 225000, 265000, 305000, 355000};
    if (level < 1) return 0;
    if (level > 20) return 355000;
    return T[level - 1];
}

/*A classic MsgState ends before the v4 fields: rebuild what the client
 * can (caller holds g_state_mutex).*/
static void classic_fill_state(MsgState *s) {
    if (s->entity_id == g_my_entity_id) {
        s->xp_next = xp_threshold(s->level + 1);
        s->race_id = g_race_id;   /*from the welcome message*/
        s->class_id = g_class_id;
        s->max_floor = s->floor_id > g_max_floor ? s->floor_id : g_max_floor;
        return;
    }
    if (s->is_player || s->is_tombstone) return;
    if (s->is_merchant) {
        if (!s->username[0]) copy_str(s->username, "Merchant", sizeof(s->username));
        return;
    }
    /*No identity is sent for creatures. Loot (treasure chests, gold
     * piles, remains) uses fixed hit points: 9999 or 1.*/
    if (s->max_hp == 9999 || s->max_hp == 1) {
        s->kind = KIND_CHEST;
        if (!s->username[0]) copy_str(s->username, "Treasure", sizeof(s->username));
        return;
    }
    s->kind = KIND_HUMANOID;
    s->archetype = 0;
    if (!s->username[0]) copy_str(s->username, "Creature", sizeof(s->username));
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

/*Classic servers send no combat events: derive them from HP changes so
 * damage numbers, hit flashes and death animations still play.*/
static void classic_hp_change(Entity *e, int new_hp) {
    if (e->is_player || e->is_merchant || e->is_tombstone || e->kind == KIND_CHEST) return;
    if (new_hp < e->hp && new_hp > 0)
        push_gev(GEV_HURT, 0, e->id, e->hp - new_hp, e->x, e->y);
    if (new_hp <= 0 && e->hp > 0 && !e->killed_at_ms) {
        bool near_me = abs(e->x - g_my_x) <= 1 && abs(e->y - g_my_y) <= 1;
        push_gev(GEV_KILL, near_me ? g_my_entity_id : 0, e->id, 0, e->x, e->y);
        e->killed_at_ms = plat_time_ms();
    }
}

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
    e->kind = s->kind;
    if (e->is_tombstone) e->kind = KIND_TOMBSTONE;
    else if (e->is_player) e->kind = KIND_PLAYER;
    else if (e->is_merchant) e->kind = KIND_MERCHANT;
    e->archetype = s->archetype;
    e->level = s->npc_level;
    e->race_id = s->race_id;
    e->class_id = s->class_id;
    e->look_seed = s->look_seed;
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
    g_my_xp_next = s->xp_next;
    g_stat_points = s->stat_points;
    g_max_floor = s->max_floor;
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
    g_race_id = s->race_id;
    g_class_id = s->class_id;
}

void *net_thread_loop(void *arg) {
    (void)arg;
    MsgHeader hdr;
    static MsgState st;
    static MsgInventory inv;
    static MsgSpellbook book;
    static MsgShop shop;
    int last_floor = -1;

    while (g_running && !session_stop_requested()) {
        /*Wait (bounded) for data so a stop request is noticed quickly,
         * then make sure the peer did not simply close the socket.*/
        int r = net_wait_readable(g_server_sock, 100);
        if (r == 0) continue;
        if (r < 0 || net_peek_closed(g_server_sock)) {
            fail(NET_DISCONNECTED, "The server closed the connection.");
            break;
        }
        int bytes = net_receive_exact(g_server_sock, &hdr, sizeof(MsgHeader));
        if (bytes < 0) {
            fail(NET_DISCONNECTED, "Connection lost.");
            break;
        }
        if (hdr.version != g_proto_version) {
            char why[160];
            snprintf(why, sizeof(why),
                     "Protocol mismatch: server v%u, client v%d. Update both to the same release.",
                     hdr.version, g_proto_version);
            fail(NET_FAILED, why);
            break;
        }
        if (!valid_msg(hdr.type, hdr.length)) {
            fail(NET_FAILED, "Protocol error (bad message) - disconnected.");
            break;
        }
        atomic_store(&g_net_rx_any, true);

        switch (hdr.type) {
        case MSG_WELCOME: {
            MsgWelcome w;
            if (net_receive_all(g_server_sock, &w, sizeof(w)) <= 0) goto io_error;
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
            break;
        }
        case MSG_AUTH_FAIL: {
            MsgAuthFail f;
            if (net_receive_all(g_server_sock, &f, sizeof(f)) <= 0) goto io_error;
            f.reason[sizeof(f.reason) - 1] = '\0';
            fail(NET_FAILED, f.reason);
            goto done;
        }
        case MSG_STATE: {
            memset(&st, 0, sizeof(st));
            if (net_receive_all(g_server_sock, &st, wire_state_size()) <= 0) goto io_error;
            st.username[sizeof(st.username) - 1] = '\0';
            pthread_mutex_lock(&g_state_mutex);
            if (client_classic()) classic_fill_state(&st);
            if (st.entity_id == g_my_entity_id) {
                if (last_floor != -1 && last_floor != st.floor_id) {
                    clear_floor_locked();
                    push_simple(CEV_FLOOR_CHANGE, st.floor_id, st.x, st.y);
                }
                last_floor = st.floor_id;
                handle_own_state(&st);
            } else if (st.floor_id == g_my_floor) {
                int free_slot = -1, slot = -1;
                for (int i = 0; i < CLIENT_MAX_ENTITIES; i++) {
                    if (g_entities[i].active && g_entities[i].id == st.entity_id) { slot = i; break; }
                    if (free_slot < 0 && !g_entities[i].active) free_slot = i;
                }
                if (st.hp <= 0 && client_classic() && slot >= 0) classic_hp_change(&g_entities[slot], st.hp);
                if (st.hp <= 0) {
                    /*removal; a creature just killed stays for its death
                     * animation (killed_at_ms, dropped by the snapshot)*/
                    if (slot >= 0 && !g_entities[slot].killed_at_ms) {
                        g_entities[slot].active = false;
                        g_entities[slot].id = 0;
                        g_entities[slot].floor_id = -1;
                    }
                } else {
                    if (slot < 0) slot = free_slot;
                    if (slot >= 0) entity_from_state(&g_entities[slot], &st);
                }
            }
            pthread_mutex_unlock(&g_state_mutex);
            break;
        }
        case MSG_ENTITY_UPDATE: {
            int32_t count = 0;
            if (net_receive_all(g_server_sock, &count, sizeof(count)) <= 0) goto io_error;
            if (count <= 0 || count > 4096 ||
                hdr.length != (int)(sizeof(count) + (size_t)count * sizeof(EntityUpdateRec))) {
                fail(NET_FAILED, "Protocol error (bad entity batch).");
                goto done;
            }
            EntityUpdateRec *recs = malloc((size_t)count * sizeof(EntityUpdateRec));
            if (!recs) goto io_error;
            if (net_receive_all(g_server_sock, recs, (int)((size_t)count * sizeof(EntityUpdateRec))) <= 0) {
                free(recs);
                goto io_error;
            }
            pthread_mutex_lock(&g_state_mutex);
            for (int k = 0; k < count; k++)
                for (int i = 0; i < CLIENT_MAX_ENTITIES; i++)
                    if (g_entities[i].active && g_entities[i].id == recs[k].entity_id) {
                        if (client_classic()) classic_hp_change(&g_entities[i], recs[k].hp);
                        g_entities[i].x = recs[k].x;
                        g_entities[i].y = recs[k].y;
                        g_entities[i].hp = recs[k].hp;
                        if (recs[k].hp > g_entities[i].max_hp) g_entities[i].max_hp = recs[k].hp;
                        if (recs[k].hp <= 0 && !g_entities[i].is_tombstone && !g_entities[i].killed_at_ms)
                            g_entities[i].killed_at_ms = plat_time_ms();
                        break;
                    }
            pthread_mutex_unlock(&g_state_mutex);
            free(recs);
            break;
        }
        case MSG_MAP_CHUNK: {
            MsgMapChunk ch;
            if (net_receive_all(g_server_sock, &ch, sizeof(ch)) <= 0) goto io_error;
            if (ch.width <= 0 || ch.height <= 0 || ch.width > 256 || ch.height > 256 ||
                ch.start_x < 0 || ch.start_y < 0) {
                fail(NET_FAILED, "Protocol error (invalid map chunk).");
                goto done;
            }
            int sz = ch.width * ch.height * (int)sizeof(TileType);
            if (hdr.length != (int)(sizeof(MsgMapChunk) + (size_t)sz)) {
                fail(NET_FAILED, "Protocol error (map chunk length).");
                goto done;
            }
            TileType *buf = malloc((size_t)sz);
            if (!buf) goto io_error;
            if (net_receive_all(g_server_sock, buf, sz) <= 0) {
                free(buf);
                goto io_error;
            }
            pthread_mutex_lock(&g_state_mutex);
            bool changed = false;
            int i = 0;
            for (int cy = ch.start_y; cy < ch.start_y + ch.height; cy++)
                for (int cx = ch.start_x; cx < ch.start_x + ch.width; cx++, i++)
                    if (cx < MAP_WIDTH && cy < MAP_HEIGHT && g_local_map[cy][cx] != buf[i]) {
                        g_local_map[cy][cx] = buf[i];
                        changed = true;
                    }
            if (changed) {
                atomic_fetch_add(&g_map_version, 1);
                g_map_dirty = true;
            }
            pthread_mutex_unlock(&g_state_mutex);
            free(buf);
            break;
        }
        case MSG_SPELL_VFX: {
            ClientEvent e;
            memset(&e, 0, sizeof(e));
            if (net_receive_all(g_server_sock, &e.vfx, sizeof(e.vfx)) <= 0) goto io_error;
            e.kind = CEV_VFX;
            e.x = e.vfx.target_x;
            e.y = e.vfx.target_y;
            pthread_mutex_lock(&g_state_mutex);
            evq_push(&e);
            pthread_mutex_unlock(&g_state_mutex);
            break;
        }
        case MSG_TEXT: {
            MsgText t;
            if (net_receive_all(g_server_sock, &t, sizeof(t)) <= 0) goto io_error;
            t.text[sizeof(t.text) - 1] = '\0';
            client_log_add(t.text);
            pthread_mutex_lock(&g_state_mutex);
            push_simple(CEV_TEXT, (int)(g_log_total - 1), g_my_x, g_my_y);
            /*classic servers announce the hero's death only as text*/
            if (client_classic() && strstr(t.text, "[SYSTEM] You died"))
                push_gev(GEV_PLAYER_DIED, 0, g_my_entity_id, 0, g_my_x, g_my_y);
            pthread_mutex_unlock(&g_state_mutex);
            break;
        }
        case MSG_TIME_SYNC: {
            MsgTimeSync ts;
            if (net_receive_all(g_server_sock, &ts, sizeof(ts)) <= 0) goto io_error;
            pthread_mutex_lock(&g_state_mutex);
            g_game_h = ts.game_hour;
            g_game_m = ts.game_min;
            g_total_turns = ts.total_turns;
            pthread_mutex_unlock(&g_state_mutex);
            break;
        }
        case MSG_TOMBSTONE_REMOVE: {
            MsgTombstoneRemove rm;
            if (net_receive_all(g_server_sock, &rm, sizeof(rm)) <= 0) goto io_error;
            pthread_mutex_lock(&g_state_mutex);
            for (int i = 0; i < CLIENT_MAX_ENTITIES; i++)
                if (g_entities[i].active && g_entities[i].id == rm.entity_id && g_entities[i].is_tombstone) {
                    g_entities[i].active = false;
                    g_entities[i].is_tombstone = false;
                    break;
                }
            pthread_mutex_unlock(&g_state_mutex);
            break;
        }
        case MSG_INVENTORY: {
            if (net_receive_all(g_server_sock, &inv, sizeof(inv)) <= 0) goto io_error;
            if (inv.count < 0) inv.count = 0;
            if (inv.count > INV_MAX_ITEMS) inv.count = INV_MAX_ITEMS;
            for (int i = 0; i < inv.count; i++) inv.items[i].name[sizeof(inv.items[i].name) - 1] = '\0';
            pthread_mutex_lock(&g_state_mutex);
            g_inventory = inv;
            g_inventory_version++;
            push_simple(CEV_INV_CHANGED, inv.count, g_my_x, g_my_y);
            pthread_mutex_unlock(&g_state_mutex);
            break;
        }
        case MSG_SPELLBOOK: {
            if (net_receive_all(g_server_sock, &book, sizeof(book)) <= 0) goto io_error;
            if (book.count < 0) book.count = 0;
            if (book.count > BOOK_MAX_SPELLS) book.count = BOOK_MAX_SPELLS;
            for (int i = 0; i < book.count; i++) book.spells[i].name[sizeof(book.spells[i].name) - 1] = '\0';
            pthread_mutex_lock(&g_state_mutex);
            g_spellbook = book;
            g_spellbook_version++;
            pthread_mutex_unlock(&g_state_mutex);
            break;
        }
        case MSG_SHOP: {
            if (net_receive_all(g_server_sock, &shop, sizeof(shop)) <= 0) goto io_error;
            if (shop.count < 0) shop.count = 0;
            if (shop.count > SHOP_MAX_ITEMS) shop.count = SHOP_MAX_ITEMS;
            shop.shop_name[sizeof(shop.shop_name) - 1] = '\0';
            for (int i = 0; i < shop.count; i++) {
                shop.items[i].name[sizeof(shop.items[i].name) - 1] = '\0';
                shop.items[i].type[sizeof(shop.items[i].type) - 1] = '\0';
                shop.items[i].stats[sizeof(shop.items[i].stats) - 1] = '\0';
            }
            pthread_mutex_lock(&g_state_mutex);
            bool was_open = g_shop.merchant_id != 0;
            g_shop = shop;
            g_shop_version++;
            if (shop.merchant_id != 0 && !was_open) push_simple(CEV_SHOP_OPEN, shop.merchant_id, g_my_x, g_my_y);
            if (shop.merchant_id == 0 && was_open) push_simple(CEV_SHOP_CLOSE, 0, g_my_x, g_my_y);
            pthread_mutex_unlock(&g_state_mutex);
            break;
        }
        case MSG_GAME_EVENT: {
            ClientEvent e;
            memset(&e, 0, sizeof(e));
            if (net_receive_all(g_server_sock, &e.ev, sizeof(e.ev)) <= 0) goto io_error;
            e.kind = CEV_SERVER;
            e.x = e.ev.x;
            e.y = e.ev.y;
            e.value = e.ev.value;
            pthread_mutex_lock(&g_state_mutex);
            if (e.ev.type == GEV_KILL)
                for (int i = 0; i < CLIENT_MAX_ENTITIES; i++)
                    if (g_entities[i].active && g_entities[i].id == e.ev.dst_id && g_entities[i].id != g_my_entity_id) {
                        g_entities[i].hp = 0;
                        if (!g_entities[i].killed_at_ms) g_entities[i].killed_at_ms = plat_time_ms();
                        break;
                    }
            evq_push(&e);
            pthread_mutex_unlock(&g_state_mutex);
            break;
        }
        default:
            break;
        }
        continue;
    io_error:
        fail(NET_DISCONNECTED, "Connection lost.");
        break;
    }
done:
    return NULL;
}
