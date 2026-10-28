#version 450
// Full-screen triangle for the vignette / tint overlay.
layout(location = 0) out vec2 v_p;
void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    v_p = p * 2.0 - 1.0;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
