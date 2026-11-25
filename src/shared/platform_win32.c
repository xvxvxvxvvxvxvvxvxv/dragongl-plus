/*
 * DragonGL+ - the POSIX calls of the server on Windows (platform_win32.h)
 * Copyright (C) 2026 Nicola Taibi - GPL-3.0-or-later
 *
 * Built only on Windows, with DRAGONGL_NO_POSIX_SHIM: the calls below are
 * the real Winsock / CRT ones.
 */
#ifdef _WIN32

#include "platform.h"
#include "platform_win32.h"

#include <bcrypt.h>
#include <io.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* --- Registry of the descriptors that are not CRT files ---------------- */

enum { FD_SOCKET = 1, FD_EVENT = 2, FD_EPOLL = 3 };
typedef struct {
    int fd;
    int kind;
    int peer;      /* FD_EVENT: the write end */
    HANDLE epoll;  /* FD_EPOLL */
} FdEntry;
#define FD_REGISTRY_MAX 1024
static FdEntry g_fds[FD_REGISTRY_MAX];
static pthread_mutex_t g_fds_lock = PTHREAD_MUTEX_INITIALIZER;

static void fd_register(int fd, int kind, int peer, HANDLE ep) {
    pthread_mutex_lock(&g_fds_lock);
    for (int i = 0; i < FD_REGISTRY_MAX; i++) {
        if (g_fds[i].kind == 0) {
            g_fds[i] = (FdEntry){fd, kind, peer, ep};
            break;
        }
    }
    pthread_mutex_unlock(&g_fds_lock);
}

static FdEntry fd_lookup(int fd, bool remove) {
    FdEntry e = {0, 0, 0, NULL};
    pthread_mutex_lock(&g_fds_lock);
    for (int i = 0; i < FD_REGISTRY_MAX; i++) {
        if (g_fds[i].kind != 0 && g_fds[i].fd == fd) {
            e = g_fds[i];
            if (remove) {
                g_fds[i].kind = 0;
            }
            break;
        }
    }
    pthread_mutex_unlock(&g_fds_lock);
    return e;
}

/* Winsock error -> errno */
static int map_wsa(int w) {
    switch (w) {
    case WSAEWOULDBLOCK:
        return EAGAIN;
    case WSAEINPROGRESS:
    case WSAEALREADY:
        return EINPROGRESS;
    case WSAEINTR:
        return EINTR;
    case WSAECONNRESET:
        return ECONNRESET;
    case WSAECONNABORTED:
        return ECONNABORTED;
    case WSAENOTCONN:
        return ENOTCONN;
    case WSAESHUTDOWN:
        return EPIPE;
    case WSAECONNREFUSED:
        return ECONNREFUSED;
    case WSAETIMEDOUT:
        return ETIMEDOUT;
    case WSAEADDRINUSE:
        return EADDRINUSE;
    case WSAEADDRNOTAVAIL:
        return EADDRNOTAVAIL;
    case WSAEAFNOSUPPORT:
        return EAFNOSUPPORT;
    case WSAENETUNREACH:
    case WSAEHOSTUNREACH:
        return ENETUNREACH;
    case WSAEMFILE:
        return EMFILE;
    case WSAENOBUFS:
        return ENOBUFS;
    case WSAEINVAL:
        return EINVAL;
    case 0:
        return 0;
    default:
        return EIO;
    }
}

static int fail_wsa(void) {
    errno = map_wsa(WSAGetLastError());
    return -1;
}

/* --- Sockets ------------------------------------------------------------- */

int plat_w_socket(int af, int type, int protocol) {
    plat_net_init();
    bool nonblock = (type & SOCK_NONBLOCK) != 0;
    type &= ~(SOCK_NONBLOCK | SOCK_CLOEXEC);
    SOCKET s = socket(af, type, protocol);
    if (s == INVALID_SOCKET) {
        return fail_wsa();
    }
    if (nonblock) {
        u_long on = 1;
        ioctlsocket(s, FIONBIO, &on);
    }
    fd_register((int)s, FD_SOCKET, 0, NULL);
    return (int)s;
}

int plat_w_accept(int fd, struct sockaddr *addr, int *len) {
    SOCKET s = accept((SOCKET)fd, addr, len);
    if (s == INVALID_SOCKET) {
        return fail_wsa();
    }
    fd_register((int)s, FD_SOCKET, 0, NULL);
    return (int)s;
}

int plat_w_close(int fd) {
    FdEntry e = fd_lookup(fd, true);
    switch (e.kind) {
    case FD_SOCKET:
        return closesocket((SOCKET)fd) == 0 ? 0 : fail_wsa();
    case FD_EVENT:
        closesocket((SOCKET)e.peer);
        return closesocket((SOCKET)fd) == 0 ? 0 : fail_wsa();
    case FD_EPOLL:
        return epoll_close(e.epoll);
    default:
        return _close(fd);
    }
}

