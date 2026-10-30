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

/*Sessions & solo server — see session.h.*/

#include "session.h"
#include "client_state.h"
#include "platform.h"
#include "net.h"
#include "protocol.h"
#include "rules.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#ifndef _WIN32
#include <sys/socket.h>
#endif

static pthread_t s_thread;
static bool s_thread_live;
static atomic_bool s_stop;
static LoginInfo s_info;
static char s_status[128] = "";

extern void *net_thread_loop(void *arg);

bool session_stop_requested(void) { return atomic_load(&s_stop); }
bool session_active(void) { return s_thread_live; }
const char *session_status_text(void) { return s_status; }

static void set_status(const char *s) {
    pthread_mutex_lock(&g_state_mutex);
    copy_str(s_status, s, sizeof(s_status));
    pthread_mutex_unlock(&g_state_mutex);
}

/*Connect (retrying until the deadline: the solo server may still be
 * generating its world) and send the login with the current protocol
 * version. Returns the socket, or -1 with g_net_error set.*/
static int connect_and_login(long long deadline) {
    char err[160] = "";
    int sock = -1;
    atomic_store(&g_net_rx_any, false);
    atomic_store(&g_net_status, NET_CONNECTING);
    set_status("Connecting...");
    for (;;) {
        sock = net_connect_timeout(s_info.host, s_info.port, 4000, err, sizeof(err));
        if (sock >= 0 || atomic_load(&s_stop) || plat_time_ms() >= deadline) break;
        set_status("Waiting for the world to be forged...");
        plat_sleep_ms(400);
    }
    if (sock < 0) {
        pthread_mutex_lock(&g_state_mutex);
        copy_str(g_net_error, err, sizeof(g_net_error));
        pthread_mutex_unlock(&g_state_mutex);
        atomic_store(&g_net_status, NET_FAILED);
        return -1;
    }
    g_server_sock = sock;
    atomic_store(&g_net_status, NET_LOGGING_IN);
    set_status(client_classic() ? "Authenticating (classic server)..." : "Authenticating...");

    MsgHeader hdr = client_hdr(MSG_LOGIN, (int)sizeof(MsgLogin));
    MsgLogin m;
    memset(&m, 0, sizeof(m));
    copy_str(m.username, s_info.user, sizeof(m.username));
    copy_str(m.password, s_info.pass, sizeof(m.password));
    copy_str(m.server_pass, s_info.server_pass, sizeof(m.server_pass));
    m.is_new_char = s_info.is_new ? 1 : 0;
    m.race_id = s_info.race_id;
    m.subrace_id = s_info.subrace_id;
    m.class_id = s_info.class_id;
    m.alignment = s_info.alignment;
    /*Cosmetic flavour rolls (the server rolls the real ability scores)*/
    m.age = rules_roll_dice(3, 6) + 15;
    m.height = rules_roll_dice(4, 10) + 140;
    m.weight = rules_roll_dice(4, 10) + 50;
    m.social_class = rules_roll_dice(1, 100);
    pthread_mutex_lock(&g_net_mutex);
    bool ok = net_send(sock, &hdr, sizeof(hdr)) && net_send(sock, &m, sizeof(m));
    pthread_mutex_unlock(&g_net_mutex);
    if (!ok) {
        pthread_mutex_lock(&g_state_mutex);
        copy_str(g_net_error, "Could not send the login.", sizeof(g_net_error));
        pthread_mutex_unlock(&g_state_mutex);
        atomic_store(&g_net_status, NET_FAILED);
        return -1;
    }
    copy_str(g_my_name, s_info.user, sizeof(g_my_name));
    return sock;
}

static void *session_thread(void *arg) {
    (void)arg;
    long long deadline = plat_time_ms() + (s_info.retry_ms > 0 ? s_info.retry_ms : 0);
    g_proto_version = PROTOCOL_VERSION;
    if (connect_and_login(deadline) < 0) return NULL;
    net_thread_loop(NULL);
    /*An original Dragon GL server drops a connection whose header carries
     * another protocol version without answering at all. Closed before
     * the first byte arrived => try once more speaking its protocol.*/
    if (!atomic_load(&s_stop) && !g_net_rx_any && atomic_load(&g_net_status) == NET_DISCONNECTED && !s_info.no_classic) {
        pthread_mutex_lock(&g_net_mutex);
        net_close(g_server_sock);
        g_server_sock = -1;
        pthread_mutex_unlock(&g_net_mutex);
        g_proto_version = PROTO_CLASSIC;
        if (connect_and_login(plat_time_ms()) < 0) return NULL;
        net_thread_loop(NULL);
        if (!g_net_rx_any && atomic_load(&g_net_status) == NET_DISCONNECTED) {
            pthread_mutex_lock(&g_state_mutex);
            copy_str(g_net_error, "The server refused the connection (incompatible version or server full).",
                     sizeof(g_net_error));
            pthread_mutex_unlock(&g_state_mutex);
            atomic_store(&g_net_status, NET_FAILED);
        }
    }
    return NULL;
}

bool session_start(const LoginInfo *info) {
    session_stop();
    s_info = *info;
    atomic_store(&s_stop, false);
    client_state_reset();
    g_net_error[0] = '\0';
    if (pthread_create(&s_thread, NULL, session_thread, NULL) != 0) return false;
    s_thread_live = true;
    return true;
}

