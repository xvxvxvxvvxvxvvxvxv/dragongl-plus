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

/*Structured UI data for the graphical client (protocol v4) — see
 * server_ui_sync.c.*/
#ifndef SERVER_UI_SYNC_H
#define SERVER_UI_SYNC_H

#include <stdint.h>
#include "server_entities.h"

/*Forget what was sent to client slot ci (call on connect/login).*/
void ui_sync_reset(int ci);
/*Send inventory/spellbook/shop if they changed since the last call.*/
void ui_sync_client(Client *c);
/*Open (and send) the shop window of merchant for c.*/
void ui_open_shop(Client *c, NPC *merchant);
/*Visual identity of an NPC for MsgState.*/
int ui_creature_kind(const NPC *n);
int ui_npc_level(const NPC *n);
uint32_t ui_look_seed(const NPC *n);
/*Game events: to one client, or to every client near (x,y) on a floor.*/
void ui_send_event(Client *c, int type, int src, int dst, int value, int x, int y);
void ui_broadcast_event(int floor_id, int type, int src, int dst, int value, int x, int y);

#endif
