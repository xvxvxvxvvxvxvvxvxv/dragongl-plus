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

#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include "map.h"

typedef uint32_t MsgType;
#define MSG_LOGIN      1
#define MSG_WELCOME    2
#define MSG_MOVE       3
#define MSG_STATE      4
#define MSG_MAP_CHUNK  5
#define MSG_TEXT_CMD   6
#define MSG_TEXT       7
#define MSG_AUTH_FAIL  8
#define MSG_SPELL_VFX  9
#define MSG_TOMBSTONE_REMOVE 10
#define MSG_TIME_SYNC 11
#define MSG_ENTITY_UPDATE 12
/*v4 (DragonGL+): structured data for the graphical client. The
 * text log is still sent for every action (it remains the authoritative
 * narration), these messages only let the UI draw windows and play
 * animations/sounds without scraping text.*/
#define MSG_INVENTORY  13  /*server -> client: backpack/belt/equipment*/
#define MSG_SHOP       14  /*server -> client: merchant stock (shop window)*/
#define MSG_SPELLBOOK  15  /*server -> client: known spells*/
#define MSG_GAME_EVENT 16  /*server -> client: attack/kill/level-up/...*/

/*Bumped whenever the wire format changes. Both ends REJECT any other
 * value, so an old client and a new server fail fast with a clear
 * protocol error instead of silently desynchronizing the stream.
 * v2: EntityUpdateRec.hp widened to int32_t (boss HP on deep floors
 * exceeded the old int16_t and showed up as random HP on clients).
 * v3: MsgMapChunk start/width/height narrowed from int32_t to int16_t
 * (a 300x300 grid fits in 16 bits with room to spare: 8-byte chunk
 * header instead of 16).
 * v4: MsgState carries the creature kind/archetype/level of NPCs (the
 * client draws distinct animated models, name plates and HP bars) plus
 * xp_next/stat_points for the player; new MSG_INVENTORY, MSG_SHOP,
 * MSG_SPELLBOOK and MSG_GAME_EVENT messages.*/
#define PROTOCOL_VERSION 4

typedef struct {
    MsgType type;
    int length;
    uint16_t version;  /*PROTOCOL_VERSION on every header*/
    uint16_t reserved; /*must be 0*/
} MsgHeader;

/*Builds a correctly-versioned header. Every send site MUST use this
 * (a hand-written {type, len} initializer would leave version = 0 and
 * the peer would reject the message).*/
static inline MsgHeader msg_hdr(MsgType type, int length) {
    MsgHeader h;
    h.type     = type;
    h.length   = length;
    h.version  = PROTOCOL_VERSION;
    h.reserved = 0;
    return h;
}

/*One compact entity record inside a MsgEntityUpdate payload.
 * Sent only for entities the client has ALREADY received a full
 * MsgState for (the server tracks that per client), so the client
 * can update position/HP without the ~1.5 KB full state message.
 * hp is int32_t (NOT int16_t): boss HP is hp_base*5 + floor*50, which
 * passes 32767 on deep floors with big JSON templates — the old
 * int16_t wrapped and clients drew random HP bars. The layout stays
 * 12 bytes (no padding: 4 + 2 + 2 + 4, hp 4-aligned at offset 8).*/
typedef struct {
    int   entity_id;
    int16_t x;
    int16_t y;
    int32_t hp;
} EntityUpdateRec; /*12 bytes: the payload is [int32 count][EntityUpdateRec count]*/

/*Sent by the client at login.
   If is_new_char == 1: The client wants to create a new character.
   race_id and class_id are used for creation only.*/
typedef struct {
    char username[32];
    char password[32];
    char server_pass[32];
    int  race_id;       /* RaceType  */
    int  subrace_id;    /* SubraceType */
    int  class_id;      /* ClassType */
    int  is_new_char;   /*1 = request for new character*/
    int  str, dex, con, intel, wis, cha;
    int  age, height, weight, social_class, alignment;
} MsgLogin;

/*Sent by the server after successful authentication.*/
typedef struct {
    int entity_id;
    int x;
    int y;
    int success;    /* 1 = OK, 0 = fallito */
    int is_new;     /*1 = new character created*/
    int hp;
    int max_hp;
    uint64_t gold;
    int race_id;
    int subrace_id;
    int class_id;
    int level;
    int alignment;
} MsgWelcome;

typedef struct {
    int entity_id;
    int dx;
    int dy;
    int dz;
} MsgMove;

