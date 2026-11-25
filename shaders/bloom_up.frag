#version 450
#extension GL_GOOGLE_include_directive : require
// Bloom, upsampling step: 3x3 tent filter of the smaller level, added
// (blend ONE, ONE) onto the next bigger one. p0.xy = texel size of the
// source, p0.z = radius in texels, p0.w = weight.
#include "common.glsl"
SAMPLER(0, sampler2D, u_src);
PUSH_BLOCK;
LOC(0) in vec2 v_uv;
layout(location = 0) out vec4 o_col;

vec3 tap(vec2 o) { return texture(u_src, v_uv + o * pc.p0.xy * pc.p0.z).rgb; }

void main() {
    vec3 c = tap(vec2(0, 0)) * 4.0;
    c += (tap(vec2(-1, 0)) + tap(vec2(1, 0)) + tap(vec2(0, -1)) + tap(vec2(0, 1))) * 2.0;
    c += tap(vec2(-1, -1)) + tap(vec2(1, -1)) + tap(vec2(-1, 1)) + tap(vec2(1, 1));
    o_col = vec4(c * (pc.p0.w / 16.0), 1.0);
}
