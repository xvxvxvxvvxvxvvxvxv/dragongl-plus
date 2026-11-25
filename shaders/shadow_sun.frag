#version 450
#extension GL_GOOGLE_include_directive : require
// Sun shadow map: depth only; cut-out textures (leaves) and unlit
// markers cast no shadow.
#include "common.glsl"
SAMPLER(1, sampler2DArray, u_tex);
LOC(0) in vec3 v_wpos;
LOC(1) in vec2 v_uv;
LOC(2) flat in float v_layer;
LOC(3) flat in int v_flags;
void main() {
    if ((v_flags & 16) != 0 || texture(u_tex, vec3(v_uv, v_layer)).a < 0.35) discard;
}
