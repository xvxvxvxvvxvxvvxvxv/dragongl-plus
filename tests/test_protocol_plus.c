/*
 * DragonGL+ - codec of the protocol extension (wire_plus.c): every
 * message survives a round trip, a truncated or oversized payload is
 * refused, and a frame keeps the v5 header.
 * Copyright (C) 2026 Nicola Taibi - GPL-3.0-or-later
 */
#include "wire.h"
#include "wire_plus.h"

#include <stdio.h>
#include <string.h>

static int g_failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                        \
            g_failures++;                                                                          \
        }                                                                                          \
    } while (0)

/* The payload of the single frame in w (header checked). */
static const uint8_t *payload_of(const BinWriter *w, MsgType type, size_t *len) {
    MsgHeader h;
    wire_read_header(w->data, &h);
    CHECK(h.version == PROTOCOL_VERSION);
    CHECK(h.type == type);
    CHECK(h.length + WIRE_HEADER_SIZE == w->len);
    CHECK(h.length <= wire_plus_server_payload_max(type));
    *len = h.length;
    return w->data + WIRE_HEADER_SIZE;
}

static void test_entity_info(void) {
    PlusEntityInfo in, out;
    memset(&in, 0, sizeof(in));
    in.entity_id = 4242;
    in.kind = KIND_DRAGON;
    in.archetype = 5;
    in.level = 17;
    in.race_id = -1;
    in.class_id = 11;
    in.look_seed = 0xDEADBEEFu;
    strcpy(in.name, "Ancient Red Dragon");
    BinWriter w;
    bw_init(&w);
    CHECK(wire_put_plus_entity_info(&w, &in));
    size_t n;
    const uint8_t *p = payload_of(&w, MSG_PLUS_ENTITY_INFO, &n);
    CHECK(wire_get_plus_entity_info(p, n, &out));
    CHECK(memcmp(&in, &out, sizeof(in)) == 0);
    CHECK(!wire_get_plus_entity_info(p, n - 1, &out)); /* truncated */
    bw_free(&w);
}

static void test_inventory(void) {
    static MsgInventory in, out;
    memset(&in, 0, sizeof(in));
    in.count = INV_MAX_ITEMS; /* the largest payload */
    in.weight_now = 12.5f;
    in.weight_max = 50.0f;
    for (int i = 0; i < in.count; i++) {
        InvItem *it = &in.items[i];
        snprintf(it->name, sizeof(it->name), "Item number %d of the full backpack", i);
        it->category = (uint8_t)(i % 16);
        it->where = (uint8_t)(i % 3);
        it->slot = (uint8_t)i;
        it->flags = INVF_IDENTIFIED | INVF_MAGIC;
        it->qty = (uint16_t)(i + 1);
        it->durability = (int16_t)-3;
        it->max_durability = 100;
        it->dmg_count = 2;
        it->dmg_sides = 6;
        it->ac = 14;
        it->heal = 8;
        it->light = 3;
        it->value = 123456u;
        it->weight = 1.25f;
    }
    BinWriter w;
    bw_init(&w);
    CHECK(wire_put_plus_inventory(&w, &in));
    size_t n;
    const uint8_t *p = payload_of(&w, MSG_PLUS_INVENTORY, &n);
    CHECK(wire_get_plus_inventory(p, n, &out));
    CHECK(memcmp(&in, &out, sizeof(in)) == 0);
    /* a count above the limit is a protocol error */
    uint8_t bad[sizeof(uint8_t) + 8];
    memset(bad, 0, sizeof(bad));
    bad[0] = INV_MAX_ITEMS + 1;
    CHECK(!wire_get_plus_inventory(bad, sizeof(bad), &out));
    bw_free(&w);
}

static void test_shop_and_book(void) {
    static MsgShop shop, shop2;
    memset(&shop, 0, sizeof(shop));
    shop.merchant_id = 77;
    strcpy(shop.shop_name, "The Iron Anvil");
    shop.count = 2;
    strcpy(shop.items[0].name, "Longsword");
    strcpy(shop.items[0].type, "Weapon");
    strcpy(shop.items[0].stats, "1d8 dmg");
    shop.items[0].cost = 1500;
    shop.items[0].stock = 3;
    shop.items[0].category = 1;
    strcpy(shop.items[1].name, "Potion of Healing");
    strcpy(shop.items[1].type, "Consum.");
    strcpy(shop.items[1].stats, "Heal 20 HP");
    shop.items[1].cost = 50;
    shop.items[1].stock = -1;
    BinWriter w;
    bw_init(&w);
    CHECK(wire_put_plus_shop(&w, &shop));
    size_t n;
    const uint8_t *p = payload_of(&w, MSG_PLUS_SHOP, &n);
    CHECK(wire_get_plus_shop(p, n, &shop2));
    CHECK(memcmp(&shop, &shop2, sizeof(shop)) == 0);

    static MsgSpellbook book, book2;
    memset(&book, 0, sizeof(book));
    book.count = BOOK_MAX_SPELLS;
    for (int i = 0; i < book.count; i++) {
        snprintf(book.spells[i].name, sizeof(book.spells[i].name), "Spell %d", i);
        book.spells[i].level = (int8_t)(i % 10);
        book.spells[i].target_type = 2;
        book.spells[i].effect_type = 1;
        book.spells[i].damage_type = 4;
        book.spells[i].range = 12;
        book.spells[i].radius = -1;
    }
    bw_reset(&w);
    CHECK(wire_put_plus_spellbook(&w, &book));
    p = payload_of(&w, MSG_PLUS_SPELLBOOK, &n);
    CHECK(wire_get_plus_spellbook(p, n, &book2));
    CHECK(memcmp(&book, &book2, sizeof(book)) == 0);
    bw_free(&w);
}

static void test_event_and_ext(void) {
    MsgGameEvent ev = {GEV_ATTACK_CRIT, 12, 34, 56, -1, 299}, ev2;
    BinWriter w;
    bw_init(&w);
    CHECK(wire_put_plus_game_event(&w, &ev));
    size_t n;
    const uint8_t *p = payload_of(&w, MSG_PLUS_GAME_EVENT, &n);
    CHECK(wire_get_plus_game_event(p, n, &ev2));
    CHECK(memcmp(&ev, &ev2, sizeof(ev)) == 0);
    /* an unknown event type is refused */
    uint8_t bad[17];
    memcpy(bad, p, sizeof(bad));
    bad[0] = 0;
    CHECK(!wire_get_plus_game_event(bad, sizeof(bad), &ev2));

    PlusPlayerExt x = {14000, 3, 27}, x2;
    bw_reset(&w);
    CHECK(wire_put_plus_player_ext(&w, &x));
    p = payload_of(&w, MSG_PLUS_PLAYER_EXT, &n);
    CHECK(wire_get_plus_player_ext(p, n, &x2));
    CHECK(memcmp(&x, &x2, sizeof(x)) == 0);
    bw_free(&w);
}

int main(void) {
    /* the extension never collides with the messages of v5 */
    CHECK(wire_server_payload_max(MSG_PLUS_ENTITY_INFO) == 0);
    CHECK(wire_plus_server_payload_max(MSG_STATE) == 0);
    test_entity_info();
    test_inventory();
    test_shop_and_book();
    test_event_and_ext();
    if (g_failures) {
        fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    printf("protocol_plus: all checks passed\n");
    return 0;
}
