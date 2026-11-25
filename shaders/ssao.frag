#version 450
#extension GL_GOOGLE_include_directive : require
// Ambient occlusion (Vulkan, half resolution) after McGuire's Alchemy AO:
// positions rebuilt from the depth buffer, a disc of samples around each
// pixel, occlusion from the samples above the surface.
#include "common.glsl"
SAMPLER(0, sampler2D, u_depth);
layout(push_constant) uniform Push {
    mat4 inv_vp;     // clip -> world
    vec4 cam;        // xyz camera, w = pixels per world unit at distance 1
    vec4 params;     // xy = texel size of the depth, z = radius (world), w = intensity
} pc;
LOC(0) in vec2 v_uv;
layout(location = 0) out vec4 o_ao;

vec3 world_at(vec2 uv) {
    float d = texture(u_depth, uv).r;
    vec4 c = vec4(uv * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);
    c.y = -c.y; // the scene was drawn with Y flipped (scene.vert)
    vec4 w = pc.inv_vp * c;
    return w.xyz / w.w;
}

void main() {
    float d0 = texture(u_depth, v_uv).r;
    if (d0 >= 1.0) { o_ao = vec4(1.0); return; }
    vec3 P = world_at(v_uv);
    vec2 t = pc.params.xy;
    vec3 px = world_at(v_uv + vec2(t.x, 0)) - P, nx = P - world_at(v_uv - vec2(t.x, 0));
    vec3 py = world_at(v_uv + vec2(0, t.y)) - P, ny = P - world_at(v_uv - vec2(0, t.y));
    vec3 dx = dot(px, px) < dot(nx, nx) ? px : nx;
    vec3 dy = dot(py, py) < dot(ny, ny) ? py : ny;
    vec3 N = normalize(cross(dy, dx));
    if (dot(N, pc.cam.xyz - P) < 0.0) N = -N;
    float dist = length(pc.cam.xyz - P);
    float radius_px = pc.params.z * pc.cam.w / dist;
    if (radius_px < 1.0) { o_ao = vec4(1.0); return; }
    radius_px = min(radius_px, 80.0);
    // interleaved gradient noise: a different rotation per pixel
    float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    const int N_SAMPLES = 10;
    float occ = 0.0;
    for (int i = 0; i < N_SAMPLES; i++) {
        float a = (float(i) + noise) * 2.39996323; // golden angle
        float r = (float(i) + 0.5 + noise) / float(N_SAMPLES) * radius_px;
        vec2 uv = v_uv + vec2(cos(a), sin(a)) * r * t;
        if (uv.x <= 0.0 || uv.y <= 0.0 || uv.x >= 1.0 || uv.y >= 1.0) continue;
        vec3 v = world_at(uv) - P;
        float vv = dot(v, v);
        float vn = dot(v, N);
        // samples far behind (another object) count less
        float range = clamp(pc.params.z * pc.params.z / max(vv, 1e-4), 0.0, 1.0);
        occ += max(vn - 0.02 * dist * 0.05, 0.0) / (vv + 0.01) * range;
    }
    float ao = clamp(1.0 - pc.params.w * occ / float(N_SAMPLES), 0.0, 1.0);
    o_ao = vec4(ao * ao);
}