long plat_w_send(int fd, const void *buf, size_t n, int flags) {
    int r = send((SOCKET)fd, (const char *)buf, (int)n, flags);
    return r == SOCKET_ERROR ? fail_wsa() : r;
}

long plat_w_recv(int fd, void *buf, size_t n, int flags) {
    int r = recv((SOCKET)fd, (char *)buf, (int)n, flags);
    return r == SOCKET_ERROR ? fail_wsa() : r;
}

long plat_w_read(int fd, void *buf, size_t n) {
    FdEntry e = fd_lookup(fd, false);
    if (e.kind == FD_EVENT) {
        /* eventfd: drain the wake-up bytes, report a counter of 1 */
        char sink[64];
        bool any = false;
        while (recv((SOCKET)fd, sink, sizeof(sink), 0) > 0) {
            any = true;
        }
        if (!any) {
            errno = EAGAIN;
            return -1;
        }
        uint64_t one = 1;
        memcpy(buf, &one, n < sizeof(one) ? n : sizeof(one));
        return (long)(n < sizeof(one) ? n : sizeof(one));
    }
    if (e.kind == FD_SOCKET) {
        return plat_w_recv(fd, buf, n, 0);
    }
    return _read(fd, buf, (unsigned)n);
}

long plat_w_write(int fd, const void *buf, size_t n) {
    FdEntry e = fd_lookup(fd, false);
    if (e.kind == FD_EVENT) {
        char b = 1;
        send((SOCKET)e.peer, &b, 1, 0);
        return (long)n;
    }
    if (e.kind == FD_SOCKET) {
        return plat_w_send(fd, buf, n, 0);
    }
    return _write(fd, buf, (unsigned)n);
}

int plat_w_connect(int fd, const struct sockaddr *addr, int len) {
    if (connect((SOCKET)fd, addr, len) == 0) {
        return 0;
    }
    int w = WSAGetLastError();
    errno = (w == WSAEWOULDBLOCK) ? EINPROGRESS : map_wsa(w);
    return -1;
}

int plat_w_getsockopt(int fd, int level, int name, void *val, int *len) {
    int r = getsockopt((SOCKET)fd, level, name, (char *)val, len);
    if (r != 0) {
        return fail_wsa();
    }
    if (level == SOL_SOCKET && name == SO_ERROR && val && *len >= (int)sizeof(int)) {
        int *err = (int *)val;
        *err = map_wsa(*err);
    }
    return 0;
}

int plat_w_setsockopt(int fd, int level, int name, const void *val, int len) {
    return setsockopt((SOCKET)fd, level, name, (const char *)val, len) == 0 ? 0 : fail_wsa();
}

int plat_w_fcntl(int fd, int cmd, ...) {
    va_list ap;
    va_start(ap, cmd);
    long arg = (cmd == F_SETFL || cmd == F_SETFD) ? va_arg(ap, long) : 0;
    va_end(ap);
    if (cmd == F_SETFL && fd_lookup(fd, false).kind != 0) {
        u_long on = (arg & O_NONBLOCK) ? 1 : 0;
        return ioctlsocket((SOCKET)fd, FIONBIO, &on) == 0 ? 0 : fail_wsa();
    }
    return 0; /* F_GETFL / F_GETFD / F_SETFD: nothing to report or change */
}

int plat_w_poll(struct pollfd *fds, unsigned long n, int timeout_ms) {
    int r = WSAPoll(fds, n, timeout_ms);
    return r == SOCKET_ERROR ? fail_wsa() : r;
}

/* --- eventfd: a loopback socket pair ------------------------------------- */

