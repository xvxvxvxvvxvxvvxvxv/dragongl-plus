// DragonGL+ - declarations shared by every shader of both renderers.
//
// The files in shaders/ are the only shader sources of the game:
//   - Vulkan: compiled to SPIR-V by glslc (VULKAN is defined), with
//     GL_GOOGLE_include_directive resolving the #include of this file;
//   - OpenGL 3.3: gfx_gl.c drops the #version / #extension lines, pastes
//     each #include "file" in place and compiles under "#version 330 core".
// The macros below hide the few syntax differences between the two.

#ifdef VULKAN
#define LOC(n) layout(location = n)
#define SAMPLER(b, type, name) layout(set = 0, binding = b) uniform type name
#define UBO(b) layout(std140, set = 0, binding = b) uniform
#define PUSH_BEGIN layout(push_constant) uniform Push {
#define PUSH_END } pc
#define VERTEX_ID gl_VertexIndex
#else
#define LOC(n)
#define SAMPLER(b, type, name) uniform type name
#define UBO(b) layout(std140) uniform
#define PUSH_BEGIN struct Push {
#define PUSH_END }; uniform Push pc
#define VERTEX_ID gl_VertexID
#endif

// Every pass has the same 64-byte block of parameters (push constants on
// Vulkan, a uniform struct on OpenGL); each shader names what p0..p3 mean.
#define PUSH_BLOCK PUSH_BEGIN vec4 p0; vec4 p1; vec4 p2; vec4 p3; PUSH_END

// Colours chosen by the game (vertex tints, lights, fog) are written in
// sRGB like the textures; lighting is computed in linear space.
vec3 to_linear(vec3 c) { return c * (c * (c * 0.305306011 + 0.682171111) + 0.012522878); }
vec3 to_srgb(vec3 c) {
    c = clamp(c, 0.0, 1.0);
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}
float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }
