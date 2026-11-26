/*
 * DragonGL+ - codec of the protocol extension (wire_plus.h)
 * Copyright (C) 2026 Nicola Taibi - GPL-3.0-or-later
 */

#include "wire_plus.h"

#include <string.h>

/* --- Framing (the v5 header: version, type, payload length) ----------- */

static void put_le32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

static size_t frame_begin(BinWriter *w, MsgType type) {
    size_t at = w->len;
    bw_u16(w, PROTOCOL_VERSION);
    bw_u16(w, type);
    bw_u32(w, 0);
    return at;
}

static bool frame_end(BinWriter *w, size_t at, MsgType type) {
    if (w->failed) {
        return false;
    }
    size_t payload = w->len - at - WIRE_HEADER_SIZE;
    if (payload > wire_plus_server_payload_max(type)) {
        w->len = at; /* drop the frame, keep what was there before */
        return false;
    }
    put_le32(w->data + at + 4, (uint32_t)payload);
    return true;
}

static void put_str(BinWriter *w, const char *s, size_t cap) {
    size_t n = s ? strnlen(s, cap - 1) : 0;
    if (n > 255) {
        n = 255;
    }
    bw_u8(w, (uint8_t)n);
    bw_bytes(w, s, n);
}

static void get_str(BinReader *r, char *out, size_t cap) {
    size_t n = br_u8(r);
    if (n >= cap) {
        r->failed = true;
        out[0] = '\0';
        return;
    }
    br_bytes(r, out, n);
    out[n] = '\0';
    if (r->failed) {
        out[0] = '\0';
    }
}

static void put_f32(BinWriter *w, float f) {
    uint32_t u;
    memcpy(&u, &f, sizeof(u));
    bw_u32(w, u);
}

static float get_f32(BinReader *r) {
    uint32_t u = br_u32(r);
    float f;
    memcpy(&f, &u, sizeof(f));
    return f;
}

/* A one-byte count that must lie in [0, max]; otherwise the reader fails. */
static int32_t get_count_u8(BinReader *r, int max) {
    int n = br_u8(r);
    if (n > max) {
        r->failed = true;
        return 0;
    }
    return n;
}

static void begin_read(BinReader *r, const void *p, size_t n, void *m, size_t m_size) {
    memset(m, 0, m_size);
    br_init(r, p, n);
}

/* Payload limits: every string at its field size, every list full. */
#define STR(n) (1u + (uint32_t)(n) - 1u)
#define INV_ITEM_BYTES (STR(48) + 4u + 2u + 2u * 7u + 4u + 4u)
#define SHOP_ITEM_BYTES (STR(40) + STR(12) + STR(24) + 4u + 2u + 1u)
#define SPELL_BYTES (STR(40) + 4u + 2u + 2u)

uint32_t wire_plus_server_payload_max(MsgType type) {
    switch (type) {
    case MSG_PLUS_ENTITY_INFO:
        return 4u + 5u + 4u + STR(32);
    case MSG_PLUS_PLAYER_EXT:
        return 12u;
    case MSG_PLUS_INVENTORY:
        return 1u + 8u + INV_MAX_ITEMS * INV_ITEM_BYTES;
    case MSG_PLUS_SHOP:
        return 4u + STR(64) + 1u + SHOP_MAX_ITEMS * SHOP_ITEM_BYTES;
    case MSG_PLUS_SPELLBOOK:
        return 2u + BOOK_MAX_SPELLS * SPELL_BYTES;
    case MSG_PLUS_GAME_EVENT:
        return 1u + 12u + 4u;
    default:
        return 0;
    }
}

/* --- MSG_PLUS_ENTITY_INFO ----------------------------------------------- */

bool wire_put_plus_entity_info(BinWriter *w, const PlusEntityInfo *m) {
    size_t at = frame_begin(w, MSG_PLUS_ENTITY_INFO);
    bw_i32(w, m->entity_id);
    bw_u8(w, m->kind);
    bw_u8(w, m->archetype);
    bw_u8(w, m->level);
    bw_u8(w, (uint8_t)m->race_id);
    bw_u8(w, (uint8_t)m->class_id);
    bw_u32(w, m->look_seed);
    put_str(w, m->name, sizeof(m->name));
    return frame_end(w, at, MSG_PLUS_ENTITY_INFO);
}

