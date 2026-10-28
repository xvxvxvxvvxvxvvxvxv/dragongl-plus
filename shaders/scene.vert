#version 450
// DragonGL - Vulkan scene vertex shader (mirror of SCENE_VS in gfx_gl.c).
layout(std140, set = 0, binding = 0) uniform Scene {
    mat4 viewproj; vec4 cam_pos; vec4 player_pos; vec4 ambient; vec4 fog;
    vec4 cut_dir; vec4 misc; vec4 lights[64];
};
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec3 a_nrm;
layout(location = 2) in vec2 a_uv;
layout(location = 3) in float a_layer;
layout(location = 4) in float a_glow;
layout(location = 5) in float a_flags;
layout(location = 6) in vec4 a_col;
layout(location = 0) out vec3 v_wpos;
layout(location = 1) out vec3 v_nrm;
layout(location = 2) out vec2 v_uv;
layout(location = 3) flat out float v_layer;
layout(location = 4) out float v_glow;
layout(location = 5) flat out int v_flags;
layout(location = 6) out vec4 v_col;
void main() {
    vec3 p = a_pos; int fl = int(a_flags + 0.5); float t = cam_pos.w;
    if ((fl & 1) != 0 && cut_dir.w > 0.5) {
        vec2 d = p.xz - player_pos.xz; vec2 f = cut_dir.xy;
        float along = dot(d, -f); float lat = abs(d.x * f.y - d.y * f.x);
        float k = smoothstep(0.3, 1.3, along) * (1.0 - smoothstep(2.0 + along * 0.35, 3.5 + along * 0.45, lat)) * (1.0 - smoothstep(11.0, 14.0, along));
        p.y = mix(p.y, min(p.y, 0.16), k);
    }
    if ((fl & 2) != 0) p.y += sin(t * 1.6 + p.x * 1.3 + p.z * 0.9) * 0.025;
    if ((fl & 8) != 0) p.x += sin(t * 2.0 + a_pos.x * 2.0 + a_pos.z) * 0.18 * a_pos.y;
    vec2 uv = a_uv;
    if ((fl & 2) != 0) uv += vec2(t * 0.03, t * 0.02);
    if ((fl & 4) != 0) uv += vec2(t * 0.02, -t * 0.015);
    v_wpos = p; v_nrm = a_nrm; v_uv = uv; v_layer = a_layer; v_glow = a_glow; v_flags = fl; v_col = a_col;
    gl_Position = viewproj * vec4(p, 1.0);
    gl_Position.y = -gl_Position.y;
    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;
}
