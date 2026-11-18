// Copies the cached static-caster depth of the cascade tile under the current viewport into the live shadow atlas (Shadow::restoreCascade, kenshi-k16.2.205.12.2):
// the two atlases have the same layout, so the texel under the fragment is the same texel of the cache. One oversized triangle, depth compare ALWAYS, depth written from the cache.
@vs shadowrestore_vs
void main()
{
	vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
	gl_Position = vec4(p * 2.0 - 1.0, 1.0, 1.0);
}
@end

@fs shadowrestore_fs
layout(binding=0) uniform texture2D cache_tex;
layout(binding=0) uniform sampler cache_smp;
void main()
{
	gl_FragDepth = texelFetch(sampler2D(cache_tex, cache_smp), ivec2(gl_FragCoord.xy), 0).r;
}
@end

@program shadowrestore shadowrestore_vs shadowrestore_fs
@image_sample_type cache_tex unfilterable_float
@sampler_type cache_smp nonfiltering
