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

#include "net.h"
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifndef _WIN32
#include <unistd.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/select.h>
#endif

int net_create_server(int port) {
    return net_create_server_ex(port, false);
}

int net_create_server_ex(int port, bool loopback_only) {
    int sock;
    struct sockaddr_in addr;
    int opt;
    
    plat_net_init();
    sock = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return -1;
    }
    
    opt = 1;
#ifdef _WIN32
    /*SO_REUSEADDR on Windows lets ANOTHER process steal the port: use
     * the exclusive flavour instead.*/
    setsockopt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&opt, sizeof(opt));
#else
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif
    
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = loopback_only ? htonl(INADDR_LOOPBACK) : INADDR_ANY;
    addr.sin_port = htons(port);
    
    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        plat_sock_close(sock);
        return -1;
    }
    
    if (listen(sock, 10) < 0) {
        plat_sock_close(sock);
        return -1;
    }
    
    return sock;
}

int net_connect_to_server(const char *ip, int port) {
    /*getaddrinfo instead of inet_pton: the player can now type a host
     * NAME (e.g. "myserver.lan") as well as a dotted IPv4 address.*/
    struct addrinfo hints, *res = NULL, *ai;
    char port_str[16];
    int sock = -1;

    plat_net_init();
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(port_str, sizeof(port_str), "%d", port);
    if (getaddrinfo(ip, port_str, &hints, &res) != 0 || !res) {
        return -1;
    }
    for (ai = res; ai; ai = ai->ai_next) {
        sock = (int)socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock < 0) continue;
        if (connect(sock, ai->ai_addr, (int)ai->ai_addrlen) == 0) {
            /*Small, latency-sensitive messages (moves, commands): no
             * Nagle batching.*/
            int one = 1;
            setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
            break;
        }
        plat_sock_close(sock);
        sock = -1;
    }
    freeaddrinfo(res);
    return sock;
}

void net_set_nonblocking(int sock) {
    plat_sock_set_nonblocking(sock);
}

/*True if the send()/recv() error is TRANSIENT (retry after a short
 * pause). Everything else (EPIPE, ENOTCONN, ECONNRESET, EBADF, ...) is
 * permanent: the peer is gone or the fd is invalid, and retrying would
 * only spin the caller for the whole retry budget (~1 s) for nothing.
 * Callers that must drop a client on the first failure (server
 * net_send_client) rely on this distinction.*/
static bool net_err_is_transient(void) {
    return plat_sock_would_block();
}

bool net_send(int sock, const void *data, int len) {
    int total = 0;
    int retries = 0;
    /*100 retries * 10ms = at most ~1s of blocking on a SLOW peer
     * (buffer full, EAGAIN) before the caller drops the send (and may
     * disconnect the client). This used to be 500 (5s): a single
     * slow/stuck client could stall the whole server loop for several
     * seconds. (Full per-client send buffers with backpressure would
     * remove the blocking entirely; this cap keeps the worst case sane
     * until then.) Permanent errors (dead peer, closed fd) fail
     * IMMEDIATELY: a failed send must be visible to the caller in the
     * same tick, not a second later.*/
    int max_retries = 100;
    const char *buf = (const char *)data;
    if (sock < 0)
        return false;

    while (total < len && retries < max_retries) {
        int sent = (int)send(sock, buf + total, len - total, MSG_NOSIGNAL);
        if (sent > 0) {
            total += sent;
            retries = 0;
        } else if (sent == 0) {
            return false;
        } else {
            if (!net_err_is_transient())
                return false; /*peer closed / bad fd: don't spin ~1s*/
            retries++;
            plat_sleep_ms(10);
        }
    }

    return total == len;
}

int net_receive(int sock, void *buffer, int max_len) {
    int bytes;
    
    bytes = (int)recv(sock, buffer, max_len, 0);
    
    return bytes;
}

/*Read exactly 'len' bytes, retrying on EAGAIN with short pauses.
   Returns len on success, -1 on peer close or retry exhaustion.
   Permanent errors (EPIPE, ENOTCONN, EBADF, ...) return -1 at once
   instead of spinning the whole retry budget (~5 s).*/
int net_receive_exact(int sock, void *buffer, int len) {
    int total = 0;
    int retries = 0;
    int max_retries = 500;
    char *buf = (char *)buffer;

    while (total < len && retries < max_retries) {
        int bytes = (int)recv(sock, buf + total, len - total, 0);
        if (bytes > 0) {
            total += bytes;
            retries = 0;
        } else if (bytes == 0) {
            return -1; /* peer closed */
        } else {
            if (!net_err_is_transient())
                return -1;
            retries++;
            plat_sleep_ms(10);
        }
    }

    return (total == len) ? total : -1;
}

