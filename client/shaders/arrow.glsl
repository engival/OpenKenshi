// The stealth detection arrows (kenshi-h2v.27.2): one arrow.mesh instance per DETECTED_BY entry, unlit in the entry's colour. The mesh's local -X
// is turned onto the observer (the original's getRotationTo(NEGATIVE_UNIT_X, d)) and it is scaled x 0.5, y/z 0.35 (5 u long, 0.84 u across).
@vs arrow_vs
layout(binding=0) uniform arrow_vs_params
{
	mat4 mvp;           // projection * rotation-only view (camera-relative)
};

in vec3 pos;            // arrow.mesh model space: x -10..0, y/z -1.2..1.2
in vec4 i_origin_quat;  // xyz = the anchor relative to the camera, w = the quaternion's w
in vec4 i_quat;         // xyz = the quaternion's axis part, w unused
in vec4 i_colour;       // rgb, unlit; a unused

out vec4 v_col;

vec3 rotate(vec4 q, vec3 v)
{
	return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

void main()
{
	const vec3 kScale = vec3(0.5, 0.35, 0.35);
	const vec4 q = vec4(i_quat.xyz, i_origin_quat.w);
	vec3 p = rotate(q, pos * kScale);
	v_col = i_colour;
	gl_Position = mvp * vec4(i_origin_quat.xyz + p, 1.0);
}
@end

@fs arrow_fs
in vec4 v_col;
out vec4 frag_color;

void main()
{
	frag_color = v_col;
}
@end

@program arrow arrow_vs arrow_fs
