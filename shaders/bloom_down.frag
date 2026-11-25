#version 450
#extension GL_GOOGLE_include_directive : require
// Bloom, downsampling step: 13-tap filter (Jimenez, "Next generation post
// processing in Call of Duty"). The first step from the HDR scene keeps
// only what is brighter than the threshold (soft knee) and weights each
// tap group by its luma to stop single bright pixels from flickering.
// p0.xy = texel size of the source, p0.z = 1 on the first step,
// p0.w = threshold, p1.x = knee.
#include "common.glsl"
SAMPLER(0, sampler2D, u_src);
PUSH_BLOCK;
LOC(0) in vec2 v_uv;
layout(location = 0) out vec4 o_col;

vec3 tap(vec2 o) { return texture(u_src, v_uv + o * pc.p0.xy).rgb; }
vec3 prefilter(vec3 c) {
    float br = max(c.r, max(c.g, c.b));
    float knee = pc.p0.w * pc.p1.x;
    float soft = clamp(br - pc.p0.w + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee + 1e-4);
    return c * max(soft, br - pc.p0.w) / max(br, 1e-4);
}
float kweight(vec3 c) { return 1.0 / (1.0 + luma(c)); }

void main() {
    vec3 a = tap(vec2(-2, -2)), b = tap(vec2(0, -2)), c = tap(vec2(2, -2));
    vec3 d = tap(vec2(-1, -1)), e = tap(vec2(1, -1));
    vec3 f = tap(vec2(-2, 0)), g = tap(vec2(0, 0)), h = tap(vec2(2, 0));
    vec3 i = tap(vec2(-1, 1)), j = tap(vec2(1, 1));
    vec3 k = tap(vec2(-2, 2)), l = tap(vec2(0, 2)), m = tap(vec2(2, 2));
    vec3 g0 = (d + e + i + j) * 0.25, g1 = (a + b + f + g) * 0.25, g2 = (b + c + g + h) * 0.25;
    vec3 g3 = (f + g + k + l) * 0.25, g4 = (g + h + l + m) * 0.25;
    vec3 col;
    if (pc.p0.z > 0.5) {
        g0 = prefilter(g0); g1 = prefilter(g1); g2 = prefilter(g2); g3 = prefilter(g3); g4 = prefilter(g4);
        float w0 = kweight(g0) * 0.5, w1 = kweight(g1) * 0.125, w2 = kweight(g2) * 0.125;
        float w3 = kweight(g3) * 0.125, w4 = kweight(g4) * 0.125;
        col = (g0 * w0 + g1 * w1 + g2 * w2 + g3 * w3 + g4 * w4) / (w0 + w1 + w2 + w3 + w4);
    } else {
        col = g0 * 0.5 + (g1 + g2 + g3 + g4) * 0.125;
    }
    o_col = vec4(col, 1.0);
}
