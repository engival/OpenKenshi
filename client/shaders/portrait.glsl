// Portrait copy (src/portraits.cpp): the rendered HDR scratch into its RGBA8 atlas cell (the viewport), as is: the world shaders already apply the
// baked exposure. Clear pixels keep alpha 0 so the UI's portrait frame shows through.
@include origin.inc

@vs portrait_vs
out vec2 uv;

void main()
{
	vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
	uv = p;
	gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
@end

@fs portrait_fs
@include_block origin
layout(binding=0) uniform texture2D scratch_tex;
layout(binding=0) uniform sampler smp;
in vec2 uv;
out vec4 frag_color;

void main()
{
	vec4 c = textureLod(sampler2D(scratch_tex, smp), offscreen_uv(uv), 0.0);
	frag_color = vec4(clamp(c.rgb, 0.0, 1.0), clamp(c.a, 0.0, 1.0));
}
@end

@program portrait portrait_vs portrait_fs
