/*
 * DragonGL+ - codec of the protocol extension (protocol_plus.h)
 * Copyright (C) 2026 Nicola Taibi - GPL-3.0-or-later
 *
 * Same rules as wire.h: every field written explicitly, little-endian;
 * strings as a u8 length and the bytes; a payload must decode completely
 * and be consumed exactly, a count above its limit is a protocol error.
 */
#ifndef DRAGONGL_WIRE_PLUS_H
#define DRAGONGL_WIRE_PLUS_H

#include <stdbool.h>
#include <stddef.h>
#include "binio.h"
#include "protocol_plus.h"

/* Largest payload of an extension message the server may send; 0 for any
 * other type. */
uint32_t wire_plus_server_payload_max(MsgType type);

bool wire_put_plus_entity_info(BinWriter *w, const PlusEntityInfo *m);
bool wire_get_plus_entity_info(const void *p, size_t n, PlusEntityInfo *m);
bool wire_put_plus_player_ext(BinWriter *w, const PlusPlayerExt *m);
bool wire_get_plus_player_ext(const void *p, size_t n, PlusPlayerExt *m);
bool wire_put_plus_inventory(BinWriter *w, const MsgInventory *m);
bool wire_get_plus_inventory(const void *p, size_t n, MsgInventory *m);
bool wire_put_plus_shop(BinWriter *w, const MsgShop *m);
bool wire_get_plus_shop(const void *p, size_t n, MsgShop *m);
bool wire_put_plus_spellbook(BinWriter *w, const MsgSpellbook *m);
bool wire_get_plus_spellbook(const void *p, size_t n, MsgSpellbook *m);
bool wire_put_plus_game_event(BinWriter *w, const MsgGameEvent *m);
bool wire_get_plus_game_event(const void *p, size_t n, MsgGameEvent *m);

#endif
