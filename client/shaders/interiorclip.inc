// The interior clip (kenshi-h2v.11.4.2): the game's terrain / grass CLIP_INTERIOR test (terrainfp4.hlsl main_fs: `if (fragCoord.w < interior.y)
// clip(interior.x - fragCoord.w)`). imask_tex is the interior mask target of the same view (interiormask.glsl: R = front face, G = back face of the
// building's `interior mask` hull, view depth + 1; cleared to 0 where no hull is, so nothing clips there). A fragment whose view depth lies between
// them is inside a building and is dropped: the dune that rose above a sunk building's floor does not fill its interior. Fetched by texel (the target
// is the scene's size; a 1x1 zero view, interiorclip.hpp, when no mask ran, is read at texel 0).
@block interior_clip
layout(binding=21) uniform texture2D imask_tex;
layout(binding=12) uniform sampler imask_smp;
bool interior_clipped()
{
	vec2 m = texelFetch(sampler2D(imask_tex, imask_smp), min(ivec2(gl_FragCoord.xy), textureSize(sampler2D(imask_tex, imask_smp), 0) - ivec2(1)), 0).rg;
	float w = 1.0 / gl_FragCoord.w;
	return w < m.y && w > m.x;
}
@end
