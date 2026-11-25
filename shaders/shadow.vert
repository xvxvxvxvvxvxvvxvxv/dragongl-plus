#version 450
#extension GL_GOOGLE_include_directive : require
// Shadow maps (Vulkan): world geometry seen from the sun or from one face
// of the torch's cube. p0..p3 = the light's view-projection (columns).
// No Y flip: the shadow maps are sampled with the same convention.
#include "common.glsl"
PUSH_BLOCK;
layout(location = 0) in vec3 a_pos;
layout(location = 2) in vec2 a_uv;
layout(location = 3) in float a_layer;
layout(location = 5) in float a_flags;
LOC(0) out vec3 v_wpos;
LOC(1) out vec2 v_uv;
LOC(2) flat out float v_layer;
LOC(3) flat out int v_flags;
void main() {
    v_wpos = a_pos;
    v_uv = a_uv;
    v_layer = a_layer;
    v_flags = int(a_flags + 0.5);
    gl_Position = mat4(pc.p0, pc.p1, pc.p2, pc.p3) * vec4(a_pos, 1.0);
    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;
}
