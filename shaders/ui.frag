#version 450
layout(set = 0, binding = 0) uniform sampler2D u_atlas;
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_col;
layout(location = 0) out vec4 o_col;
void main() { o_col = texture(u_atlas, v_uv) * v_col; }
