// The interior mask, the game's InteriorMask compositor node (data/materials/common/interiormask.material + interiormask.hlsl): the interior-mask
// hulls (BUILDING `interior mask` parts) drawn twice into one RG16F target - back faces into G, front faces into R - every fragment writing its view
// depth + 1. particles.glsl with a CLIP_INTERIOR material (basic.hlsl) drops a fragment whose depth lies between the two, which keeps weather
// billboards out of building interiors. The hulls come in the scene's instance layout (worldbuildings.cpp, mesh.glsl inst_vs); the mvp is the
// view's, so a future second view renders its own mask rather than sharing this one.
@vs imask_vs
layout(binding=0) uniform imask_vs_params
{
	mat4 mvp;       // projection * rotation-only view (camera-relative)
	vec4 offset;    // xyz = part offset in mesh space (applied after the spin)
	vec4 spin;      // xyz = unit spin axis (mesh space), w = angle in radians
};
in vec3 position;
in vec4 inst_pos;      // xyz = origin relative to the camera, w = scale
in vec4 inst_quat;     // x y z w
in vec4 inst_stretch;  // xyz = per-axis scale factor on top of w
out float v_w;

vec3 qrot(vec4 q, vec3 v)
{
	return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

vec3 spin_rot(vec3 v)
{
	float c = cos(spin.w), s = sin(spin.w);
	return v * c + cross(spin.xyz, v) * s + spin.xyz * dot(spin.xyz, v) * (1.0 - c);
}

void main()
{
	vec3 local = (spin_rot(position) + offset.xyz) * inst_stretch.xyz;
	vec3 p = inst_pos.xyz + qrot(inst_quat, local * inst_pos.w);
	gl_Position = mvp * vec4(p, 1.0);
	v_w = gl_Position.w;
}
@end

@fs imask_fs
in float v_w;
out vec4 frag_color;

void main()
{
	// interiormask.hlsl: pos.w + 1; the pipeline's write mask picks R (front faces) or G (back faces)
	frag_color = vec4(v_w + 1.0);
}
@end

@program interiormask imask_vs imask_fs