void session_stop(void) {
    if (!s_thread_live) return;
    atomic_store(&s_stop, true);
    if (g_server_sock >= 0) {
        /*shutdown wakes the receive loop immediately*/
#ifdef _WIN32
        shutdown(g_server_sock, SD_BOTH);
#else
        shutdown(g_server_sock, SHUT_RDWR);
#endif
    }
    pthread_join(s_thread, NULL);
    s_thread_live = false;
    if (g_server_sock >= 0) {
        pthread_mutex_lock(&g_net_mutex);
        net_close(g_server_sock);
        g_server_sock = -1;
        pthread_mutex_unlock(&g_net_mutex);
    }
    atomic_store(&g_net_status, NET_IDLE);
}

/* ------------------------------------------------------------------ */
/* Solo server                                                         */
/* ------------------------------------------------------------------ */

static PlatProcess s_proc;
static char s_dir[1024];
static char s_log[1100];
static char s_stop_file[1100];

const char *local_server_dir(void) {
    if (!s_dir[0]) {
        char base[900];
        if (!plat_user_dir(base, sizeof(base))) snprintf(base, sizeof(base), ".");
        snprintf(s_dir, sizeof(s_dir), "%s/solo", base);
        plat_mkdirs(s_dir);
    }
    return s_dir;
}

static bool find_file(char *out, size_t cap, const char *const *cands, int n) {
    for (int i = 0; i < n; i++) {
        if (cands[i] && plat_path_exists(cands[i])) {
            snprintf(out, cap, "%s", cands[i]);
            return true;
        }
    }
    return false;
}

bool local_server_start(int port, const char *password, char *err, int err_len) {
    if (local_server_running()) return true;
    char exe_dir[900];
    if (!plat_exe_dir(exe_dir, sizeof(exe_dir))) snprintf(exe_dir, sizeof(exe_dir), ".");
#ifdef _WIN32
    const char *exe_name = "dragongl_server.exe";
#else
    const char *exe_name = "dragongl_server";
#endif
    char c1[1024], c2[1024], c3[1024];
    snprintf(c1, sizeof(c1), "%s/%s", exe_dir, exe_name);
    snprintf(c2, sizeof(c2), "%s/../build/%s", exe_dir, exe_name);
    snprintf(c3, sizeof(c3), "%s/../bin/%s", exe_dir, exe_name);
    const char *exe_c[] = {c1, c2, c3};
    char exe[1024];
    if (!find_file(exe, sizeof(exe), exe_c, 3)) {
        snprintf(err, (size_t)err_len, "Server program not found next to the client (%s).", exe_name);
        return false;
    }
    char d1[1024], d2[1024], d3[1024], d4[1024];
    snprintf(d1, sizeof(d1), "%s/data/bestiary.json", exe_dir);
    snprintf(d2, sizeof(d2), "%s/../data/bestiary.json", exe_dir);
    snprintf(d3, sizeof(d3), "%s/../share/dragongl/data/bestiary.json", exe_dir);
    snprintf(d4, sizeof(d4), "/usr/share/dragongl/data/bestiary.json");
    const char *data_c[] = {d1, d2, d3, d4};
    char data[1024];
    if (!find_file(data, sizeof(data), data_c, 4)) {
        snprintf(err, (size_t)err_len, "Game data (data/bestiary.json) not found.");
        return false;
    }
    *strrchr(data, '/') = '\0';
    const char *dir = local_server_dir();
    char state[1100];
    snprintf(state, sizeof(state), "%s/world", dir);
    plat_mkdirs(state);
    snprintf(s_log, sizeof(s_log), "%s/server.log", dir);
    snprintf(s_stop_file, sizeof(s_stop_file), "%s/stop.request", dir);
    remove(s_stop_file);
    char port_s[16];
    snprintf(port_s, sizeof(port_s), "%d", port);
    char pw[64];
    snprintf(pw, sizeof(pw), "%s", password);
    char *argv[] = {exe, "--local", "-P", port_s, "-p", pw, "-D", data, "-S", state,
                    "--stop-file", s_stop_file, NULL};
    if (!plat_spawn(&s_proc, exe, argv, dir, s_log)) {
        snprintf(err, (size_t)err_len, "Could not start the server program.");
        return false;
    }
    return true;
}

bool local_server_running(void) { return plat_process_alive(&s_proc); }

void local_server_stop(void) {
    if (!plat_process_alive(&s_proc)) return;
    /*Graceful: the stop file makes the server save every player and
     * exit; forced termination after the timeout.*/
    FILE *f = fopen(s_stop_file, "w");
    if (f) fclose(f);
    plat_process_stop(&s_proc, 4000);
    remove(s_stop_file);
}

void local_server_log_tail(char *out, int cap) {
    out[0] = '\0';
    FILE *f = fopen(s_log, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    long start = sz > cap - 1 ? sz - (cap - 1) : 0;
    fseek(f, start, SEEK_SET);
    size_t n = fread(out, 1, (size_t)(cap - 1), f);
    out[n] = '\0';
    fclose(f);
}
