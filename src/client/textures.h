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

/*Procedural material library: every surface texture of the game is
 * GENERATED at start-up (tileable noise, Voronoi, brick/plank patterns),
 * so the client ships no image assets at all. The result is one RGBA
 * texture array (one layer per material).*/
#ifndef TEXTURES_H
#define TEXTURES_H

#include <stdint.h>

enum {
    MAT_WHITE = 0,
    MAT_FLAGSTONE,
    MAT_BRICK,
    MAT_WALL_TOP,
    MAT_ROCK,
    MAT_COBBLE,
    MAT_GRASS,
    MAT_PLANK,
    MAT_WATER,
    MAT_LAVA,
    MAT_ICE,
    MAT_SAND,
    MAT_ASH,
    MAT_MUD,
    MAT_MARBLE,
    MAT_OBSIDIAN,
    MAT_GOLD_VEIN,
    MAT_CRYSTAL,
    MAT_MUSHROOM,
    MAT_DOOR,
    MAT_IRON,
    MAT_TRAP,
    MAT_STAIRS,
    MAT_SKIN,
    MAT_CLOTH,
    MAT_LEATHER,
    MAT_METAL,
    MAT_BONE,
    MAT_SCALES,
    MAT_FUR,
    MAT_SLIME,
    MAT_BARK,
    MAT_LEAF,
    MAT_GLOW,      /*soft round sprite (particles, halos), alpha*/
    MAT_SPARK,     /*four-point star sprite, alpha*/
    MAT_RUNE,      /*magic circle, alpha*/
    MAT_SHADOW,    /*blob shadow, alpha*/
    MAT_DIRT,
    MAT_ROOF,
    MAT_GOLD,
    MAT_PAPER,
    MAT_FLAME,     /*flame tongue sprite, alpha*/
    MAT_COUNT
};

#define MAT_TEX_SIZE 128

/*Generate all layers: returns a malloc'ed buffer of
 * MAT_COUNT * MAT_TEX_SIZE * MAT_TEX_SIZE * 4 bytes (layer-major).*/
uint8_t *textures_generate(void);

#endif