int plat_w_eventfd(unsigned int initval, int flags) {
    (void)flags;
    plat_net_init();
    SOCKET l = socket(AF_INET, SOCK_STREAM, 0), w = INVALID_SOCKET, r = INVALID_SOCKET;
    struct sockaddr_in a;
    int alen = sizeof(a);
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (l == INVALID_SOCKET || bind(l, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(l, 1) != 0 ||
        getsockname(l, (struct sockaddr *)&a, &alen) != 0) {
        goto fail;
    }
    w = socket(AF_INET, SOCK_STREAM, 0);
    if (w == INVALID_SOCKET || connect(w, (struct sockaddr *)&a, sizeof(a)) != 0) {
        goto fail;
    }
    r = accept(l, NULL, NULL);
    if (r == INVALID_SOCKET) {
        goto fail;
    }
    closesocket(l);
    u_long on = 1;
    ioctlsocket(r, FIONBIO, &on);
    fd_register((int)r, FD_EVENT, (int)w, NULL);
    if (initval) {
        char b = 1;
        send(w, &b, 1, 0);
    }
    return (int)r;
fail:
    fail_wsa();
    if (l != INVALID_SOCKET) closesocket(l);
    if (w != INVALID_SOCKET) closesocket(w);
    return -1;
}

/* --- epoll (wepoll) ----------------------------------------------------- */

int plat_w_epoll_create1(int flags) {
    (void)flags;
    plat_net_init();
    HANDLE h = epoll_create1(0);
    if (!h) {
        errno = ENOMEM;
        return -1;
    }
    /* Kernel handles are 32-bit significant (Win32 interop rule). */
    int fd = (int)(intptr_t)h;
    fd_register(fd, FD_EPOLL, 0, h);
    return fd;
}

static HANDLE epoll_handle(int epfd) {
    FdEntry e = fd_lookup(epfd, false);
    return e.kind == FD_EPOLL ? e.epoll : NULL;
}

int plat_w_epoll_ctl(int epfd, int op, int fd, struct epoll_event *ev) {
    HANDLE h = epoll_handle(epfd);
    if (!h) {
        errno = EBADF;
        return -1;
    }
    return epoll_ctl(h, op, (SOCKET)fd, ev);
}

int plat_w_epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout) {
    HANDLE h = epoll_handle(epfd);
    if (!h) {
        errno = EBADF;
        return -1;
    }
    int r = epoll_wait(h, events, maxevents, timeout);
    if (r < 0) {
        errno = EINTR;
    }
    return r;
}

/* --- flock --------------------------------------------------------------- */

int plat_w_flock(int fd, int op) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }
    OVERLAPPED ov;
    memset(&ov, 0, sizeof(ov));
    if (op & LOCK_UN) {
        return UnlockFileEx(h, 0, 1, 0, &ov) ? 0 : -1;
    }
    DWORD f = (op & LOCK_EX) ? LOCKFILE_EXCLUSIVE_LOCK : 0;
    if (op & LOCK_NB) {
        f |= LOCKFILE_FAIL_IMMEDIATELY;
    }
    if (!LockFileEx(h, f, 0, 1, 0, &ov)) {
        errno = EWOULDBLOCK;
        return -1;
    }
    return 0;
}

/* --- signals ------------------------------------------------------------- */

int plat_w_sigaction(int sig, const struct sigaction *sa, struct sigaction *old) {
    if (old) {
        memset(old, 0, sizeof(*old));
    }
    if (sig == SIGPIPE || !sa) {
        return 0; /* no SIGPIPE on Windows */
    }
    return signal(sig, sa->sa_handler) == SIG_ERR ? -1 : 0;
}

/* --- files, random, processes ------------------------------------------- */

long plat_w_getrandom(void *buf, size_t n, unsigned int flags) {
    (void)flags;
    return BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0
               ? (long)n
               : -1;
}

static void forward_slashes(char *s) {
    for (; *s; s++) {
        if (*s == '\\') {
            *s = '/';
        }
    }
}

long plat_w_readlink(const char *path, char *buf, size_t n) {
    if (strcmp(path, "/proc/self/exe") != 0 || n == 0) {
        errno = EINVAL;
        return -1;
    }
    DWORD len = GetModuleFileNameA(NULL, buf, (DWORD)n);
    if (len == 0 || len >= n) {
        errno = ENAMETOOLONG;
        return -1;
    }
    buf[len] = '\0';
    forward_slashes(buf);
    return (long)len; /* readlink() does not count a NUL */
}

char *plat_w_realpath(const char *path, char *resolved) {
    char *r = _fullpath(resolved, path, MAX_PATH);
    if (!r) {
        return NULL;
    }
    DWORD attr = GetFileAttributesA(r);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        if (!resolved) {
            free(r);
        }
        errno = ENOENT;
        return NULL;
    }
    forward_slashes(r);
    return r;
}

int plat_w_posix_spawn(void *pid, const char *path, const posix_spawn_file_actions_t *fa,
                       const posix_spawnattr_t *attr, char *const argv[], char *const envp[]) {
    (void)pid;
    (void)path;
    (void)fa;
    (void)attr;
    (void)argv;
    (void)envp;
    return ENOSYS; /* the DM's PDF maps need the Linux server */
}

int plat_w_waitpid(int pid, int *status, int options) {
    (void)pid;
    (void)options;
    if (status) {
        *status = 0;
    }
    errno = ECHILD;
    return -1;
}

#endif /* _WIN32 */
