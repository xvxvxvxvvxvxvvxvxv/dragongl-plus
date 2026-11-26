/*
 * DragonGL+ - the POSIX calls of the server on Windows
 * Copyright (C) 2026 Nicola Taibi - GPL-3.0-or-later
 *
 * Dragon GL's server is written for Linux. On Windows (MinGW-w64) this
 * header, pulled in by platform.h (force-included in every source of the
 * game), maps what it uses onto Win32 so that its files build unchanged:
 *
 *   sockets      socket/accept/close/read/write/send/recv/connect/fcntl
 *                go through plat_w_* wrappers: a registry tells sockets
 *                from CRT file descriptors (close() must be closesocket()
 *                for a socket) and Winsock errors become errno values
 *                (EAGAIN, EINPROGRESS, ECONNRESET...).
 *   epoll        wepoll (third_party/wepoll), level-triggered, sockets.
 *   eventfd      a connected loopback socket pair (epoll can watch it).
 *   flock        LockFileEx.
 *   signals      sigaction() over signal() (SIGINT and SIGTERM work in a
 *                console); SIGPIPE does not exist (MSG_NOSIGNAL is 0).
 *   getrandom    BCryptGenRandom; readlink("/proc/self/exe") is
 *                GetModuleFileName; realpath is _fullpath.
 *   posix_spawn  not available: the DM's PDF maps are Linux-only.
 *
 * The missing system headers (<sys/epoll.h>, <arpa/inet.h>, ...) are
 * empty files in src/shared/compat_win32/. Third-party sources that call
 * Winsock themselves are built with DRAGONGL_NO_POSIX_SHIM.
 */
#ifndef DRAGONGL_PLATFORM_WIN32_H
#define DRAGONGL_PLATFORM_WIN32_H

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "wepoll.h"

/* windows.h macros that clash with names in the game */
#undef near
#undef far
#undef interface

/* --- sockets ---------------------------------------------------------- */
#define SOCK_NONBLOCK 0x40000000
#define SOCK_CLOEXEC 0x20000000
#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0
#endif
#ifndef SHUT_RD
#define SHUT_RD SD_RECEIVE
#define SHUT_WR SD_SEND
#define SHUT_RDWR SD_BOTH
#endif
#ifndef TCP_KEEPIDLE
#define TCP_KEEPIDLE 3 /* Windows 10 1709+ */
#endif
#ifndef TCP_KEEPCNT
#define TCP_KEEPCNT 16
#endif
#ifndef TCP_KEEPINTVL
#define TCP_KEEPINTVL 17
#endif
#ifndef F_GETFL
#define F_GETFL 3
#define F_SETFL 4
#define F_GETFD 1
#define F_SETFD 2
#define FD_CLOEXEC 1
#endif
#ifndef O_NONBLOCK
#define O_NONBLOCK 0x4000
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0 /* opening a directory fails on Windows: no directory fsync */
#endif
#ifndef SIGPIPE
#define SIGPIPE 13
#endif

int plat_w_socket(int af, int type, int protocol);
int plat_w_accept(int fd, struct sockaddr *addr, int *len);
int plat_w_close(int fd);
long plat_w_read(int fd, void *buf, size_t n);
long plat_w_write(int fd, const void *buf, size_t n);
long plat_w_send(int fd, const void *buf, size_t n, int flags);
long plat_w_recv(int fd, void *buf, size_t n, int flags);
int plat_w_connect(int fd, const struct sockaddr *addr, int len);
int plat_w_getsockopt(int fd, int level, int name, void *val, int *len);
int plat_w_setsockopt(int fd, int level, int name, const void *val, int len);
int plat_w_fcntl(int fd, int cmd, ...);
int plat_w_poll(struct pollfd *fds, unsigned long n, int timeout_ms);

/* --- eventfd, epoll, flock --------------------------------------------- */
#define EFD_NONBLOCK 0x800
#define EFD_CLOEXEC 0x80000
int plat_w_eventfd(unsigned int initval, int flags);
#ifndef EPOLL_CLOEXEC
#define EPOLL_CLOEXEC 0
#endif
int plat_w_epoll_create1(int flags);
int plat_w_epoll_ctl(int epfd, int op, int fd, struct epoll_event *ev);
int plat_w_epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout);
#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8
int plat_w_flock(int fd, int op);

