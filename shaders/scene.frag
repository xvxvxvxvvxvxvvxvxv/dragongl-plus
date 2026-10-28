#version 450
// DragonGL - Vulkan scene fragment shader (mirror of SCENE_FS in gfx_gl.c).
layout(std140, set = 0, binding = 0) uniform Scene {
    mat4 viewproj; vec4 cam_pos; vec4 player_pos; vec4 ambient; vec4 fog;
    vec4 cut_dir; vec4 misc; vec4 lights[64];
};
layout(set = 0, binding = 1) uniform sampler2DArray u_tex;
layout(set = 0, binding = 2) uniform sampler2D u_explored;
layout(push_constant) uniform Push { int pass; } pc;
layout(location = 0) in vec3 v_wpos;
layout(location = 1) in vec3 v_nrm;
layout(location = 2) in vec2 v_uv;
layout(location = 3) flat in float v_layer;
layout(location = 4) in float v_glow;
layout(location = 5) flat in int v_flags;
layout(location = 6) in vec4 v_col;
layout(location = 0) out vec4 o_col;
void main() {
    vec4 tex = texture(u_tex, vec3(v_uv, v_layer));
    vec4 base = tex * v_col;
    float expl = texture(u_explored, (v_wpos.xz + 0.5) / misc.y).r;
    float pd = length(v_wpos.xz - player_pos.xz);
    float vis = 1.0 - smoothstep(player_pos.w - 1.8, player_pos.w + 0.4, pd);
    if ((v_flags & 16) != 0) { expl = 1.0; vis = 1.0; }
    if (pc.pass == 2) {
        if (expl < 0.5) discard;
        float a = base.a * max(vis, 0.0);
        o_col = vec4(base.rgb * a, 1.0); return;
    }
    if (expl < 0.5) discard;
    if (pc.pass == 0 && base.a < 0.35) discard;
    vec3 n = normalize(v_nrm);
    float hemi = 0.55 + 0.45 * n.y;
    vec3 light = ambient.rgb * hemi;
    int nl = int(misc.x);
    for (int i = 0; i < nl; i++) {
        vec4 lp = lights[i * 2]; vec4 lc = lights[i * 2 + 1];
        vec3 L = lp.xyz - v_wpos; float d = length(L);
        float att = clamp(1.0 - d / lp.w, 0.0, 1.0); att *= att;
        float ndl = max(dot(n, L / max(d, 1e-3)), 0.0) * 0.8 + 0.2;
        light += lc.rgb * lc.a * att * ndl;
    }
    vec3 lit = base.rgb * light + base.rgb * v_glow * 1.4;
    if ((v_flags & 2) != 0) {
        vec3 vd = normalize(cam_pos.xyz - v_wpos);
        float fres = pow(1.0 - max(vd.y, 0.0), 3.0);
        lit += vec3(0.10, 0.16, 0.24) * (0.4 + fres) * (0.4 + light);
    }
    float lum = dot(base.rgb, vec3(0.3, 0.59, 0.11));
    vec3 mem = vec3(lum) * vec3(0.62, 0.70, 0.95) * ambient.a;
    vec3 col = mix(mem, lit, vis);
    float cd = length(v_wpos - cam_pos.xyz);
    col = mix(col, fog.rgb, 1.0 - exp(-fog.a * max(cd - 14.0, 0.0)));
    col = col * misc.w; col = col / (1.0 + max(col - 1.0, 0.0) * 0.6);
    o_col = vec4(col, pc.pass == 1 ? base.a : 1.0);
}
