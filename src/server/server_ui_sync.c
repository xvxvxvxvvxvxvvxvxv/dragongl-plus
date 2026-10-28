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

/*Structured UI data for the graphical client (protocol v4).
 *
 * The text log remains the authoritative narration of every action;
 * this module only mirrors the state the client needs to draw its
 * windows (inventory, shop, spellbook), the visual identity of NPCs
 * (creature family for the 3D model, name plate, level) and short game
 * events that drive animations and sound effects.
 *
 * Everything is CHANGE-DRIVEN: the inventory/spellbook/shop payloads
 * are hashed per client and re-sent only when the hash changes, so
 * calling ui_sync_client() after every command costs a hash, not a
 * packet.*/

#include "server_ui_sync.h"
#include "server_internal.h"
#include "server_net.h"
#include "server_combat.h"
#include "bestiary.h"
#include "spells.h"
#include "protocol.h"
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stddef.h>


/*Per-client change detection (indexed by the g_clients slot).*/
static uint32_t s_inv_hash[MAX_CLIENTS];
static uint32_t s_book_hash[MAX_CLIENTS];
static uint32_t s_shop_hash[MAX_CLIENTS];
static int s_shop_merchant[MAX_CLIENTS]; /*entity id of the open shop, 0 = none*/

static uint32_t fnv1a(const void *data, size_t len, uint32_t h) {
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static int client_index(const Client *c) {
    if (!c || !g_clients) return -1;
    int i = (int)(c - g_clients);
    return (i >= 0 && i < MAX_CLIENTS) ? i : -1;
}

void ui_sync_reset(int ci) {
    if (ci < 0 || ci >= MAX_CLIENTS) return;
    /*Never-matching values: the first sync after login always sends.*/
    s_inv_hash[ci] = 0xFFFFFFFFu;
    s_book_hash[ci] = 0xFFFFFFFFu;
    s_shop_hash[ci] = 0xFFFFFFFFu;
    s_shop_merchant[ci] = 0;
}

/* ------------------------------------------------------------------ */
/* Creature families                                                   */
/* ------------------------------------------------------------------ */

static bool has_word(const char *s, const char *w) {
    return s && strcasestr(s, w) != NULL;
}

/*Name keywords win over the bestiary "Type:" tag: a "Giant Spider" is
 * tagged Beast but must look like a spider, a "Bone Dragon" is Undead
 * but should still be a dragon.*/
static int kind_from_template(const MonsterTemplate *t) {
    if (!t || !t->name) return KIND_HUMANOID;
    const char *n = t->name;
    static const struct { const char *kw; int kind; } name_rules[] = {
        {"spider", KIND_INSECT}, {"insect", KIND_INSECT}, {"beetle", KIND_INSECT},
        {"scorpion", KIND_INSECT}, {"centipede", KIND_INSECT}, {"ant", KIND_INSECT},
        {"wasp", KIND_INSECT}, {"hive", KIND_INSECT}, {"crab", KIND_INSECT},
        {"crustacean", KIND_INSECT}, {"mantis", KIND_INSECT}, {"weaver", KIND_INSECT},
        {"dragon", KIND_DRAGON}, {"wyrm", KIND_DRAGON}, {"drake", KIND_DRAGON},
        {"wyvern", KIND_DRAGON}, {"draconic", KIND_DRAGON},
        {"serpent", KIND_SERPENT}, {"snake", KIND_SERPENT}, {"naga", KIND_SERPENT},
        {"worm", KIND_SERPENT}, {"eel", KIND_SERPENT}, {"viper", KIND_SERPENT},
        {"ooze", KIND_OOZE}, {"slime", KIND_OOZE}, {"jelly", KIND_OOZE},
        {"pudding", KIND_OOZE}, {"blob", KIND_OOZE}, {"liquid", KIND_OOZE},
        {"bat", KIND_FLYER}, {"bird", KIND_FLYER}, {"hawk", KIND_FLYER},
        {"eagle", KIND_FLYER}, {"owl", KIND_FLYER}, {"vulture", KIND_FLYER},
        {"harpy", KIND_FLYER}, {"feather", KIND_FLYER}, {"raven", KIND_FLYER},
        {"crow", KIND_FLYER}, {"griffon", KIND_FLYER}, {"wing", KIND_FLYER},
        {"skeleton", KIND_UNDEAD}, {"zombie", KIND_UNDEAD}, {"lich", KIND_UNDEAD},
        {"ghoul", KIND_UNDEAD}, {"mummy", KIND_UNDEAD}, {"vampire", KIND_UNDEAD},
        {"wight", KIND_UNDEAD}, {"bone", KIND_UNDEAD}, {"corpse", KIND_UNDEAD},
        {"ghost", KIND_SPIRIT}, {"wraith", KIND_SPIRIT}, {"spirit", KIND_SPIRIT},
        {"phantom", KIND_SPIRIT}, {"specter", KIND_SPIRIT}, {"spectre", KIND_SPIRIT},
        {"shade", KIND_SPIRIT}, {"banshee", KIND_SPIRIT}, {"wisp", KIND_SPIRIT},
        {"mist", KIND_SPIRIT},
        {"golem", KIND_CONSTRUCT}, {"automaton", KIND_CONSTRUCT}, {"construct", KIND_CONSTRUCT},
        {"animated", KIND_CONSTRUCT}, {"clockwork", KIND_CONSTRUCT},
        {"giant", KIND_GIANT}, {"troll", KIND_GIANT}, {"ogre", KIND_GIANT},
        {"titan", KIND_GIANT}, {"cyclops", KIND_GIANT}, {"ettin", KIND_GIANT},
        {"wolf", KIND_BEAST}, {"bear", KIND_BEAST}, {"boar", KIND_BEAST},
        {"rat", KIND_BEAST}, {"hound", KIND_BEAST}, {"dog", KIND_BEAST},
        {"cat", KIND_BEAST}, {"lion", KIND_BEAST}, {"tiger", KIND_BEAST},
        {"elk", KIND_BEAST}, {"stag", KIND_BEAST}, {"hare", KIND_BEAST},
        {"horse", KIND_BEAST}, {"ape", KIND_BEAST}, {"lizard", KIND_BEAST},
        {"tree", KIND_PLANT}, {"shrub", KIND_PLANT}, {"vine", KIND_PLANT},
        {"fungus", KIND_PLANT}, {"mushroom", KIND_PLANT}, {"root", KIND_PLANT},
        {"moss", KIND_PLANT}, {"algae", KIND_PLANT}, {"bloom", KIND_PLANT},
        {"demon", KIND_FIEND}, {"devil", KIND_FIEND}, {"fiend", KIND_FIEND},
        {"imp", KIND_FIEND}, {"infernal", KIND_FIEND}, {"abyss", KIND_FIEND},
        {"angel", KIND_CELESTIAL}, {"seraph", KIND_CELESTIAL}, {"celestial", KIND_CELESTIAL},
        {"eye", KIND_ABERRATION}, {"tentacle", KIND_ABERRATION}, {"mind", KIND_ABERRATION},
        {"elemental", KIND_ELEMENTAL}, {"gale", KIND_ELEMENTAL}, {"flame", KIND_ELEMENTAL},
        {"storm", KIND_ELEMENTAL},
        /*people: checked last, so "Goblin-Wolf Shifter" stays a beast, but
         * before the "Type:" tag (goblins are tagged Monstrosity)*/
        {"goblin", KIND_HUMANOID}, {"hobgoblin", KIND_HUMANOID}, {"orc", KIND_HUMANOID},
        {"kobold", KIND_HUMANOID}, {"gnoll", KIND_HUMANOID}, {"bandit", KIND_HUMANOID},
        {"raider", KIND_HUMANOID}, {"cultist", KIND_HUMANOID}, {"knight", KIND_HUMANOID},
        {"warrior", KIND_HUMANOID}, {"soldier", KIND_HUMANOID}, {"guard", KIND_HUMANOID},
        {"mage", KIND_HUMANOID}, {"priest", KIND_HUMANOID}, {"thief", KIND_HUMANOID},
        {"assassin", KIND_HUMANOID}, {"pirate", KIND_HUMANOID}, {"dwarf", KIND_HUMANOID},
        {"dwarven", KIND_HUMANOID}, {"elf", KIND_HUMANOID}, {"elven", KIND_HUMANOID},
    };
    for (size_t i = 0; i < sizeof(name_rules) / sizeof(name_rules[0]); i++) {
        if (has_word(n, name_rules[i].kw)) {
            /*"ant" and "rat" are substrings of many words: require a word start*/
            const char *p = strcasestr(n, name_rules[i].kw);
            size_t kl = strlen(name_rules[i].kw);
            if (kl <= 3) {
                bool start = (p == n) || !isalpha((unsigned char)p[-1]);
                bool end = !isalpha((unsigned char)p[kl]) || p[kl] == 's';
                if (!start || !end) continue;
            }
            return name_rules[i].kind;
        }
    }
    const char *d = t->description;
    static const struct { const char *tag; int kind; } type_rules[] = {
        {"Type: Beast", KIND_BEAST}, {"Type: Wild Animal", KIND_BEAST},
        {"Type: Mammal", KIND_BEAST}, {"Type: Magical Beast", KIND_BEAST},
        {"Type: Lycanthrope", KIND_BEAST}, {"Type: Reptile", KIND_BEAST},
        {"Type: Insect", KIND_INSECT}, {"Type: Vermin", KIND_INSECT},
        {"Type: Undead", KIND_UNDEAD}, {"Type: Necrotic", KIND_UNDEAD},
        {"Type: Fiend", KIND_FIEND}, {"Type: Demon", KIND_FIEND},
        {"Type: Elemental", KIND_ELEMENTAL}, {"Type: Genie", KIND_ELEMENTAL},
        {"Type: Construct", KIND_CONSTRUCT},
        {"Type: Aberration", KIND_ABERRATION}, {"Type: Eldritch", KIND_ABERRATION},
        {"Type: Abomination", KIND_ABERRATION},
        {"Type: Dragon", KIND_DRAGON}, {"Type: Wyrm", KIND_DRAGON},
        {"Type: Plant", KIND_PLANT}, {"Type: Nature Spirit", KIND_SPIRIT},
        {"Type: Fey", KIND_FEY},
        {"Type: Celestial", KIND_CELESTIAL},
        {"Type: Giant", KIND_GIANT},
        {"Type: Ooze", KIND_OOZE},
        {"Type: Spirit", KIND_SPIRIT}, {"Type: Planar", KIND_SPIRIT},
        {"Type: Monstrosity", KIND_BEAST},
    };
    if (d) {
        for (size_t i = 0; i < sizeof(type_rules) / sizeof(type_rules[0]); i++)
            if (strstr(d, type_rules[i].tag)) return type_rules[i].kind;
    }
    return KIND_HUMANOID;
}

int ui_creature_kind(const NPC *n) {
    if (!n) return KIND_HUMANOID;
    switch (n->archetype) {
    case ARCH_TREASURE: return KIND_CHEST;
    case ARCH_GOLD:     return KIND_GOLD;
    case ARCH_MERCHANT: return KIND_MERCHANT;
    default: break;
    }
    if (n->is_ghost) return KIND_SPIRIT;
    /*Per-template cache: the keyword scan runs once per bestiary entry.*/
    static signed char *cache = NULL;
    static int cache_n = 0;
    if (!n->template) return (n->archetype == ARCH_DRAGON) ? KIND_DRAGON : KIND_ELEMENTAL;
    int idx = n->template_idx;
    if (idx < 0 || idx >= bestiary_size) return kind_from_template(n->template);
    if (cache == NULL || cache_n != bestiary_size) {
        free(cache);
        cache = malloc((size_t)bestiary_size);
        if (!cache) return kind_from_template(n->template);
        memset(cache, -1, (size_t)bestiary_size);
        cache_n = bestiary_size;
    }
    if (cache[idx] < 0) cache[idx] = (signed char)kind_from_template(&bestiary_data[idx]);
    return cache[idx];
}

int ui_npc_level(const NPC *n) {
    if (!n) return 1;
    /*Challenge level for the name plate: grows with the XP reward (the
     * bestiary's own difficulty measure), bosses get a bump.*/
    int xp = n->xp_reward > 0 ? n->xp_reward : 10;
    int lvl = (int)lroundf(sqrtf((float)xp / 12.0f));
    if (n->archetype == ARCH_BOSS) lvl += 3;
    if (lvl < 1) lvl = 1;
    if (lvl > 40) lvl = 40;
    return lvl;
}

uint32_t ui_look_seed(const NPC *n) {
    if (!n) return 0;
    if (n->template_idx >= 0) return fnv1a(&n->template_idx, sizeof(int), 2166136261u);
    return fnv1a(&n->entity_id, sizeof(int), 2166136261u);
}

/* ------------------------------------------------------------------ */
/* Inventory                                                           */
/* ------------------------------------------------------------------ */

static void fill_inv_item(InvItem *o, const ItemInstance *it, int where, int slot) {
    memset(o, 0, sizeof(*o));
    const ItemTemplate *t = &item_database[it->template_idx];
    char name[128];
    get_full_item_name(it, name, sizeof(name));
    copy_str(o->name, name, sizeof(o->name));
    o->category = (uint8_t)t->category;
    o->where = (uint8_t)where;
    o->slot = (uint8_t)slot;
    o->flags = 0;
    if (it->is_identified || !t->needs_id) o->flags |= INVF_IDENTIFIED;
    if (it->blessing == BLESS_CURSED && it->is_identified) o->flags |= INVF_CURSED;
    if (it->is_artifact) o->flags |= INVF_ARTIFACT;
    if (t->str_bonus || t->dex_bonus || t->con_bonus || t->int_bonus ||
        t->wis_bonus || t->cha_bonus || it->element != ELEM_NONE || t->cast_spell_idx >= 0)
        o->flags |= INVF_MAGIC;
    o->qty = (uint16_t)(it->stack_count > 0 ? (it->stack_count > 65535 ? 65535 : it->stack_count) : 1);
    o->durability = (int16_t)it->durability;
    o->max_durability = (int16_t)t->max_durability;
    o->dmg_count = (int16_t)t->damage_dice_count;
    o->dmg_sides = (int16_t)t->damage_dice_sides;
    o->ac = (int16_t)(t->category == ITEM_SHIELD ? t->ac_bonus : t->ac_base);
    o->heal = (int16_t)t->heal_amount;
    o->light = (int16_t)t->light_radius;
    o->value = (uint32_t)(t->cost > 0xFFFFFFFFull ? 0xFFFFFFFFu : t->cost);
    o->weight = t->weight;
}

static void build_inventory(Client *c, MsgInventory *m) {
    memset(m, 0, sizeof(*m));
    int n = 0;
    for (int i = 0; i < c->backpack_count && n < INV_MAX_ITEMS; i++) {
        if (c->backpack[i].template_idx < 0 || c->backpack[i].template_idx >= item_database_size) continue;
        fill_inv_item(&m->items[n++], &c->backpack[i], INV_BACKPACK, i);
    }
    for (int i = 0; i < MAX_BELT && n < INV_MAX_ITEMS; i++) {
        if (c->belt[i].template_idx < 0 || c->belt[i].template_idx >= item_database_size) continue;
        fill_inv_item(&m->items[n++], &c->belt[i], INV_BELT, i);
    }
    ItemInstance *eq[EQ_SLOT_COUNT] = {
        &c->slot_head, &c->slot_neck, &c->slot_body, &c->slot_back,
        &c->slot_hand_r, &c->slot_hand_l, &c->slot_hands, &c->slot_arm_r,
        &c->slot_arm_l, &c->slot_feet,
        &c->slot_rings[0], &c->slot_rings[1], &c->slot_rings[2], &c->slot_rings[3],
        &c->slot_rings[4], &c->slot_rings[5], &c->slot_rings[6], &c->slot_rings[7],
        &c->slot_rings[8], &c->slot_rings[9]};
    for (int s = 0; s < EQ_SLOT_COUNT && n < INV_MAX_ITEMS; s++) {
        if (eq[s]->template_idx < 0 || eq[s]->template_idx >= item_database_size) continue;
        fill_inv_item(&m->items[n++], eq[s], INV_EQUIPPED, s);
    }
    m->count = n;
    m->weight_now = get_current_weight(c);
    m->weight_max = (float)(c->str * 15); /*pounds, see get_movement_cooldown*/
}

/* ------------------------------------------------------------------ */
/* Spellbook                                                           */
/* ------------------------------------------------------------------ */

static void build_spellbook(Client *c, MsgSpellbook *m) {
    memset(m, 0, sizeof(*m));
    int n = 0;
    for (int lv = 0; lv <= MAX_SPELL_LEVEL; lv++) {
        for (int i = 0; i < spell_database_size && n < BOOK_MAX_SPELLS; i++) {
            if (spell_database[i].level != lv || !spell_known_get(c, i)) continue;
            SpellEntry *e = &m->spells[n++];
            copy_str(e->name, spell_database[i].name ? spell_database[i].name : "?", sizeof(e->name));
            e->level = (int8_t)lv;
            e->target_type = (uint8_t)spell_database[i].target_type;
            e->effect_type = (uint8_t)spell_database[i].effect_type;
            e->damage_type = (uint8_t)spell_database[i].damage_type;
            e->range = (int16_t)spell_database[i].range;
            e->radius = (int16_t)spell_database[i].radius;
        }
    }
    m->count = n;
}

/* ------------------------------------------------------------------ */
/* Shop                                                                */
/* ------------------------------------------------------------------ */

static NPC *find_npc_by_entity(int entity_id) {
    if (!g_npcs || entity_id <= 0) return NULL;
    for (int i = 0; i < MAX_NPCS; i++)
        if (g_npcs[i].active && g_npcs[i].entity_id == entity_id) return &g_npcs[i];
    return NULL;
}

static void build_shop(const NPC *merchant, MsgShop *m) {
    memset(m, 0, sizeof(*m));
    m->merchant_id = merchant->entity_id;
    copy_str(m->shop_name, merchant->merchant.shop_name, sizeof(m->shop_name));
    int n = 0;
    for (int j = 0; j < merchant->merchant.item_count && n < SHOP_MAX_ITEMS; j++) {
        int ti = merchant->merchant.item_templates[j];
        if (ti < 0 || ti >= item_database_size) continue;
        const ItemTemplate *it = &item_database[ti];
        ShopItem *o = &m->items[n++];
        copy_str(o->name, it->name, sizeof(o->name));
        o->category = (uint8_t)it->category;
        o->stock = (int16_t)merchant->merchant.item_stock[j];
        const char *type = "Misc";
        char stats[32] = "";
        switch (it->category) {
        case ITEM_WEAPON: type = "Weapon";
            snprintf(stats, sizeof(stats), "%dd%d dmg", it->damage_dice_count, it->damage_dice_sides); break;
        case ITEM_ARMOR: type = "Armor"; snprintf(stats, sizeof(stats), "AC %d", it->ac_base); break;
        case ITEM_SHIELD: type = "Shield"; snprintf(stats, sizeof(stats), "+%d AC", it->ac_bonus); break;
        case ITEM_HEAD: type = "Helm"; if (it->ac_bonus) snprintf(stats, sizeof(stats), "+%d AC", it->ac_bonus); break;
        case ITEM_FEET: type = "Boots"; break;
        case ITEM_HANDS: type = "Gloves"; break;
        case ITEM_BACK: type = "Cloak"; break;
        case ITEM_CONSUMABLE:
            type = "Consum.";
            if (it->heal_amount > 0) snprintf(stats, sizeof(stats), "Heal %d HP", it->heal_amount);
            break;
        case ITEM_RING: type = "Ring"; break;
        case ITEM_NECK: type = "Amulet"; break;
        case ITEM_BRACELET: type = "Bracelet"; break;
        case ITEM_LIGHT_SOURCE: type = "Light";
            snprintf(stats, sizeof(stats), "%d turns", it->max_durability > 0 ? it->max_durability : 100); break;
        case ITEM_FUEL: type = "Fuel";
            snprintf(stats, sizeof(stats), "%d turns", it->duration_turns > 0 ? it->duration_turns : 300); break;
        case ITEM_AMMO: type = "Ammo"; break;
        case ITEM_BOOK: type = "Book"; break;
        default: break;
        }
        if (it->str_bonus > 0 || it->dex_bonus > 0 || it->con_bonus > 0 ||
            it->int_bonus > 0 || it->wis_bonus > 0 || it->cha_bonus > 0) {
            if (stats[0] == '\0') snprintf(stats, sizeof(stats), "Magic");
            else strncat(stats, " (*)", sizeof(stats) - strlen(stats) - 1);
        }
        copy_str(o->type, type, sizeof(o->type));
        copy_str(o->stats, stats[0] ? stats : "-", sizeof(o->stats));
        /*Same dynamic price as print_merchant_inventory (main_server.c)*/
        float supply = 1.0f;
        if (merchant->merchant.item_stock_max[j] > 0)
            supply = (float)merchant->merchant.item_stock[j] / (float)merchant->merchant.item_stock_max[j];
        float mod = 1.0f + (1.0f - supply) * 0.5f;
        if (mod < 0.5f) mod = 0.5f;
        if (mod > 2.0f) mod = 2.0f;
        uint64_t cost = (uint64_t)(it->cost * mod);
        o->cost = (uint32_t)(cost > 0xFFFFFFFFull ? 0xFFFFFFFFu : cost);
    }
    m->count = n;
}

void ui_open_shop(Client *c, NPC *merchant) {
    int ci = client_index(c);
    if (ci < 0 || !merchant) return;
    s_shop_merchant[ci] = merchant->entity_id;
    s_shop_hash[ci] = 0xFFFFFFFFu; /*force a send*/
    ui_sync_client(c);
}

/* ------------------------------------------------------------------ */

void ui_sync_client(Client *c) {
    int ci = client_index(c);
    if (ci < 0 || !c->active || !c->authenticated) return;

    /*The three payloads are a few KB each: keep them off the stack of
     * the (deep) command handlers.*/
    static MsgInventory inv;
    build_inventory(c, &inv);
    size_t inv_len = offsetof(MsgInventory, items) + sizeof(InvItem) * (size_t)inv.count;
    uint32_t h = fnv1a(&inv, inv_len, 2166136261u);
    if (h != s_inv_hash[ci]) {
        s_inv_hash[ci] = h;
        MsgHeader hdr = msg_hdr(MSG_INVENTORY, (int)sizeof(MsgInventory));
        if (!net_send_client(c->sock, &hdr, sizeof(hdr)) ||
            !net_send_client(c->sock, &inv, sizeof(inv)))
            return;
    }

    static MsgSpellbook book;
    build_spellbook(c, &book);
    size_t book_len = offsetof(MsgSpellbook, spells) + sizeof(SpellEntry) * (size_t)book.count;
    h = fnv1a(&book, book_len, 2166136261u);
    if (h != s_book_hash[ci]) {
        s_book_hash[ci] = h;
        MsgHeader hdr = msg_hdr(MSG_SPELLBOOK, (int)sizeof(MsgSpellbook));
        if (!net_send_client(c->sock, &hdr, sizeof(hdr)) ||
            !net_send_client(c->sock, &book, sizeof(book)))
            return;
    }

    if (s_shop_merchant[ci] != 0) {
        static MsgShop shop;
        NPC *m = find_npc_by_entity(s_shop_merchant[ci]);
        bool close_by = m && m->floor_id == c->floor_id &&
                    abs(m->x - c->x) <= 2 && abs(m->y - c->y) <= 2;
        if (!close_by) {
            /*Walked away (or the merchant vanished): close the window.*/
            memset(&shop, 0, sizeof(shop));
            s_shop_merchant[ci] = 0;
            MsgHeader hdr = msg_hdr(MSG_SHOP, (int)sizeof(MsgShop));
            if (!net_send_client(c->sock, &hdr, sizeof(hdr))) return;
            net_send_client(c->sock, &shop, sizeof(shop));
            return;
        }
        build_shop(m, &shop);
        size_t shop_len = offsetof(MsgShop, items) + sizeof(ShopItem) * (size_t)shop.count;
        h = fnv1a(&shop, shop_len, 2166136261u);
        if (h != s_shop_hash[ci]) {
            s_shop_hash[ci] = h;
            MsgHeader hdr = msg_hdr(MSG_SHOP, (int)sizeof(MsgShop));
            if (!net_send_client(c->sock, &hdr, sizeof(hdr))) return;
            net_send_client(c->sock, &shop, sizeof(shop));
        }
    }
}

/* ------------------------------------------------------------------ */
/* Game events                                                         */
/* ------------------------------------------------------------------ */

void ui_send_event(Client *c, int type, int src, int dst, int value, int x, int y) {
    if (!c || !c->active || !c->authenticated) return;
    MsgHeader hdr = msg_hdr(MSG_GAME_EVENT, (int)sizeof(MsgGameEvent));
    MsgGameEvent ev;
    ev.type = type;
    ev.src_id = src;
    ev.dst_id = dst;
    ev.value = value;
    ev.x = (int16_t)x;
    ev.y = (int16_t)y;
    if (!net_send_client(c->sock, &hdr, sizeof(hdr))) return;
    net_send_client(c->sock, &ev, sizeof(ev));
}

void ui_broadcast_event(int floor_id, int type, int src, int dst, int value, int x, int y) {
    if (!g_clients) return;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        Client *o = &g_clients[i];
        if (!o->active || !o->authenticated || o->floor_id != floor_id) continue;
        /*Only clients that can plausibly see it (same radius as the
         * entity broadcast in server_net.c).*/
        if (abs(o->x - x) > 64 || abs(o->y - y) > 64) continue;
        ui_send_event(o, type, src, dst, value, x, y);
    }
}
