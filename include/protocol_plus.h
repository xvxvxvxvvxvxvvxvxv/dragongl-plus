/*
 * DragonGL+ - extension of protocol v5
 * Copyright (C) 2026 Nicola Taibi - GPL-3.0-or-later
 *
 * A DragonGL+ server and client agree on the extension in the TLS
 * handshake: the client offers the ALPN protocols "dragongl-plus/5.1"
 * and "dragongl/5", the server prefers the first. Every frame keeps the
 * v5 header (version 5); the extension adds server -> client messages
 * numbered from 256, which an upstream Dragon GL client never receives
 * (it negotiates "dragongl/5"). The payloads are written field by field,
 * little-endian, like v5 (src/shared/wire_plus.c, PROTOCOL_PLUS.md).
 */
#ifndef DRAGONGL_PROTOCOL_PLUS_H
#define DRAGONGL_PROTOCOL_PLUS_H

#include <stdint.h>
#include "protocol.h"

/*ALPN identifier of the extension (base protocol 5, extension 1).*/
#define TLS_ALPN_PLUS_NAME "dragongl-plus/5.1"

/*Messages of the extension (server -> client)*/
#define MSG_PLUS_ENTITY_INFO 256 /*identity of a creature or player*/
#define MSG_PLUS_PLAYER_EXT  257 /*own character: progress not in MSG_STATE*/
#define MSG_PLUS_INVENTORY   258 /*backpack, belt and equipment*/
#define MSG_PLUS_SHOP        259 /*the merchant's stock (shop window)*/
#define MSG_PLUS_SPELLBOOK   260 /*known spells*/
#define MSG_PLUS_GAME_EVENT  261 /*attack, kill, level up... (animations, sounds)*/

/*Visual families of creatures (Entity.kind): the client maps each family
 * to a procedural animated model.*/
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
    KIND_CHEST,       /*treasure chests*/
    KIND_GOLD,        /*gold piles*/
    KIND_MERCHANT,
    KIND_PLAYER,
    KIND_TOMBSTONE,
    KIND_COUNT
};

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
} InvItem;

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
} ShopItem;

typedef struct {
    int32_t  merchant_id;   /*entity id of the merchant; 0 = no shop open*/
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
} SpellEntry;

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


/*MSG_PLUS_ENTITY_INFO: sent right after the MSG_STATE that introduces an
 * entity to the client.*/
typedef struct {
    int32_t  entity_id;
    uint8_t  kind;       /*KIND_**/
    uint8_t  archetype;  /*EntityArchetype of the server*/
    uint8_t  level;      /*challenge level of a creature, level of a player*/
    int8_t   race_id;    /*players: RaceType, else -1*/
    int8_t   class_id;   /*players: ClassType, else -1*/
    uint32_t look_seed;  /*stable per-creature seed for colour variation*/
    char     name[32];   /*display name*/
} PlusEntityInfo;

/*MSG_PLUS_PLAYER_EXT: sent with every full state of the own character.*/
typedef struct {
    int32_t xp_next;      /*XP threshold of the next level*/
    int32_t stat_points;  /*unspent ability points ('train')*/
    int32_t max_floor;    /*deepest floor explored*/
} PlusPlayerExt;

#endif
