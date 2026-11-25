#version 450
#extension GL_GOOGLE_include_directive : require
// One triangle covering the screen. v_uv addresses the render targets the
// same way on both APIs (each samples images drawn with its own convention).
#include "common.glsl"
LOC(0) out vec2 v_uv;
void main() {
    vec2 p = vec2((VERTEX_ID << 1) & 2, VERTEX_ID & 2);
    v_uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