/* --- signals ------------------------------------------------------------ */
struct sigaction {
    void (*sa_handler)(int);
    int sa_mask;
    int sa_flags;
};
#define sigemptyset(set) ((void)(*(set) = 0), 0)
int plat_w_sigaction(int sig, const struct sigaction *sa, struct sigaction *old);

/* --- files, random, processes ----------------------------------------- */
#define GRND_NONBLOCK 1
long plat_w_getrandom(void *buf, size_t n, unsigned int flags);
long plat_w_readlink(const char *path, char *buf, size_t n);
char *plat_w_realpath(const char *path, char *resolved);
#ifndef WNOHANG
#define WNOHANG 1
#endif
#define WIFEXITED(s) 1
#define WEXITSTATUS(s) (s)
#define WIFSIGNALED(s) 0
#define WTERMSIG(s) 0

/* --- the C library's POSIX extras --------------------------------------- */
static inline struct tm *plat_w_localtime_r(const time_t *t, struct tm *out) {
    return localtime_s(out, t) == 0 ? out : NULL;
}
static inline struct tm *plat_w_gmtime_r(const time_t *t, struct tm *out) {
    return gmtime_s(out, t) == 0 ? out : NULL;
}
static inline int plat_w_strerror_r(int e, char *buf, size_t n) {
    return strerror_s(buf, n, e);
}
static inline int plat_w_unsetenv(const char *name) {
    char kv[256];
    snprintf(kv, sizeof(kv), "%s=", name);
    return _putenv(kv);
}
typedef int posix_spawn_file_actions_t;
typedef int posix_spawnattr_t;
int plat_w_posix_spawn(void *pid, const char *path, const posix_spawn_file_actions_t *fa,
                       const posix_spawnattr_t *attr, char *const argv[], char *const envp[]);
int plat_w_waitpid(int pid, int *status, int options);

#ifndef DRAGONGL_NO_POSIX_SHIM
#define socket(a, t, p) plat_w_socket((a), (t), (p))
#define accept(f, a, l) plat_w_accept((f), (a), (l))
#define close(f) plat_w_close(f)
#define read(f, b, n) plat_w_read((f), (b), (n))
#define write(f, b, n) plat_w_write((f), (b), (n))
#define send(f, b, n, fl) plat_w_send((f), (b), (n), (fl))
#define recv(f, b, n, fl) plat_w_recv((f), (b), (n), (fl))
#define connect(f, a, l) plat_w_connect((f), (a), (l))
#define getsockopt(f, l, n, v, s) plat_w_getsockopt((f), (l), (n), (v), (s))
#define setsockopt(f, l, n, v, s) plat_w_setsockopt((f), (l), (n), (v), (s))
#define fcntl plat_w_fcntl
#define poll(f, n, t) plat_w_poll((f), (n), (t))
#define eventfd(v, f) plat_w_eventfd((v), (f))
#define epoll_create1(f) plat_w_epoll_create1(f)
#define epoll_ctl(e, o, f, ev) plat_w_epoll_ctl((e), (o), (f), (ev))
#define epoll_wait(e, ev, m, t) plat_w_epoll_wait((e), (ev), (m), (t))
#define flock(f, o) plat_w_flock((f), (o))
#define sigaction(s, a, o) plat_w_sigaction((s), (a), (o))
#define getrandom(b, n, f) plat_w_getrandom((b), (n), (f))
#define readlink(p, b, n) plat_w_readlink((p), (b), (n))
#define realpath(p, r) plat_w_realpath((p), (r))
#define posix_spawn plat_w_posix_spawn
#define waitpid(p, s, o) plat_w_waitpid((p), (s), (o))
#define localtime_r(t, o) plat_w_localtime_r((t), (o))
#define gmtime_r(t, o) plat_w_gmtime_r((t), (o))
#define strerror_r(e, b, n) plat_w_strerror_r((e), (b), (n))
#define unsetenv(n) plat_w_unsetenv(n)
#define flockfile(f) _lock_file(f)
#define funlockfile(f) _unlock_file(f)
#endif

#endif
