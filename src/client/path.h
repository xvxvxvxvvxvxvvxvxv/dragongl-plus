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

#ifndef PATH_H
#define PATH_H

#include <stdbool.h>
#include <stdint.h>
#include "map.h"

/*8-way A* from (sx,sy) to (tx,ty). occupied: optional W*H mask of
 * blocking entities. stop_adjacent: end next to the target (to attack or
 * talk to it). Writes x,y pairs (excluding the start) into out; returns
 * the number of steps (0 = no path).*/
int path_find(const TileType map[MAP_HEIGHT][MAP_WIDTH], const uint8_t *occupied,
              int sx, int sy, int tx, int ty, bool stop_adjacent, int *out, int max);

#endif
