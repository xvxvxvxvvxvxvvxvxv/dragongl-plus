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

/*Platform layer implementation — see platform.h.*/

#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "platform.h"

#include <errno.h>
#include <time.h>
#include <ctype.h>

#ifdef _WIN32
#include <bcrypt.h>
#include <shlobj.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/random.h>
#include <limits.h>
#endif

/* ===================================================================== */
#ifdef _WIN32
/* ===================================================================== */

#undef rand
#undef srand
#undef rename

static uint64_t s_rng_state = 0x9E3779B97F4A7C15ull;

int plat_rand(void) {
    /*splitmix64 -> 31 bits (same range as glibc's rand())*/
    uint64_t z = (s_rng_state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return (int)(z & 0x7fffffff);
}

void plat_srand(unsigned s) {
    s_rng_state = 0x9E3779B97F4A7C15ull ^ ((uint64_t)s * 0xD1B54A32D192ED03ull);
}

int plat_rename_replace(const char *from, const char *to) {
    if (MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return 0;
    errno = EACCES;
    return -1;
}

char *strcasestr(const char *h, const char *n) {
    if (!h || !n) return NULL;
    size_t nl = strlen(n);
    if (nl == 0) return (char *)h;
    for (; *h; h++) {
        if (_strnicmp(h, n, nl) == 0) return (char *)h;
    }
    return NULL;
}

bool plat_net_init(void) {
    static bool done = false;
    if (done) return true;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    done = true;
    return true;
}

bool plat_sock_would_block(void) {
    int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINTR || e == WSAEINPROGRESS;
}

void plat_sock_close(int sock) {
    if (sock >= 0) closesocket((SOCKET)sock);
}

void plat_sock_set_nonblocking(int sock) {
    u_long mode = 1;
    ioctlsocket((SOCKET)sock, FIONBIO, &mode);
}

void plat_sleep_ms(int ms) {
    Sleep((DWORD)(ms < 0 ? 0 : ms));
}

long long plat_time_ms(void) {
    return (long long)GetTickCount64();
}

bool plat_random_bytes(void *buf, size_t len) {
    return BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)len,
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

bool plat_exe_dir(char *out, size_t cap) {
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    char *s = strrchr(path, '\\');
    if (!s) s = strrchr(path, '/');
    if (s) *s = '\0';
    snprintf(out, cap, "%s", path);
    return true;
}

bool plat_user_dir(char *out, size_t cap) {
    char base[MAX_PATH];
    if (SHGetFolderPathA(NULL, CSIDL_APPDATA | CSIDL_FLAG_CREATE, NULL, 0, base) != S_OK)
        return false;
    snprintf(out, cap, "%s\\DragonGL", base);
    return plat_mkdirs(out);
}

bool plat_path_exists(const char *path) {
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

bool plat_mkdirs(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char c = *p;
            *p = '\0';
            if (!(p == tmp + 2 && tmp[1] == ':')) _mkdir(tmp);
            *p = c;
        }
    }
    _mkdir(tmp);
    return plat_path_exists(tmp);
}

bool plat_spawn(PlatProcess *p, const char *exe, char *const argv[],
                const char *cwd, const char *log_path) {
    char cmd[4096];
    size_t pos = 0;
    cmd[0] = '\0';
    for (int i = 0; argv[i]; i++) {
        int n = snprintf(cmd + pos, sizeof(cmd) - pos, "%s\"%s\"", i ? " " : "", argv[i]);
        if (n < 0 || (size_t)n >= sizeof(cmd) - pos) return false;
        pos += (size_t)n;
    }
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    HANDLE log = INVALID_HANDLE_VALUE;
    if (log_path) {
        SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
        log = CreateFileA(log_path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                          &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (log != INVALID_HANDLE_VALUE) {
            si.dwFlags |= STARTF_USESTDHANDLES;
            si.hStdOutput = log;
            si.hStdError = log;
            si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        }
    }
    BOOL ok = CreateProcessA(exe, cmd, NULL, NULL, TRUE,
                             CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP,
                             NULL, cwd, &si, &pi);
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    if (!ok) return false;
    CloseHandle(pi.hThread);
    p->process = pi.hProcess;
    p->running = true;
    return true;
}

bool plat_process_alive(PlatProcess *p) {
    if (!p->running) return false;
    DWORD code = 0;
    if (!GetExitCodeProcess((HANDLE)p->process, &code) || code != STILL_ACTIVE) {
        CloseHandle((HANDLE)p->process);
        p->running = false;
    }
    return p->running;
}

void plat_process_stop(PlatProcess *p, int timeout_ms) {
    if (!p->running) return;
    if (WaitForSingleObject((HANDLE)p->process, (DWORD)timeout_ms) != WAIT_OBJECT_0)
        TerminateProcess((HANDLE)p->process, 0);
    CloseHandle((HANDLE)p->process);
    p->running = false;
}

/* ===================================================================== */
#else /* POSIX */
/* ===================================================================== */

bool plat_net_init(void) {
    /*A send() on a socket whose peer already closed raises SIGPIPE,
     * whose default action KILLS the process: one client vanishing in
     * the middle of a broadcast used to be able to take the whole
     * server down. Ignore it; send() then just fails with EPIPE.*/
    signal(SIGPIPE, SIG_IGN);
    return true;
}

bool plat_sock_would_block(void) {
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}

void plat_sock_close(int sock) {
    if (sock >= 0) close(sock);
}

void plat_sock_set_nonblocking(int sock) {
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
}

void plat_sleep_ms(int ms) {
    struct timespec ts;
    if (ms < 0) ms = 0;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
    }
}

long long plat_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

bool plat_random_bytes(void *buf, size_t len) {
    return getrandom(buf, len, 0) == (ssize_t)len;
}

bool plat_exe_dir(char *out, size_t cap) {
    char path[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (n <= 0) return false;
    path[n] = '\0';
    char *s = strrchr(path, '/');
    if (s) *s = '\0';
    snprintf(out, cap, "%s", path);
    return true;
}

bool plat_user_dir(char *out, size_t cap) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (xdg && xdg[0])
        snprintf(out, cap, "%s/dragongl", xdg);
    else if (home && home[0])
        snprintf(out, cap, "%s/.config/dragongl", home);
    else
        snprintf(out, cap, ".dragongl");
    return plat_mkdirs(out);
}

bool plat_path_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

bool plat_mkdirs(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
    return plat_path_exists(tmp);
}

bool plat_spawn(PlatProcess *p, const char *exe, char *const argv[],
                const char *cwd, const char *log_path) {
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        if (cwd && chdir(cwd) != 0) _exit(127);
        if (log_path) {
            int fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) {
                dup2(fd, 1);
                dup2(fd, 2);
                close(fd);
            }
        }
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            dup2(devnull, 0);
            close(devnull);
        }
        /*Own process group: a Ctrl-C in the client's terminal must not
         * hit the server first (the client stops it cleanly itself).*/
        setpgid(0, 0);
        execv(exe, argv);
        _exit(127);
    }
    p->pid = pid;
    p->running = true;
    return true;
}

bool plat_process_alive(PlatProcess *p) {
    if (!p->running) return false;
    int status;
    pid_t r = waitpid(p->pid, &status, WNOHANG);
    if (r == p->pid || r < 0) p->running = false;
    return p->running;
}

void plat_process_stop(PlatProcess *p, int timeout_ms) {
    if (!plat_process_alive(p)) return;
    kill(p->pid, SIGTERM);
    long long end = plat_time_ms() + timeout_ms;
    while (plat_time_ms() < end) {
        if (!plat_process_alive(p)) return;
        plat_sleep_ms(20);
    }
    kill(p->pid, SIGKILL);
    int status;
    waitpid(p->pid, &status, 0);
    p->running = false;
}

#endif
