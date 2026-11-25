#version 450
#extension GL_GOOGLE_include_directive : require
// 2D interface: pixel coordinates, top-left origin. p0.xy = screen size.
#include "common.glsl"
PUSH_BLOCK;
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_col;
LOC(0) out vec2 v_uv;
LOC(1) out vec4 v_col;
void main() {
    v_uv = a_uv;
    v_col = a_col;
    vec2 ndc = a_pos / pc.p0.xy * 2.0 - 1.0;
#ifndef VULKAN
    ndc.y = -ndc.y;
#endif
    gl_Position = vec4(ndc, 0.0, 1.0);
}
