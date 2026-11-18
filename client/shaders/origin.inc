// Texture origin of our own render targets (kenshi-k16.2.79; docs/CLIENT.md "Rendering": offscreen reads). Geometry is drawn with GL's NDC (y up), and sokol keeps that
// on every backend (Vulkan gets a negative height viewport). The image's rows differ: GL stores the bottom row first (uv.y = 0 at the bottom, the same as NDC y = -1),
// Vulkan, D3D, Metal and WebGPU the top row (sg_features.origin_top_left). So a uv worked out from NDC (v up, the logical coordinate) must be turned into the
// storage's uv before it samples a render target we drew, a depth texture or an atlas written by geometry; a texture loaded from a file is not one (its rows are
// in file order on every backend), nor is a texelFetch at gl_FragCoord (a framebuffer coordinate: it is in storage order on both).
//
//   vec2 offscreen_uv(vec2 uv)        logical uv (v up) -> the render target's uv
//   vec2 offscreen_texel(vec2 p, float height)   the same for a texel coordinate p with its texel centres at integers (p = uv x size - 0.5), a target `height` rows high
//   offscreen_offset(o)               a constant ivec2 texel offset (textureLodOffset) in logical rows -> the target's (a macro: the offset must stay constant)
//
// One GLSL source: sokol-shdc defines SOKOL_GLSL while it compiles the GL targets (glsl430) and not for spirv_vk / hlsl / metal / wgsl. It follows
// sg_query_features().origin_top_left (main.cpp checks the two agree at startup), so there is no hard-coded flip and the GL code is the identity.
@block origin
#ifdef SOKOL_GLSL
vec2 offscreen_uv(vec2 uv) { return uv; }
vec2 offscreen_texel(vec2 p, float height) { return p; }
#define offscreen_offset(o) (o)
#else
vec2 offscreen_uv(vec2 uv) { return vec2(uv.x, 1.0 - uv.y); }
vec2 offscreen_texel(vec2 p, float height) { return vec2(p.x, height - 1.0 - p.y); }
#define offscreen_offset(o) ivec2((o).x, -(o).y)
#endif
@end
