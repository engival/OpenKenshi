// The character creator's backdrop (creatorscene.hpp): the original's CharacterEditorBackground, a full-screen quad of SandyGround.dds (Basic_Texture_FP, colour 1,
// charedit.compositor), the texture's raw texels stretched to the viewport. Drawn first in the scene pass; the display gain is pinned to 1 while the creator is shown,
// so the screen holds the texture's bytes.
@vs creatorbg_vs
out vec2 uv;

void main()
{
	vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
	uv = vec2(p.x, 1.0 - p.y);	// a file's rows are in file order (origin.inc): row 0 at the top of the screen
	gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
@end

@fs creatorbg_fs
layout(binding=0) uniform texture2D bg_tex;
layout(binding=0) uniform sampler bg_smp;
in vec2 uv;
out vec4 frag_color;

void main()
{
	frag_color = vec4(texture(sampler2D(bg_tex, bg_smp), uv).rgb, 1.0);
}
@end

@program creatorbg creatorbg_vs creatorbg_fs
