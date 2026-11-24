/*
 * DragonGL+ - characters of the DragonGL+ server of October 2026
 * Copyright (C) 2026 Nicola Taibi - GPL-3.0-or-later
 * (see legacy_import.h)
 */

#include "legacy_import.h"

#include "alignment.h"
#include "auth.h"
#include "binio.h"
#include "data_loader.h"
#include "items.h"
#include "persist.h"
#include "player.h"
#include "server_internal.h"
#include "server_log.h"
#include "spells.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

/* --- The file format of DragonGL+ 2026.10 (raw structs, x86-64) ------- */

#define LEGACY_MAGIC 0x44474C53u /* 'D''G''L''S' */
#define LEGACY_VERSION 1u

typedef struct {
    uint32_t magic, version, crc32, reserved;
} LegacyHeader;

typedef struct {
    int template_idx;
    int durability;
    int stack_count;
    bool is_identified;
    int current_charges;
    int to_hit_bonus;
    int to_dam_bonus;
    int ac_bonus;
    int quality, blessing, element; /* enums */
    bool is_artifact;
    char artifact_name[64];
    int artifact_str_bonus;
    int artifact_dex_bonus;
    int artifact_con_bonus;
} LegacyItem;

typedef struct {
    int cond, trigger, mod_type, value, duration_rounds, is_persistent;
} LegacyEffect;

typedef struct {
    char password[80];
    int x, y, floor_id;
    int race_id, subrace_id, class_id;
    int level, xp, str, dex, con, intel, wis, cha;
    uint64_t gold;
    int hp, max_hp;
    LegacyItem backpack[32];
    int backpack_count;
    LegacyItem belt[4];
    LegacyItem s_head, s_neck, s_body, s_back, s_hand_r, s_hand_l, s_hands, s_arm_r, s_arm_l,
        s_feet, s_rings[10];
    int spell_slots[10], spell_slots_max[10];
    int alignment;
    LegacyEffect effects[16];
    int effect_count;
    int light_turns_left, hunger_level, exhaustion_level;
    int total_kills, total_steps, max_floor_explored;
    uint64_t known_spells[16];
    uint32_t bosses_defeated;
    int unspent_stat_points;
    char blocked_players[16][32];
    int blocked_count;
} LegacySave;

/* --- Name matching --------------------------------------------------- */

static bool same_name(const char *a, const char *b) {
    while (*a == ' ') {
        a++;
    }
    while (*b == ' ') {
        b++;
    }
    size_t la = strlen(a), lb = strlen(b);
    while (la > 0 && a[la - 1] == ' ') {
        la--;
    }
    while (lb > 0 && b[lb - 1] == ' ') {
        lb--;
    }
    return la == lb && strncasecmp(a, b, la) == 0;
}

static int item_by_legacy_index(int old_idx) {
    if (old_idx < 0 || old_idx >= LEGACY_ITEM_COUNT) {
        return -1;
    }
    const char *name = LEGACY_ITEM_NAMES[old_idx];
    for (int i = 0; i < item_database_size; i++) {
        if (item_database[i].name && same_name(item_database[i].name, name)) {
            return i;
        }
    }
    return -1;
}

static int spell_by_legacy_index(int old_idx) {
    if (old_idx < 0 || old_idx >= LEGACY_SPELL_COUNT) {
        return -1;
    }
    const char *name = LEGACY_SPELL_NAMES[old_idx];
    for (int i = 0; i < spell_database_size; i++) {
        if (spell_database[i].name && same_name(spell_database[i].name, name)) {
            return i;
        }
    }
    return -1;
}

/* An old item as an item of today's data; template_idx -1 if it no longer
 * exists (it is dropped, and counted). */