typedef struct {
    uint64_t gold;
    int entity_id;
    int x, y, hp, max_hp;
    int floor_id;
    int game_hour, game_min, total_turns;
    int str, dex, con, intel, wis, cha;
    float movement_cooldown;
    int equipped_mask; 
    int level;
    int xp;
    int vision_radius;
    int ac;
    uint32_t status_icons; //Bitmask for status icons
    char weapon_name[32];
    char armor_name[32];
    int to_hit;
    int to_dmg;
    uint32_t bosses_defeated; // bitmask for 10 bosses
    int hunger_level;
    int spell_slots[10];
    int spell_slots_max[10];
    char eq_head[32];
    char eq_neck[32];
    char eq_body[32];
    char eq_back[32];
    char eq_hand_r[32];
    char eq_hand_l[32];
    char eq_hands[32];
    char eq_arm_r[32];
    char eq_arm_l[32];
    char eq_feet[32];
    char eq_ring[10][32];
    char eq_belt[4][32];
    int  is_merchant;   /*1 = entity is a merchant*/
    int  shop_spec;    /*MerchantSpecialization of the shop (SHOP_SPEC_NONE if not a merchant)*/
    int  is_tombstone;  /*1 = entity is a tombstone*/
    int  is_player;
    char username[32];   /*player name, or the NPC's display name (v4)*/
    /*--- v4 ---*/
    int  kind;           /*CreatureKind (below): model family for NPCs*/
    int  archetype;      /*server EntityArchetype (ARCH_*)*/
    int  npc_level;      /*challenge level shown on the name plate*/
    int  xp_next;        /*player: XP threshold of the next level*/
    int  stat_points;    /*player: unspent stat points ('train')*/
    int  race_id;        /*players (own and others): RaceType*/
    int  class_id;       /*players (own and others): ClassType*/
    uint32_t look_seed;  /*stable per-creature seed for colour variation*/
    int  max_floor;      /*player: deepest floor explored*/
    int  reserved_v4[3];
} MsgState;

/*Visual families of creatures (MsgState.kind). Derived on the server
 * from the bestiary "Type:" tag and name keywords (data_loader.c); the
 * client maps each family to a procedural animated model.*/
enum {
    KIND_HUMANOID = 0,
    KIND_BEAST,       /*quadruped animals*/
    KIND_INSECT,      /*spiders, insects, vermin*/
    KIND_UNDEAD,
    KIND_FIEND,
    KIND_ELEMENTAL,
    KIND_CONSTRUCT,
    KIND_ABERRATION,
    KIND_DRAGON,
    KIND_PLANT,
    KIND_FEY,
    KIND_CELESTIAL,
    KIND_GIANT,
    KIND_OOZE,
    KIND_SERPENT,
    KIND_FLYER,       /*birds, bats, winged things*/
    KIND_SPIRIT,
    KIND_CHEST,       /*ARCH_TREASURE*/
    KIND_GOLD,        /*ARCH_GOLD*/
    KIND_MERCHANT,
    KIND_PLAYER,
    KIND_TOMBSTONE,
    KIND_COUNT
};

/* Values for MsgState.shop_spec - they must match the
 * MerchantSpecialization enum in src/server/server_entities.h.
 * The client only needs to recognize the martial bookshop to
 * draw its unique sprite.*/
#define SHOP_SPEC_NONE          (-1)
#define SHOP_SPEC_BOOKS_MARTIAL 11

/*Sent by the server when a tombstone is removed (item recovery)*/
typedef struct {
    int entity_id; /*ID of the tombstone to remove*/
} MsgTombstoneRemove;

/*Map chunk descriptor. int16_t is enough (L8): coordinates and
 * dimensions are bounded by MAP_WIDTH/MAP_HEIGHT (300), so 16 bits
 * carry them with margin and the header shrinks from 16 to 8 bytes
 * per chunk. (Changed layout => PROTOCOL_VERSION bumped to 3.)*/
typedef struct {
    int16_t start_x;
    int16_t start_y;
    int16_t width;
    int16_t height;
} MsgMapChunk;

typedef struct {
    char cmd[256];
} MsgTextCmd;

typedef struct {
    char text[256];
} MsgText;

/*Sent by the server when authentication fails.
   The reason field explains the reason.*/
typedef struct {
    char reason[128];
} MsgAuthFail;


typedef struct {
    int start_x;
    int start_y;
    int target_x;
    int target_y;
    int vfx_type;
    float color_r;
    float color_g;
    float color_b;
} MsgSpellVFX;

