#version 450
#extension GL_GOOGLE_include_directive : require
// Torch shadow cube: distance to the light / its radius. The player who
// carries the torch casts no shadow (a column around the light).
#include "common.glsl"
#include "scene_ubo.glsl"
SAMPLER(1, sampler2DArray, u_tex);
LOC(0) in vec3 v_wpos;
LOC(1) in vec2 v_uv;
LOC(2) flat in float v_layer;
LOC(3) flat in int v_flags;
layout(location = 0) out float o_dist;
void main() {
    vec4 lp = lights[0];
    if ((v_flags & 16) != 0 || texture(u_tex, vec3(v_uv, v_layer)).a < 0.35) discard;
    if (length(v_wpos.xz - lp.xz) < 0.55 && v_wpos.y > 0.05) discard;
    o_dist = length(v_wpos - lp.xyz) / lp.w;
}
