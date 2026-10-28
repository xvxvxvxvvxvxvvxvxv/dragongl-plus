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

/*Client-side A* for click-to-move. The server stays authoritative: the
 * path is only a sequence of ordinary MSG_MOVE steps sent at the normal
 * movement cadence; any step the server refuses simply stops the walk.*/

#include "path.h"
#include "scene_internal.h"
#include <stdlib.h>
#include <string.h>

#define W MAP_WIDTH
#define H MAP_HEIGHT

static int g_cost[H][W];
static int g_from[H][W];
static uint32_t g_stamp[H][W];
static uint32_t g_cur;

typedef struct { int f, x, y; } Node;
static Node heap[W * H];
static int hn;

static void hpush(Node n) {
    if (hn >= (int)(sizeof(heap) / sizeof(heap[0]))) return;
    int i = hn++;
    heap[i] = n;
    while (i > 0) {
        int p = (i - 1) / 2;
        if (heap[p].f <= heap[i].f) break;
        Node t = heap[p]; heap[p] = heap[i]; heap[i] = t;
        i = p;
    }
}

static Node hpop(void) {
    Node r = heap[0];
    heap[0] = heap[--hn];
    int i = 0;
    for (;;) {
        int l = i * 2 + 1, rr = l + 1, m = i;
        if (l < hn && heap[l].f < heap[m].f) m = l;
        if (rr < hn && heap[rr].f < heap[m].f) m = rr;
        if (m == i) break;
        Node t = heap[m]; heap[m] = heap[i]; heap[i] = t;
        i = m;
    }
    return r;
}

static int heur(int x, int y, int tx, int ty) {
    int dx = abs(x - tx), dy = abs(y - ty);
    return 10 * (dx > dy ? dx : dy) + 4 * (dx < dy ? dx : dy);
}

int path_find(const TileType map[MAP_HEIGHT][MAP_WIDTH], const uint8_t *occupied,
              int sx, int sy, int tx, int ty, bool stop_adjacent, int *out, int max) {
    if (tx < 0 || ty < 0 || tx >= W || ty >= H) return 0;
    if (!stop_adjacent && !tile_walkable(map[ty][tx])) return 0;
    g_cur++;
    if (g_cur == 0) { memset(g_stamp, 0, sizeof(g_stamp)); g_cur = 1; }
    hn = 0;
    g_stamp[sy][sx] = g_cur;
    g_cost[sy][sx] = 0;
    g_from[sy][sx] = -1;
    hpush((Node){heur(sx, sy, tx, ty), sx, sy});
    int expanded = 0;
    int goal = -1;
    static const int DX[8] = {1, -1, 0, 0, 1, 1, -1, -1};
    static const int DY[8] = {0, 0, 1, -1, 1, -1, 1, -1};
    while (hn > 0 && expanded < 60000) {
        Node n = hpop();
        expanded++;
        int cx = n.x, cy = n.y;
        bool reached = stop_adjacent ? (abs(cx - tx) <= 1 && abs(cy - ty) <= 1 && !(cx == tx && cy == ty))
                                     : (cx == tx && cy == ty);
        if (stop_adjacent && cx == sx && cy == sy && abs(cx - tx) <= 1 && abs(cy - ty) <= 1) reached = true;
        if (reached) { goal = cy * W + cx; break; }
        for (int d = 0; d < 8; d++) {
            int nx = cx + DX[d], ny = cy + DY[d];
            if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
            if (!tile_walkable(map[ny][nx])) continue;
            if (occupied && occupied[ny * W + nx] && !(nx == tx && ny == ty)) continue;
            if (d >= 4) {
                /*no corner cutting through walls*/
                if (!tile_walkable(map[cy][nx]) || !tile_walkable(map[ny][cx])) continue;
            }
            int nc = g_cost[cy][cx] + (d < 4 ? 10 : 14);
            if (g_stamp[ny][nx] == g_cur && g_cost[ny][nx] <= nc) continue;
            g_stamp[ny][nx] = g_cur;
            g_cost[ny][nx] = nc;
            g_from[ny][nx] = cy * W + cx;
            hpush((Node){nc + heur(nx, ny, tx, ty), nx, ny});
        }
    }
    if (goal < 0) return 0;
    /*walk back*/
    int len = 0;
    int cur = goal;
    while (cur != -1 && cur != sy * W + sx) {
        len++;
        cur = g_from[cur / W][cur % W];
    }
    if (len > max) return 0;
    cur = goal;
    for (int i = len - 1; i >= 0; i--) {
        out[i * 2] = cur % W;
        out[i * 2 + 1] = cur / W;
        cur = g_from[cur / W][cur % W];
    }
    return len;
}
