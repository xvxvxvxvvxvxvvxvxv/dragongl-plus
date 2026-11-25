#version 450
#extension GL_GOOGLE_include_directive : require
// Multiplies the lit opaque scene by the ambient occlusion (blend: dst *
// src colour), before the transparent effects are drawn.
#include "common.glsl"
SAMPLER(0, sampler2D, u_ao);
LOC(0) in vec2 v_uv;
layout(location = 0) out vec4 o_col;
void main() { o_col = vec4(vec3(texture(u_ao, v_uv).r), 1.0); }
