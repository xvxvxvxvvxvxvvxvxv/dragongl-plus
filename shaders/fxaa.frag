#version 450
#extension GL_GOOGLE_include_directive : require
// FXAA (after Timothy Lottes' fast variant) on the tone-mapped image.
// p0.xy = texel size.
#include "common.glsl"
SAMPLER(0, sampler2D, u_src);
PUSH_BLOCK;
LOC(0) in vec2 v_uv;
layout(location = 0) out vec4 o_col;

void main() {
    vec2 r = pc.p0.xy;
    float nw = texture(u_src, v_uv + vec2(-1.0, -1.0) * r).a;
    float ne = texture(u_src, v_uv + vec2(1.0, -1.0) * r).a;
    float sw = texture(u_src, v_uv + vec2(-1.0, 1.0) * r).a;
    float se = texture(u_src, v_uv + vec2(1.0, 1.0) * r).a;
    vec4 m = texture(u_src, v_uv);
    float lmin = min(m.a, min(min(nw, ne), min(sw, se)));
    float lmax = max(m.a, max(max(nw, ne), max(sw, se)));
    if (lmax - lmin < max(0.0312, lmax * 0.125)) { o_col = vec4(m.rgb, 1.0); return; }
    vec2 dir = vec2(-((nw + ne) - (sw + se)), (nw + sw) - (ne + se));
    float reduce = max((nw + ne + sw + se) * 0.03125, 1.0 / 128.0);
    float rmin = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);
    dir = clamp(dir * rmin, -8.0, 8.0) * r;
    vec3 a = 0.5 * (texture(u_src, v_uv + dir * (1.0 / 3.0 - 0.5)).rgb + texture(u_src, v_uv + dir * (2.0 / 3.0 - 0.5)).rgb);
    vec3 b = a * 0.5 + 0.25 * (texture(u_src, v_uv - dir * 0.5).rgb + texture(u_src, v_uv + dir * 0.5).rgb);
    float lb = luma(b);
    o_col = vec4((lb < lmin || lb > lmax) ? a : b, 1.0);
}