static ItemInstance convert_item(const LegacyItem *o, int *lost) {
    ItemInstance it;
    memset(&it, 0, sizeof(it));
    it.template_idx = -1;
    if (o->template_idx < 0) {
        return it;
    }
    int idx = item_by_legacy_index(o->template_idx);
    if (idx < 0) {
        (*lost)++;
        return it;
    }
    it.template_idx = idx;
    it.durability = o->durability;
    it.stack_count = o->stack_count > 0 ? o->stack_count : 1;
    it.is_identified = o->is_identified;
    it.current_charges = o->current_charges;
    it.to_hit_bonus = o->to_hit_bonus;
    it.to_dam_bonus = o->to_dam_bonus;
    it.ac_bonus = o->ac_bonus;
    it.quality = (o->quality >= QUALITY_RUSTY && o->quality <= QUALITY_MASTERWORK)
                     ? (ItemQuality)o->quality
                     : QUALITY_NORMAL;
    it.blessing = (o->blessing >= BLESS_CURSED && o->blessing <= BLESS_BLESSED)
                      ? (ItemBlessing)o->blessing
                      : BLESS_NORMAL;
    it.element = (o->element >= ELEM_NONE && o->element <= ELEM_POISONOUS) ? (ItemElement)o->element
                                                                             : ELEM_NONE;
    it.is_artifact = o->is_artifact;
    memcpy(it.artifact_name, o->artifact_name, sizeof(it.artifact_name));
    it.artifact_name[sizeof(it.artifact_name) - 1] = '\0';
    it.artifact_str_bonus = o->artifact_str_bonus;
    it.artifact_dex_bonus = o->artifact_dex_bonus;
    it.artifact_con_bonus = o->artifact_con_bonus;
    return it;
}

static int clamp_int(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* --- One character ---------------------------------------------------- */

static bool read_legacy(const char *path, LegacySave *sd) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    LegacyHeader h;
    bool ok = fread(&h, 1, sizeof(h), f) == sizeof(h) && h.magic == LEGACY_MAGIC &&
              h.version == LEGACY_VERSION && fread(sd, 1, sizeof(*sd), f) == sizeof(*sd) &&
              fgetc(f) == EOF && crc32_data(sd, sizeof(*sd)) == h.crc32;
    fclose(f);
    return ok;
}

