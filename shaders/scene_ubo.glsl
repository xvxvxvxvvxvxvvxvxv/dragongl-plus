// Scene uniforms: mirror of SceneUniforms in src/client/gfx.h.
UBO(0) Scene {
    mat4 viewproj;
    vec4 cam_pos;     // xyz, w = time
    vec4 player_pos;  // xyz, w = vision radius
    vec4 ambient;     // rgb, a = memory brightness
    vec4 fog;         // rgb, a = density
    vec4 cut_dir;     // xy = camera forward on the ground, w = cut-away on
    vec4 misc;        // x = light count, y = explored map size
    vec4 sun_dir;     // xyz = towards the sun, w = intensity (0: no sun)
    vec4 sun_col;     // rgb
    mat4 sun_vp;      // sun shadow map: world -> clip
    vec4 shadow;      // x sun shadows on, y light 0 shadows on, z light 0 = player, w texel size
    vec4 proj;        // near, far, 3D image width, height
    vec4 lights[64];  // 32 x (position + radius, colour + intensity)
    vec4 mat_params[32]; // 2 materials per vec4: (specular, roughness)
};

vec2 material(float layer) {
    int m = int(layer + 0.5);
    vec4 p = mat_params[m >> 1];
    return (m & 1) != 0 ? p.zw : p.xy;
}
