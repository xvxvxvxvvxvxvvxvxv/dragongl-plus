#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"
SAMPLER(0, sampler2D, u_atlas);
LOC(0) in vec2 v_uv;
LOC(1) in vec4 v_col;
layout(location = 0) out vec4 o_col;
void main() { o_col = texture(u_atlas, v_uv) * v_col; }
