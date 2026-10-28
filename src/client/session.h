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

/*Connection sessions and the private "solo" server.
 *
 * session_start() runs connect + login + the receive loop on a
 * background thread, so the menus stay responsive (and animated) while
 * connecting; progress is reported through g_net_status/g_net_error.
 * The solo mode spawns dragongl_server next to the client binary,
 * bound to 127.0.0.1, with its world stored in the user profile.*/
#ifndef SESSION_H
#define SESSION_H

#include <stdbool.h>

typedef struct {
    char host[128];
    int port;
    char server_pass[64];
    char user[32];
    char pass[32];
    bool is_new;
    int race_id, subrace_id, class_id, alignment;
    char sex; /*'M' or 'F' (cosmetic)*/
    /*Keep retrying the connection for this long (ms): the solo server
     * may still be generating its world when the client first tries.*/
    int retry_ms;
} LoginInfo;

bool session_start(const LoginInfo *info);
void session_stop(void);            /*disconnect and join the threads*/
bool session_active(void);
bool session_stop_requested(void);
const char *session_status_text(void); /*"Connecting...", ...*/

/*Solo server*/
bool local_server_start(int port, const char *password, char *err, int err_len);
void local_server_stop(void);
bool local_server_running(void);
/*Last lines of the solo server log (for error reporting).*/
void local_server_log_tail(char *out, int cap);
/*Directory holding the solo world (user profile).*/
const char *local_server_dir(void);

#endif
