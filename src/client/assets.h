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

/*Binary assets embedded in the client executable at build time
 * (cmake/embed.cmake turns each file into a C array), so the portable
 * packages need no font files or compiled shaders next to the binary.*/
#ifndef ASSETS_H
#define ASSETS_H

#include <stddef.h>

extern const unsigned char asset_font_body[];
extern const size_t asset_font_body_size;
extern const unsigned char asset_font_bold[];
extern const size_t asset_font_bold_size;
extern const unsigned char asset_font_title[];
extern const size_t asset_font_title_size;

/*Window icon, raw RGBA*/
extern const unsigned char asset_icon64[];
extern const size_t asset_icon64_size;
extern const unsigned char asset_icon32[];
extern const size_t asset_icon32_size;

/*SPIR-V for the Vulkan backend (4-byte aligned)*/
extern const unsigned char asset_spv_scene_vert[];
extern const size_t asset_spv_scene_vert_size;
extern const unsigned char asset_spv_scene_frag[];
extern const size_t asset_spv_scene_frag_size;
extern const unsigned char asset_spv_ui_vert[];
extern const size_t asset_spv_ui_vert_size;
extern const unsigned char asset_spv_ui_frag[];
extern const size_t asset_spv_ui_frag_size;
extern const unsigned char asset_spv_post_vert[];
extern const size_t asset_spv_post_vert_size;
extern const unsigned char asset_spv_post_frag[];
extern const size_t asset_spv_post_frag_size;

#endif
