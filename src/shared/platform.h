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

/*Platform layer (Linux / Windows).
 *
 * The code base was written against POSIX. Instead of sprinkling
 * #ifdef _WIN32 over 30k lines, the Windows build FORCE-INCLUDES this
 * header in every translation unit (CMake: -include platform.h), so the
 * POSIX spellings used everywhere (mkdir(path, mode), fsync, rename over
 * an existing file, strcasestr, usleep, rand() with a 31-bit range ...)
 * map onto their Win32 equivalents. On Linux it only declares the small
 * plat_* API below, which both platforms implement in platform.c.*/

#ifndef DRAGONGL_PLATFORM_H
#define DRAGONGL_PLATFORM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
/*winsock2.h MUST precede windows.h (otherwise the legacy winsock.h is
 * pulled in and the two clash).*/
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
#  include <io.h>
#  include <direct.h>
#  include <process.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>

/*POSIX mkdir(path, mode) -> _mkdir(path)*/
#  ifdef mkdir
#    undef mkdir
#  endif
#  define mkdir(p, m) _mkdir(p)
/*fsync(fd) -> _commit(fd) (flushes the CRT handle to disk)*/
#  define fsync(fd) _commit(fd)
/*rename() over an existing file FAILS on Windows: every atomic save
 * (temp file + rename) relies on POSIX "replace" semantics.*/
int plat_rename_replace(const char *from, const char *to);
#  define rename(a, b) plat_rename_replace((a), (b))
/*MSVCRT's rand() is 15-bit (RAND_MAX 32767): dice code written for
 * glibc (31-bit) would lose range. Use the portable 31-bit generator.*/
int  plat_rand(void);
void plat_srand(unsigned s);
#  undef RAND_MAX
#  define RAND_MAX 0x7fffffff
#  define rand() plat_rand()
#  define srand(s) plat_srand(s)
/*GNU extensions used by the code base*/
char *strcasestr(const char *haystack, const char *needle);
#  ifndef strtok_r
#    define strtok_r(s, d, p) strtok_s((s), (d), (p))
#  endif
#  define MSG_NOSIGNAL 0
#else
#  include <unistd.h>
#  include <sys/types.h>
#  include <sys/stat.h>
#endif

/*--- portable API (both platforms) ---------------------------------*/
/*Socket subsystem (WSAStartup on Windows, SIGPIPE ignore on Linux).
 * Idempotent; call once at program start.*/
bool plat_net_init(void);
/*Last socket error is transient (would block / interrupted)?*/
bool plat_sock_would_block(void);
/*Close a socket descriptor.*/
void plat_sock_close(int sock);
/*Make a socket non-blocking.*/
void plat_sock_set_nonblocking(int sock);
/*Sleep for ms milliseconds.*/
void plat_sleep_ms(int ms);
/*Monotonic clock in milliseconds.*/
long long plat_time_ms(void);
/*Fill buf with OS entropy; false if unavailable.*/
bool plat_random_bytes(void *buf, size_t len);
/*Directory of the running executable (no trailing slash).*/
bool plat_exe_dir(char *out, size_t cap);
/*Per-user writable directory for settings/saves of the client
 * (created if missing), e.g. ~/.config/dragongl or %APPDATA%\DragonGL.*/
bool plat_user_dir(char *out, size_t cap);
/*Does a file/dir exist?*/
bool plat_path_exists(const char *path);
/*Create a directory (and parents). true if it exists afterwards.*/
bool plat_mkdirs(const char *path);

/*Child process handle (local "solo" server spawned by the client).*/
typedef struct {
#ifdef _WIN32
    void *process; /*HANDLE*/
#else
    int pid;
#endif
    bool running;
} PlatProcess;
/*Spawn exe with argv (argv[0] = exe, NULL-terminated), working
 * directory cwd, stdout/stderr redirected to log_path (may be NULL).*/
bool plat_spawn(PlatProcess *p, const char *exe, char *const argv[],
                const char *cwd, const char *log_path);
bool plat_process_alive(PlatProcess *p);
/*Ask the process to terminate (graceful first, forced after timeout).*/
void plat_process_stop(PlatProcess *p, int timeout_ms);

#endif /*DRAGONGL_PLATFORM_H*/
