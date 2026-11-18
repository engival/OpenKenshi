// Blood decal painter (src/render/blooddecals.cpp): one blood image quad into a patch tile of the decal atlas. The game's Quad2DMaterial (materials/forward/basic2D.hlsl:
// position = mul(float3(pos, 1), WorldMatrix).xy, out = tex x Colour), blended RGB src_alpha / one_minus_src_alpha, alpha added.
@vs decal_vs
layout(binding=0) uniform decal_vs_params
{
	vec4 row0;	// tile NDC x = dot(row0.xyz, (corner, 1))
	vec4 row1;	// tile NDC y
};

in vec2 corner;	// +-0.5
out vec2 v_uv;

void main()
{
	vec3 p = vec3(corner, 1.0);
	gl_Position = vec4(dot(row0.xyz, p), dot(row1.xyz, p), 0.0, 1.0);
	v_uv = vec2(corner.x + 0.5, 0.5 - corner.y);
}
@end

@fs decal_fs
layout(binding=0) uniform texture2D decal_tex;
layout(binding=0) uniform sampler decal_smp;
layout(binding=1) uniform decal_fs_params
{
	vec4 colour;	// RACE blood colour, alpha = min(the decal's alpha, life)
};

in vec2 v_uv;
out vec4 frag_color;

void main()
{
	frag_color = texture(sampler2D(decal_tex, decal_smp), v_uv) * colour;
}
@end

@program blooddecal decal_vs decal_fs