bool wire_get_plus_entity_info(const void *p, size_t n, PlusEntityInfo *m) {
    BinReader r;
    begin_read(&r, p, n, m, sizeof(*m));
    m->entity_id = br_i32(&r);
    m->kind = br_u8(&r);
    m->archetype = br_u8(&r);
    m->level = br_u8(&r);
    m->race_id = (int8_t)br_u8(&r);
    m->class_id = (int8_t)br_u8(&r);
    m->look_seed = br_u32(&r);
    get_str(&r, m->name, sizeof(m->name));
    if (m->kind >= KIND_COUNT) {
        r.failed = true;
    }
    return br_done(&r);
}

/* --- MSG_PLUS_PLAYER_EXT ------------------------------------------------ */

bool wire_put_plus_player_ext(BinWriter *w, const PlusPlayerExt *m) {
    size_t at = frame_begin(w, MSG_PLUS_PLAYER_EXT);
    bw_i32(w, m->xp_next);
    bw_i32(w, m->stat_points);
    bw_i32(w, m->max_floor);
    return frame_end(w, at, MSG_PLUS_PLAYER_EXT);
}

bool wire_get_plus_player_ext(const void *p, size_t n, PlusPlayerExt *m) {
    BinReader r;
    begin_read(&r, p, n, m, sizeof(*m));
    m->xp_next = br_i32(&r);
    m->stat_points = br_i32(&r);
    m->max_floor = br_i32(&r);
    return br_done(&r);
}

/* --- MSG_PLUS_INVENTORY ------------------------------------------------- */

bool wire_put_plus_inventory(BinWriter *w, const MsgInventory *m) {
    size_t at = frame_begin(w, MSG_PLUS_INVENTORY);
    int count = m->count < 0 ? 0 : (m->count > INV_MAX_ITEMS ? INV_MAX_ITEMS : m->count);
    bw_u8(w, (uint8_t)count);
    put_f32(w, m->weight_now);
    put_f32(w, m->weight_max);
    for (int i = 0; i < count; i++) {
        const InvItem *it = &m->items[i];
        put_str(w, it->name, sizeof(it->name));
        bw_u8(w, it->category);
        bw_u8(w, it->where);
        bw_u8(w, it->slot);
        bw_u8(w, it->flags);
        bw_u16(w, it->qty);
        bw_u16(w, (uint16_t)it->durability);
        bw_u16(w, (uint16_t)it->max_durability);
        bw_u16(w, (uint16_t)it->dmg_count);
        bw_u16(w, (uint16_t)it->dmg_sides);
        bw_u16(w, (uint16_t)it->ac);
        bw_u16(w, (uint16_t)it->heal);
        bw_u16(w, (uint16_t)it->light);
        bw_u32(w, it->value);
        put_f32(w, it->weight);
    }
    return frame_end(w, at, MSG_PLUS_INVENTORY);
}

bool wire_get_plus_inventory(const void *p, size_t n, MsgInventory *m) {
    BinReader r;
    begin_read(&r, p, n, m, sizeof(*m));
    m->count = get_count_u8(&r, INV_MAX_ITEMS);
    m->weight_now = get_f32(&r);
    m->weight_max = get_f32(&r);
    for (int i = 0; i < m->count && !r.failed; i++) {
        InvItem *it = &m->items[i];
        get_str(&r, it->name, sizeof(it->name));
        it->category = br_u8(&r);
        it->where = br_u8(&r);
        it->slot = br_u8(&r);
        it->flags = br_u8(&r);
        it->qty = br_u16(&r);
        it->durability = (int16_t)br_u16(&r);
        it->max_durability = (int16_t)br_u16(&r);
        it->dmg_count = (int16_t)br_u16(&r);
        it->dmg_sides = (int16_t)br_u16(&r);
        it->ac = (int16_t)br_u16(&r);
        it->heal = (int16_t)br_u16(&r);
        it->light = (int16_t)br_u16(&r);
        it->value = br_u32(&r);
        it->weight = get_f32(&r);
        if (it->where > INV_EQUIPPED) {
            r.failed = true;
        }
    }
    return br_done(&r);
}

/* --- MSG_PLUS_SHOP ------------------------------------------------------ */

