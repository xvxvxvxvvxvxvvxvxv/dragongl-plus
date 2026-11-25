#version 450
#extension GL_GOOGLE_include_directive : require
// Edge-aware 4x4 blur of the ambient occlusion (weights drop across
// depth discontinuities). p0.xy = texel size, p0.zw = near, far.
#include "common.glsl"
SAMPLER(0, sampler2D, u_ao);
SAMPLER(1, sampler2D, u_depth);
PUSH_BLOCK;
LOC(0) in vec2 v_uv;
layout(location = 0) out vec4 o_ao;
float lin(float d) {
    float z = d * 2.0 - 1.0;
    return 2.0 * pc.p0.z * pc.p0.w / (pc.p0.w + pc.p0.z - z * (pc.p0.w - pc.p0.z));
}
void main() {
    float z0 = lin(texture(u_depth, v_uv).r);
    float sum = 0.0, wsum = 0.0;
    for (int y = -2; y < 2; y++)
        for (int x = -2; x < 2; x++) {
            vec2 uv = v_uv + (vec2(x, y) + 0.5) * pc.p0.xy;
            float z = lin(texture(u_depth, uv).r);
            float w = 1.0 / (1e-3 + abs(z - z0) / max(z0, 1e-3) * 30.0);
            sum += texture(u_ao, uv).r * w;
            wsum += w;
        }
    o_ao = vec4(sum / max(wsum, 1e-4));
}
