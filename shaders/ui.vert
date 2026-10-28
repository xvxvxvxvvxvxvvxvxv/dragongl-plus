#version 450
// DragonGL - Vulkan UI vertex shader (pixel coordinates, top-left origin).
layout(push_constant) uniform Push { vec2 screen; } pc;
layout(location = 0) in vec2 a_pos;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_col;
layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_col;
void main() {
    v_uv = a_uv; v_col = a_col;
    gl_Position = vec4(a_pos.x / pc.screen.x * 2.0 - 1.0, a_pos.y / pc.screen.y * 2.0 - 1.0, 0.0, 1.0);
}