bool wire_put_plus_shop(BinWriter *w, const MsgShop *m) {
    size_t at = frame_begin(w, MSG_PLUS_SHOP);
    int count = m->count < 0 ? 0 : (m->count > SHOP_MAX_ITEMS ? SHOP_MAX_ITEMS : m->count);
    bw_i32(w, m->merchant_id);
    put_str(w, m->shop_name, sizeof(m->shop_name));
    bw_u8(w, (uint8_t)count);
    for (int i = 0; i < count; i++) {
        const ShopItem *it = &m->items[i];
        put_str(w, it->name, sizeof(it->name));
        put_str(w, it->type, sizeof(it->type));
        put_str(w, it->stats, sizeof(it->stats));
        bw_u32(w, it->cost);
        bw_u16(w, (uint16_t)it->stock);
        bw_u8(w, it->category);
    }
    return frame_end(w, at, MSG_PLUS_SHOP);
}

bool wire_get_plus_shop(const void *p, size_t n, MsgShop *m) {
    BinReader r;
    begin_read(&r, p, n, m, sizeof(*m));
    m->merchant_id = br_i32(&r);
    get_str(&r, m->shop_name, sizeof(m->shop_name));
    m->count = get_count_u8(&r, SHOP_MAX_ITEMS);
    for (int i = 0; i < m->count && !r.failed; i++) {
        ShopItem *it = &m->items[i];
        get_str(&r, it->name, sizeof(it->name));
        get_str(&r, it->type, sizeof(it->type));
        get_str(&r, it->stats, sizeof(it->stats));
        it->cost = br_u32(&r);
        it->stock = (int16_t)br_u16(&r);
        it->category = br_u8(&r);
    }
    return br_done(&r);
}

/* --- MSG_PLUS_SPELLBOOK ------------------------------------------------- */

bool wire_put_plus_spellbook(BinWriter *w, const MsgSpellbook *m) {
    size_t at = frame_begin(w, MSG_PLUS_SPELLBOOK);
    int count = m->count < 0 ? 0 : (m->count > BOOK_MAX_SPELLS ? BOOK_MAX_SPELLS : m->count);
    bw_u16(w, (uint16_t)count);
    for (int i = 0; i < count; i++) {
        const SpellEntry *s = &m->spells[i];
        put_str(w, s->name, sizeof(s->name));
        bw_u8(w, (uint8_t)s->level);
        bw_u8(w, s->target_type);
        bw_u8(w, s->effect_type);
        bw_u8(w, s->damage_type);
        bw_u16(w, (uint16_t)s->range);
        bw_u16(w, (uint16_t)s->radius);
    }
    return frame_end(w, at, MSG_PLUS_SPELLBOOK);
}

bool wire_get_plus_spellbook(const void *p, size_t n, MsgSpellbook *m) {
    BinReader r;
    begin_read(&r, p, n, m, sizeof(*m));
    uint16_t count = br_u16(&r);
    if (count > BOOK_MAX_SPELLS) {
        r.failed = true;
        count = 0;
    }
    m->count = count;
    for (int i = 0; i < m->count && !r.failed; i++) {
        SpellEntry *s = &m->spells[i];
        get_str(&r, s->name, sizeof(s->name));
        s->level = (int8_t)br_u8(&r);
        s->target_type = br_u8(&r);
        s->effect_type = br_u8(&r);
        s->damage_type = br_u8(&r);
        s->range = (int16_t)br_u16(&r);
        s->radius = (int16_t)br_u16(&r);
    }
    return br_done(&r);
}

/* --- MSG_PLUS_GAME_EVENT ------------------------------------------------ */

bool wire_put_plus_game_event(BinWriter *w, const MsgGameEvent *m) {
    size_t at = frame_begin(w, MSG_PLUS_GAME_EVENT);
    bw_u8(w, (uint8_t)m->type);
    bw_i32(w, m->src_id);
    bw_i32(w, m->dst_id);
    bw_i32(w, m->value);
    bw_u16(w, (uint16_t)m->x);
    bw_u16(w, (uint16_t)m->y);
    return frame_end(w, at, MSG_PLUS_GAME_EVENT);
}

bool wire_get_plus_game_event(const void *p, size_t n, MsgGameEvent *m) {
    BinReader r;
    begin_read(&r, p, n, m, sizeof(*m));
    m->type = br_u8(&r);
    m->src_id = br_i32(&r);
    m->dst_id = br_i32(&r);
    m->value = br_i32(&r);
    m->x = (int16_t)br_u16(&r);
    m->y = (int16_t)br_u16(&r);
    if (m->type < GEV_ATTACK_HIT || m->type > GEV_BOSS_DEFEATED) {
        r.failed = true;
    }
    return br_done(&r);
}
