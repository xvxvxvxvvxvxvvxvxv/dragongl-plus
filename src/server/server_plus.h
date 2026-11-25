/*
 * DragonGL+ - the protocol extension on the server side
 * Copyright (C) 2026 Nicola Taibi - GPL-3.0-or-later
 *
 * Inventory, shop, spellbook, progress, creature identity and game
 * events for the clients that negotiated the extension
 * (protocol_plus.h); every function is a no-op for the others.
 */
#ifndef DRAGONGL_SERVER_PLUS_H
#define DRAGONGL_SERVER_PLUS_H

#include "game.h"
#include "protocol_plus.h"
#include "server_entities.h"

/* Forget what was sent to c (login, logout). */
void plus_reset(const Game *game, const Client *c);
/* Send what changed for c (inventory, spellbook, progress, open shop). */
void plus_sync_client(Game *game, Client *c);
void plus_sync_all(Game *game);
/* Open (and send) the shop window of merchant for c. */
void plus_open_shop(Game *game, Client *c, const NPC *merchant);
/* Identity of an entity, after the MSG_STATE that introduces it. */
void plus_send_npc_info(int sock, const NPC *n);
void plus_send_player_info(int sock, const Client *p);
/* Game events (GEV_*): to one client, or to every client near (x, y). */
void plus_event(Client *c, int type, int src, int dst, int value, int x, int y);
void plus_broadcast_event(Game *game, int floor_id, int type, int src, int dst, int value, int x,
                          int y);
/* Creature identity (also used by tests). */
int plus_creature_kind(const NPC *n);
int plus_npc_level(const NPC *n);
uint32_t plus_look_seed(const NPC *n);

#endif
