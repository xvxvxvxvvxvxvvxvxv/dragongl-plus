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

/*The shaders of both renderers, embedded at build time from shaders/:
 * the GLSL text of every file (OpenGL compiles it, resolving #include
 * itself) and the SPIR-V of every stage (Vulkan).*/
#ifndef SHADERS_H
#define SHADERS_H

#include <stddef.h>

typedef struct {
    const char *name;          /*file name in shaders/, e.g. "scene.frag"*/
    const unsigned char *glsl; /*NUL-terminated text*/
    size_t glsl_size;
    const unsigned char *spv;  /*NULL for include files*/
    size_t spv_size;
} ShaderFile;

/*NULL when there is no such file.*/
const ShaderFile *shader_file(const char *name);

/*OpenGL source of a stage: "#version 330 core", then the file with its
 * #version / #extension lines dropped and every #include "x" replaced by
 * the text of x. Returns a malloc'ed string or NULL.*/
char *shader_gl_source(const char *name);

#endif