typedef struct {
    int game_hour;
    int game_min;
    int total_turns;
} MsgTimeSync;

/*--- v4 structured messages ------------------------------------------*/

/*Where an inventory entry lives (InvItem.where)*/
#define INV_BACKPACK 0
#define INV_BELT     1
#define INV_EQUIPPED 2

/*InvItem.flags*/
#define INVF_IDENTIFIED 0x01
#define INVF_CURSED     0x02
#define INVF_ARTIFACT   0x04
#define INVF_MAGIC      0x08

/*Equipment slot ids (InvItem.slot when where == INV_EQUIPPED); also the
 * argument the client passes to 'takeoff'.*/
enum {
    EQ_HEAD = 0, EQ_NECK, EQ_BODY, EQ_BACK, EQ_HAND_R, EQ_HAND_L,
    EQ_HANDS, EQ_ARM_R, EQ_ARM_L, EQ_FEET, EQ_RING0 /*.. EQ_RING0+9*/
};
#define EQ_SLOT_COUNT (EQ_RING0 + 10)

typedef struct {
    char     name[48];
    uint8_t  category;   /*ItemCategory (items.h)*/
    uint8_t  where;      /*INV_*/
    uint8_t  slot;       /*backpack index (0-based), belt index or EQ_**/
    uint8_t  flags;      /*INVF_**/
    uint16_t qty;
    int16_t  durability;
    int16_t  max_durability;
    int16_t  dmg_count, dmg_sides; /*weapons*/
    int16_t  ac;         /*armor base / shield bonus*/
    int16_t  heal;       /*consumables*/
    int16_t  light;      /*light sources: radius*/
    uint32_t value;      /*base cost in gp*/
    float    weight;
} InvItem; /*76 bytes*/

#define INV_MAX_ITEMS 64
typedef struct {
    int32_t count;
    float   weight_now;
    float   weight_max;
    InvItem items[INV_MAX_ITEMS];
} MsgInventory;

#define SHOP_MAX_ITEMS 50
typedef struct {
    char     name[40];
    char     type[12];
    char     stats[24];
    uint32_t cost;       /*current dynamic price*/
    int16_t  stock;
    uint8_t  category;
    uint8_t  pad;
} ShopItem; /*84 bytes*/

typedef struct {
    int32_t  merchant_id;   /*entity id of the merchant; 0 = close shop*/
    char     shop_name[64];
    int32_t  count;
    ShopItem items[SHOP_MAX_ITEMS];
} MsgShop;

#define BOOK_MAX_SPELLS 160
typedef struct {
    char    name[40];
    int8_t  level;        /*0 = cantrip*/
    uint8_t target_type;  /*SpellTargetType*/
    uint8_t effect_type;  /*SpellEffectType*/
    uint8_t damage_type;
    int16_t range;
    int16_t radius;
} SpellEntry; /*48 bytes*/

typedef struct {
    int32_t    count;
    SpellEntry spells[BOOK_MAX_SPELLS];
} MsgSpellbook;

/*Game events (MsgGameEvent.type)*/
enum {
    GEV_ATTACK_HIT = 1,  /*src attacked dst and hit (value = damage)*/
    GEV_ATTACK_CRIT,     /*critical hit (value = damage)*/
    GEV_ATTACK_MISS,     /*src attacked dst and missed*/
    GEV_KILL,            /*dst died (src = killer, may be 0)*/
    GEV_HURT,            /*dst took non-melee damage (spell/trap/aoe)*/
    GEV_LEVEL_UP,        /*value = new level*/
    GEV_GOLD,            /*value = gold gained*/
    GEV_PICKUP,          /*picked up an item*/
    GEV_PLAYER_DIED,     /*the receiving player died and respawned*/
    GEV_TRAP,            /*a trap fired at (x,y)*/
    GEV_TRADE,           /*buy/sell completed*/
    GEV_HEAL,            /*dst healed (value = amount)*/
    GEV_CAST,            /*src cast a spell*/
    GEV_DOOR,            /*a door opened/closed at (x,y)*/
    GEV_DIG,             /*tunnel/dig at (x,y)*/
    GEV_STAIRS,          /*src took the stairs*/
    GEV_BOSS_DEFEATED    /*value = floor*/
};

typedef struct {
    int32_t type;
    int32_t src_id;
    int32_t dst_id;
    int32_t value;
    int16_t x, y;
} MsgGameEvent;

#endif // PROTOCOL_H