/*Always-NUL-terminated string copy (see net.h).
 * The NUL goes right after the copied bytes (position n), so a string
 * that SHRINKS between two calls cannot leave the old tail behind —
 * setting only dst[cap-1] would do exactly that.*/
void copy_str(char *dst, const char *src, size_t cap) {
    if (cap == 0)
        return;
    size_t n = 0;
    if (src) {
        n = strlen(src);
        if (n >= cap)
            n = cap - 1;
        memcpy(dst, src, n);
    }
    dst[n] = '\0';
}

int net_wait_readable(int sock, int timeout_ms) {
    fd_set rf;
    struct timeval tv;
    if (sock < 0) return -1;
    FD_ZERO(&rf);
    FD_SET(sock, &rf);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    int r = select(sock + 1, &rf, NULL, NULL, &tv);
    if (r < 0) return plat_sock_would_block() ? 0 : -1;
    return r > 0 ? 1 : 0;
}

bool net_peek_closed(int sock) {
    char c;
    int r = (int)recv(sock, &c, 1, MSG_PEEK);
    if (r == 0) return true;
    if (r < 0) return !plat_sock_would_block();
    return false;
}

int net_connect_timeout(const char *host, int port, int timeout_ms, char *err, size_t err_len) {
    struct addrinfo hints, *res = NULL, *ai;
    char port_str[16];
    int sock = -1;

    plat_net_init();
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(port_str, sizeof(port_str), "%d", port);
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
        if (err) snprintf(err, err_len, "Unknown host '%s'.", host);
        return -1;
    }
    if (err) snprintf(err, err_len, "Cannot reach %s:%d (no server listening?).", host, port);
    for (ai = res; ai; ai = ai->ai_next) {
        sock = (int)socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock < 0) continue;
        plat_sock_set_nonblocking(sock);
        int r = connect(sock, ai->ai_addr, (int)ai->ai_addrlen);
        bool ok = (r == 0);
        if (!ok) {
#ifdef _WIN32
            bool pending = WSAGetLastError() == WSAEWOULDBLOCK;
#else
            bool pending = errno == EINPROGRESS;
#endif
            if (pending) {
                fd_set wf, ef;
                struct timeval tv;
                FD_ZERO(&wf);
                FD_ZERO(&ef);
                FD_SET(sock, &wf);
                FD_SET(sock, &ef);
                tv.tv_sec = timeout_ms / 1000;
                tv.tv_usec = (timeout_ms % 1000) * 1000;
                if (select(sock + 1, NULL, &wf, &ef, &tv) > 0 && FD_ISSET(sock, &wf)) {
                    int so_err = 0;
                    socklen_t len = sizeof(so_err);
                    getsockopt(sock, SOL_SOCKET, SO_ERROR, (char *)&so_err, &len);
                    ok = (so_err == 0);
                } else if (err) {
                    snprintf(err, err_len, "Connection to %s:%d timed out.", host, port);
                }
            }
        }
        if (ok) {
            int one = 1;
            setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
            break;
        }
        plat_sock_close(sock);
        sock = -1;
    }
    freeaddrinfo(res);
    return sock; /*still NON-BLOCKING*/
}

void net_close(int sock) {
    plat_sock_close(sock);
}

/*Read exactly 'len' bytes on a non-blocking socket,
   trying again with short pauses up to a maximum of 500 iterations (~5 sec).
   Permanent errors (EPIPE, ENOTCONN, EBADF, ...) return -1 at once
   instead of spinning the whole budget.*/
int net_receive_all(int sock, void *buffer, int len) {
    int total = 0;
    int retries = 0;
    int max_retries = 500;
    char *buf = (char *)buffer;
    
    while (total < len && retries < max_retries) {
        int bytes = (int)recv(sock, buf + total, len - total, 0);
        if (bytes > 0) {
            total = total + bytes;
            retries = 0;
        } else if (bytes == 0) {
            /* connection closed */
            return -1;
        } else {
            /* EAGAIN / EWOULDBLOCK (retry); anything else is a dead peer */
            if (!net_err_is_transient())
                return -1;
            retries = retries + 1;
            plat_sleep_ms(10);
        }
    }
    
    if (total == len) {
        return total;
    }
    return -1;
}

