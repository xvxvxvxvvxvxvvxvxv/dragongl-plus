#version 450
#extension GL_GOOGLE_include_directive : require
// Linear-space lighting into the HDR target. p0.x = pass:
// 0 opaque, 1 alpha blended, 2 additive.
#include "common.glsl"
#include "scene_ubo.glsl"
SAMPLER(1, sampler2DArray, u_tex);
SAMPLER(2, sampler2D, u_explored);
#ifdef VULKAN
// Vulkan only: scene depth (soft particles) and the shadow maps
SAMPLER(3, sampler2D, u_depth);
SAMPLER(4, sampler2DShadow, u_sun_shadow);
SAMPLER(5, samplerCube, u_torch_shadow);
#endif
PUSH_BLOCK;
LOC(0) in vec3 v_wpos;
LOC(1) in vec3 v_nrm;
LOC(2) in vec2 v_uv;
LOC(3) flat in float v_layer;
LOC(4) in float v_glow;
LOC(5) flat in int v_flags;
LOC(6) in vec4 v_col;
layout(location = 0) out vec4 o_col;

#ifdef VULKAN
// 1 = lit, 0 = in the sun's shadow (3x3 PCF on the hardware comparison)
float sun_shadow(vec3 wp, vec3 n) {
    vec4 c = sun_vp * vec4(wp + n * 0.06, 1.0);
    vec3 p = c.xyz / c.w;
    vec2 uv = p.xy * 0.5 + 0.5;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) return 1.0;
    float z = p.z * 0.5 + 0.5 - 0.0015;
    float s = 0.0;
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++) s += texture(u_sun_shadow, vec3(uv + vec2(x, y) * shadow.w, z));
    return s / 9.0;
}
// player's torch: cube map of distance / radius
float torch_shadow(vec3 wp, vec3 n, vec4 lp) {
    vec3 d = wp + n * 0.05 - lp.xyz;
    float cur = length(d) / lp.w - 0.012;
    vec3 a = normalize(cross(d, abs(d.y) < 0.9 * length(d) ? vec3(0, 1, 0) : vec3(1, 0, 0)));
    vec3 b = cross(normalize(d), a);
    float r = 0.015 * length(d);
    float s = 0.0;
    s += cur < texture(u_torch_shadow, d + (a + b) * r).r ? 1.0 : 0.0;
    s += cur < texture(u_torch_shadow, d + (a - b) * r).r ? 1.0 : 0.0;
    s += cur < texture(u_torch_shadow, d + (-a + b) * r).r ? 1.0 : 0.0;
    s += cur < texture(u_torch_shadow, d + (-a - b) * r).r ? 1.0 : 0.0;
    return s * 0.25;
}
float linear_depth(float d) {
    float z = d * 2.0 - 1.0;
    return 2.0 * proj.x * proj.y / (proj.y + proj.x - z * (proj.y - proj.x));
}
// particles and glows fade out where they cut into walls and floors
float soft_fade() {
    if (proj.z <= 0.0) return 1.0;
    float scene = linear_depth(texture(u_depth, gl_FragCoord.xy / proj.zw).r);
    float self = linear_depth(gl_FragCoord.z);
    return clamp((scene - self) / 0.35, 0.0, 1.0);
}
#endif

void main() {
    int pass = int(pc.p0.x + 0.5);
    vec4 base = texture(u_tex, vec3(v_uv, v_layer)) * v_col; // linear (sRGB texture)
    float expl = texture(u_explored, (v_wpos.xz + 0.5) / misc.y).r;
    float pd = length(v_wpos.xz - player_pos.xz);
    float vis = 1.0 - smoothstep(player_pos.w - 1.8, player_pos.w + 0.4, pd);
    if ((v_flags & 16) != 0) { expl = 1.0; vis = 1.0; }
    if (expl < 0.5) discard;
    float soft = 1.0;
#ifdef VULKAN
    if (pass != 0) soft = soft_fade();
#endif
    if (pass == 2) {
        float a = base.a * max(vis, 0.0) * soft;
        o_col = vec4(base.rgb * a * 1.6, 1.0);
        return;
    }
    if (pass == 0 && base.a < 0.35) discard;

    vec3 n = normalize(v_nrm);
    vec3 V = normalize(cam_pos.xyz - v_wpos);
    vec2 mp = material(v_layer);            // x = specular strength, y = roughness
    float shininess = 2.0 / max(mp.y * mp.y * mp.y * mp.y, 1e-3) - 2.0;
    float spec_norm = (shininess + 8.0) / 25.13;

    float hemi = 0.55 + 0.45 * n.y;
    vec3 diffuse = to_linear(ambient.rgb) * hemi;
    vec3 specular = vec3(0.0);
    if (sun_dir.w > 0.0) {
        vec3 L = normalize(sun_dir.xyz);
        float ndl = max(dot(n, L), 0.0);
        vec3 sc = to_linear(sun_col.rgb) * sun_dir.w;
#ifdef VULKAN
        if (shadow.x > 0.5 && ndl > 0.0) sc *= sun_shadow(v_wpos, n);
#endif
        diffuse += sc * ndl;
        vec3 H = normalize(L + V);
        specular += sc * pow(max(dot(n, H), 0.0), shininess) * spec_norm * ndl;
    }
    int nl = int(misc.x);
    for (int i = 0; i < nl; i++) {
        vec4 lp = lights[i * 2];
        vec4 lc = lights[i * 2 + 1];
        vec3 L = lp.xyz - v_wpos;
        float d = length(L);
        L /= max(d, 1e-3);
        float att = clamp(1.0 - d / lp.w, 0.0, 1.0);
        att *= att;
        float ndl = max(dot(n, L), 0.0);
        vec3 c = to_linear(lc.rgb) * lc.a * att;
#ifdef VULKAN
        if (i == 0 && shadow.y > 0.5 && att > 0.0) c *= mix(0.25, 1.0, torch_shadow(v_wpos, n, lp));
#endif
        diffuse += c * (ndl * 0.8 + 0.2);
        vec3 H = normalize(L + V);
        specular += c * pow(max(dot(n, H), 0.0), shininess) * spec_norm * ndl;
    }
    vec3 lit = base.rgb * diffuse + specular * mp.x + base.rgb * v_glow * 3.0;
    if ((v_flags & 2) != 0) {
        float fres = pow(1.0 - max(V.y, 0.0), 3.0);
        lit += to_linear(vec3(0.10, 0.16, 0.24)) * (0.4 + fres) * (0.4 + diffuse);
    }
    // seen before but out of sight: a cold grey "memory" of the place
    float lum = luma(base.rgb);
    vec3 mem = vec3(lum) * to_linear(vec3(0.62, 0.70, 0.95) * ambient.a);
    vec3 col = mix(mem, lit, vis);
    float cd = length(v_wpos - cam_pos.xyz);
    col = mix(col, to_linear(fog.rgb), 1.0 - exp(-fog.a * max(cd - 14.0, 0.0)));
    o_col = vec4(col, pass == 1 ? base.a * soft : 1.0);
}
