// Weather particles (ParticleUniverse billboards, particles.hpp): one instanced quad per particle, drawn in their own pass over the finished scene. The material is
// Basic_Coloured_Ambient_VP + Basic_Texture_Clipped_FP_HLSL (materials/forward/basic.hlsl): out = tex x vertex colour, where the vertex colour already holds the
// ParticleUniverse colour x the ambient factor clamp(5 sunY + 0.2, 0.1, 1) (CPU side); no sun colour, no fog. A CLIP_INTERIOR material (41 particle
// materials use Basic_Texture_Clipped_FP_HLSL: ash, rain, dust, twisters, wisps, drifting foliage, detritus) drops the fragments inside a building,
// by the interior mask target (interiormask.glsl). The scene depth is sampled (no depth attachment, as in fogvolume.glsl) and the fragment is dropped
// when it lies behind it. `soft` > 0 fades the alpha over that distance in front of the scene (a deviation: the game intersects hard).
// Positions are camera relative. billboard_type point: the quad faces the camera (right / up); oriented_self: the quad's long axis is the particle's direction and it turns
// around that axis to face the camera.
@vs part_vs
layout(binding=0) uniform part_vs_params
{
	mat4 mvp;       // projection x rotation-only view
	vec4 cam_r;     // xyz = camera right
	vec4 cam_u;     // xyz = camera up
};
in vec2 corner;     // x -0.5..0.5, y 0..1
in vec3 i_pos;      // camera relative
in vec2 i_size;     // width, height (world units)
in vec3 i_dir;      // unit axis, zero = camera facing
in vec3 i_misc;     // x = texture rotation (rad), y = origin offset on y (0 = bottom centre, 0.5 = centre), z = 1: oriented_shape (a fixed plane, not camera facing)
in vec4 i_col;
out vec2 v_uv;
out vec4 v_col;
out float v_w;

void main()
{
	float cx = corner.x * i_size.x;
	float cy = (corner.y - i_misc.y) * i_size.y;
	vec3 off;
	if (i_misc.z > 0.5)
	{
		// oriented_shape (Ogre BBT_ORIENTED_SHAPE): the quad lies in the plane of the particle's own orientation, which here is the shortest turn of the up
		// axis onto i_dir (the Align affector's direction). Its x axis is that turn applied to world x; nothing turns it to the camera.
		vec3 k = vec3(i_dir.z, 0.0, -i_dir.x);
		float c = i_dir.y;
		vec3 ex = vec3(1.0, 0.0, 0.0);
		vec3 kx = cross(k, ex);
		vec3 xa = c > -0.999 ? ex + kx + cross(k, kx) / (1.0 + c) : vec3(1.0, 0.0, 0.0);
		off = xa * cx + i_dir * cy;
	} else if (dot(i_dir, i_dir) > 0.0)
	{
		vec3 side = cross(i_dir, normalize(i_pos));
		float sl = length(side);
		side = sl > 1e-4 ? side / sl : cam_r.xyz;
		off = side * cx + i_dir * cy;
	} else {
		off = cam_r.xyz * cx + cam_u.xyz * cy;
	}
	gl_Position = mvp * vec4(i_pos + off, 1.0);
	v_w = gl_Position.w;
	// texture coordinates (v = 0 at the top of the quad, Ogre), rotated around the quad centre (TextureRotator)
	vec2 q = vec2(corner.x, 0.5 - corner.y);
	float s = sin(i_misc.x), k = cos(i_misc.x);
	v_uv = vec2(k * q.x - s * q.y, s * q.x + k * q.y) + vec2(0.5);
	v_col = i_col;
}
@end

@fs part_fs
layout(binding=0) uniform texture2D depth_tex;
layout(binding=1) uniform texture2D part_tex;
layout(binding=2) uniform texture2D mask_tex;
layout(binding=0) uniform sampler depth_smp;
layout(binding=1) uniform sampler part_smp;
layout(binding=2) uniform sampler mask_smp;
layout(binding=1) uniform part_fs_params
{
	vec4 zinfo;     // x = near, y = far, z = 1 when the depth is already 0..1 clip (Vulkan), w = 1 / soft distance (0 = hard)
	vec4 res;       // xy = framebuffer size
	vec4 misc;      // x = 1: clip indoors by the interior mask (the game's CLIP_INTERIOR materials); y = 1: the scene depth test is on (a dev A/B turns it off)
};
in vec2 v_uv;
in vec4 v_col;
in float v_w;
out vec4 frag_color;

void main()
{
	vec2 uv = gl_FragCoord.xy / res.xy;
	if (misc.x > 0.5)
	{
		// basic.hlsl CLIP_INTERIOR: the mask's R = front face, G = back face of the interior hulls (view depth + 1, interiormask.glsl); a
		// fragment between them is inside a building and goes. Cleared pixels are 0, so the test never fires outside a hull.
		vec2 interior = textureLod(sampler2D(mask_tex, mask_smp), uv, 0.0).rg;
		if (v_w < interior.y && v_w > interior.x) { discard; }
	}
	float d = textureLod(sampler2D(depth_tex, depth_smp), uv, 0.0).r;
	float n = zinfo.x;
	float f = zinfo.y;
	float zv = d >= 0.99999 ? f : (zinfo.z > 0.5 ? n * f / (f - d * (f - n)) : 2.0 * n * f / ((f + n) - (d * 2.0 - 1.0) * (f - n)));
	if (misc.y > 0.5 && v_w > zv) { discard; }
	vec4 t = texture(sampler2D(part_tex, part_smp), v_uv);
	// The texture and v_col (ParticleUniverse authored colours x the light multipliers) are the game's display-referred values, used raw as
	// particles.hlsl:15 does. Alpha is coverage, not colour.
	vec3 rgb = t.rgb;
	vec4 c = vec4(rgb * v_col.rgb, t.a * v_col.a);
	if (zinfo.w > 0.0) { c.a *= clamp((zv - v_w) * zinfo.w, 0.0, 1.0); }
	frag_color = c;
}
@end

@image_sample_type depth_tex unfilterable_float
@sampler_type depth_smp nonfiltering
@program particles part_vs part_fs
