#version 450
#extension GL_GOOGLE_include_directive : require
// HDR scene + bloom -> exposure -> ACES tone curve -> sRGB -> grading ->
// vignette and screen tint (damage flash, fades).
// p0 = tint (rgb, a = amount)
// p1 = vignette, aspect x, aspect y, exposure
// p2 = bloom strength, saturation, contrast, 1 when bloom is on
// p3 = colour grade (rgb multiplier in linear space)
#include "common.glsl"
SAMPLER(0, sampler2D, u_hdr);
SAMPLER(1, sampler2D, u_bloom);
PUSH_BLOCK;
LOC(0) in vec2 v_uv;
layout(location = 0) out vec4 o_col;

vec3 aces(vec3 x) { // Narkowicz's fit of the ACES filmic curve
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

void main() {
    vec3 c = texture(u_hdr, v_uv).rgb;
    if (pc.p2.w > 0.5) c += texture(u_bloom, v_uv).rgb * pc.p2.x;
    c *= pc.p1.w * pc.p3.rgb;
    c = to_srgb(aces(c));
    float l = luma(c);
    c = mix(vec3(l), c, pc.p2.y);
    c = clamp((c - 0.5) * pc.p2.z + 0.5, 0.0, 1.0);
    vec2 q = (v_uv * 2.0 - 1.0) * pc.p1.yz;
    float vig = smoothstep(0.55, 1.6, dot(q, q)) * pc.p1.x;
    vec4 tint = pc.p0;
    vec3 over = mix(vec3(0.0), tint.rgb, tint.a / max(tint.a + vig, 1e-4));
    c = mix(c, over, clamp(vig + tint.a, 0.0, 1.0));
    o_col = vec4(c, luma(c));
}