static bool import_one(const char *path, const char *username, const char *password) {
    static Client c; /* large: not on the stack */
    LegacySave sd;
    memset(&sd, 0, sizeof(sd));
    if (!read_legacy(path, &sd)) {
        return false; /* not a save of DragonGL+ 2026.10 (or damaged) */
    }
    memset(&c, 0, sizeof(c));
    copy_str(c.username, username, sizeof(c.username));
    if (!auth_hash_password(password, c.password_hash)) {
        server_log("IMPORT", "%s: cannot hash the password", username);
        return false;
    }
    c.authenticated = true;
    c.entity_id = -1;
    c.pending_trade_merchant_id = -1;
    /* back in the village square */
    c.floor_id = 0;
    c.x = MAP_CENTER_X;
    c.y = MAP_CENTER_Y;
    c.race_id = (RaceType)clamp_int(sd.race_id, 0, RACE_COUNT - 1);
    c.subrace_id = (SubraceType)clamp_int(sd.subrace_id, -1, SUBRACE_COUNT - 1);
    c.class_id = (ClassType)clamp_int(sd.class_id, 0, CLASS_COUNT - 1);
    c.alignment = clamp_int(sd.alignment, 0, ALIGN_COUNT - 1);
    c.level = clamp_int(sd.level, 1, 20);
    c.xp = sd.xp > 0 ? sd.xp : 0;
    c.str = clamp_int(sd.str, 1, 30);
    c.dex = clamp_int(sd.dex, 1, 30);
    c.con = clamp_int(sd.con, 1, 30);
    c.intel = clamp_int(sd.intel, 1, 30);
    c.wis = clamp_int(sd.wis, 1, 30);
    c.cha = clamp_int(sd.cha, 1, 30);
    c.unspent_stat_points = clamp_int(sd.unspent_stat_points, 0, 60);
    c.gold = sd.gold;
    c.max_hp = sd.max_hp > 0 ? sd.max_hp : 1;
    c.hp = c.max_hp; /* rested in the village */
    c.max_floor_explored = clamp_int(sd.max_floor_explored, 0, MAX_FLOORS - 1);
    c.bosses_defeated = sd.bosses_defeated;
    c.light_turns_left = sd.light_turns_left > 0 ? sd.light_turns_left : 0;
    c.total_kills = sd.total_kills;
    c.total_steps = sd.total_steps;
    class_spell_slots(c.class_id, c.level, c.spell_slots_max);
    for (int s = 0; s <= MAX_SPELL_LEVEL; s++) {
        c.spell_slots[s] = c.spell_slots_max[s];
    }

    int lost = 0;
    int n = 0;
    for (int i = 0; i < sd.backpack_count && i < 32 && n < MAX_BACKPACK; i++) {
        ItemInstance it = convert_item(&sd.backpack[i], &lost);
        if (it.template_idx >= 0) {
            c.backpack[n++] = it;
        }
    }
    c.backpack_count = n;
    for (int b = 0; b < MAX_BELT; b++) {
        c.belt[b] = convert_item(&sd.belt[b], &lost);
    }
    c.slot_head = convert_item(&sd.s_head, &lost);
    c.slot_neck = convert_item(&sd.s_neck, &lost);
    c.slot_body = convert_item(&sd.s_body, &lost);
    c.slot_back = convert_item(&sd.s_back, &lost);
    c.slot_hand_r = convert_item(&sd.s_hand_r, &lost);
    c.slot_hand_l = convert_item(&sd.s_hand_l, &lost);
    c.slot_hands = convert_item(&sd.s_hands, &lost);
    c.slot_arm_r = convert_item(&sd.s_arm_r, &lost);
    c.slot_arm_l = convert_item(&sd.s_arm_l, &lost);
    c.slot_feet = convert_item(&sd.s_feet, &lost);
    for (int r = 0; r < 10; r++) {
        c.slot_rings[r] = convert_item(&sd.s_rings[r], &lost);
    }

    int spells = 0, spells_lost = 0;
    for (int w = 0; w < 16; w++) {
        for (int bit = 0; bit < 64; bit++) {
            if (!((sd.known_spells[w] >> bit) & 1ULL)) {
                continue;
            }
            int idx = spell_by_legacy_index(w * 64 + bit);
            if (idx >= 0) {
                spell_known_set(&c, idx);
                spells++;
            } else {
                spells_lost++;
            }
        }
    }
    for (int i = 0; i < sd.blocked_count && i < 16 && c.blocked_count < MAX_BLOCKED_PLAYERS; i++) {
        sd.blocked_players[i][31] = '\0';
        copy_str(c.blocked_players[c.blocked_count++], sd.blocked_players[i],
                 sizeof(c.blocked_players[0]));
    }
    auth_wipe(&sd, sizeof(sd));
    save_player_data(&c);
    server_log("IMPORT", "%s imported from DragonGL+ 2026.10: level %d, %d spells%s", username,
               c.level, spells, (lost || spells_lost) ? " (some items or spells no longer exist)" : "");
    return true;
}

/* --- Every character of a directory ----------------------------------- */

static bool valid_name(const char *s, size_t n) {
    if (n == 0 || n > 16) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (!isalnum((unsigned char)s[i]) && s[i] != '_') {
            return false;
        }
    }
    return true;
}

int legacy_import_saves(const char *dir, const char *password) {
    if (!dir || !dir[0] || !password || !auth_password_acceptable(password)) {
        return 0;
    }
    DIR *d = opendir(dir);
    if (!d) {
        return 0;
    }
    int imported = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        size_t n = strlen(de->d_name);
        if (n <= 5 || strcmp(de->d_name + n - 5, ".save") != 0 || !valid_name(de->d_name, n - 5)) {
            continue;
        }
        char name[32];
        snprintf(name, sizeof(name), "%.*s", (int)(n - 5), de->d_name);
        char now_path[STATE_DIR_MAX + 64];
        if (!state_path(now_path, sizeof(now_path), "saves/%s.save", name) ||
            access(now_path, F_OK) == 0) {
            continue; /* exists already: never replaced */
        }
        char old_path[4096];
        snprintf(old_path, sizeof(old_path), "%s/%s", dir, de->d_name);
        if (import_one(old_path, name, password)) {
            imported++;
        }
    }
    closedir(d);
    return imported;
}
