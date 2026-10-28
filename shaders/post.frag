#version 450
layout(push_constant) uniform Push { vec4 tint; vec4 params; } pc; // params.x = vignette, yz = aspect
layout(location = 0) in vec2 v_p;
layout(location = 0) out vec4 o_col;
void main() {
    vec2 q = v_p * pc.params.yz; float r = dot(q, q);
    float vig = smoothstep(0.55, 1.6, r) * pc.params.x;
    vec3 c = mix(vec3(0.0), pc.tint.rgb, pc.tint.a / max(pc.tint.a + vig, 1e-4));
    o_col = vec4(c, clamp(vig + pc.tint.a, 0.0, 1.0));
}
