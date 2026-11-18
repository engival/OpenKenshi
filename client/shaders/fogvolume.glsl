// Fog volumes (static, fogfeatures.dat): our own GLSL after materials/post/fog.hlsl (fog_sphere_fs, fog_beam_fs, fog_planes_fs, fogValue). One full-screen triangle per
// volume; the pixel's ray is intersected analytically with the volume and the thickness is clipped to the scene depth (sampled, so this runs in its own pass after the
// scene pass). Output is premultiplied (the game's alpha_blend): rgb = colour x a, a = fogValue's S curve of saturate(thickness x density) x alpha.
// All positions are camera relative (the CPU subtracts the double precision camera), so the maths stays exact at +-150000.
@include origin.inc

@vs fog_vs
out vec2 v_ndc;

void main()
{
	vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
	v_ndc = p * 2.0 - 1.0;
	gl_Position = vec4(v_ndc, 0.0, 1.0);
}
@end

@fs fog_fs
@include_block origin
layout(binding=0) uniform texture2D depth_tex;
layout(binding=0) uniform sampler depth_smp;
layout(binding=0) uniform fog_params
{
	vec4 cam_f;	// xyz = camera forward
	vec4 cam_r;	// xyz = camera right x tan(fovY / 2) x aspect
	vec4 cam_u;	// xyz = camera up x tan(fovY / 2)
	vec4 zinfo;	// x = near, y = far, z = 1 when the depth is already 0..1 clip (Vulkan), w = type (2 sphere, 3 beam, 4 planes)
	vec4 col;	// rgb = colour x light x exposure, w = alpha
	vec4 prm;	// x = density, y = edgeBlur (1 / the file's blur length), z = atmosphere fog start, w = atmosphere fog end
	vec4 pa;	// sphere: centre; beam: base; w = analytic radius (sphere 0.48 x scale, beam 0.4 x scale: the proxy meshes are a little larger)
	vec4 pb;	// beam: xyz = unit axis, w = height
	vec4 atm;	// rgb = atmosphere fog colour (display-referred, unexposed), the colour a far plane volume fades into
	vec4 wpos;	// xyz = camera world position (the sphere's distance break-up uses world coordinates)
	vec4 pl[7];	// planes: xyz = normal, w = offset in camera relative space: inside is dot(n, p) <= w
};
in vec2 v_ndc;
out vec4 frag_color;

vec4 fog_value(float depth, float density, vec4 colour)
{
	float amount = clamp(depth * density, 0.0, 1.0);
	float curve = amount < 0.5 ? 2.0 * amount * amount : 1.0 - 2.0 * (amount - 1.0) * (amount - 1.0);
	return vec4(colour.rgb, curve * colour.a);
}

void main()
{
	float d = textureLod(sampler2D(depth_tex, depth_smp), offscreen_uv(v_ndc * 0.5 + 0.5), 0.0).r;
	float n = zinfo.x;
	float f = zinfo.y;
	float zv = zinfo.z > 0.5 ? n * f / (f - d * (f - n)) : 2.0 * n * f / ((f + n) - (d * 2.0 - 1.0) * (f - n));
	vec3 ray = cam_f.xyz + cam_r.xyz * v_ndc.x + cam_u.xyz * v_ndc.y;
	float rl = length(ray);
	vec3 dir = ray / rl;
	float dist = d >= 0.99999 ? f : min(zv * rl, f);
	vec4 res = vec4(0.0);
	int type = int(zinfo.w + 0.5);
	if (type == 2)
	{
		// fog_sphere_fs
		vec3 m = -pa.xyz;
		float b = dot(m, dir);
		float disc = b * b - (dot(m, m) - pa.w * pa.w);
		if (disc < 0.0) { discard; }
		float root = sqrt(disc);
		float t0 = clamp(-b - root, 0.0, dist);
		float t1 = clamp(-b + root, 0.0, dist);
		// break up the sphere in the distance
		vec3 wp = wpos.xyz + dir * t0;
		float ss = sin(wp.y * 0.004) + 1.0;
		ss -= cos(wp.z * 0.0008) * cos(wp.x * 0.0008) - 1.0;
		ss *= clamp(t0 * 0.0001 - 1.0, 0.0, 1.0) * 1000.0;
		t1 -= ss;
		res = fog_value(t1 - t0, prm.x, col);
	} else if (type == 3) {
		// fog_beam_fs: cylinder from the base along the axis, capped by the two end planes, faded over the blur length at both ends
		vec3 axis = pb.xyz;
		vec3 m = -pa.xyz;
		float md = dot(m, axis);
		float nd = dot(dir, axis);
		float a = max(1.0 - nd * nd, 1e-6);
		float c = dot(m, m) - pa.w * pa.w - md * md;
		float b = dot(m, dir) - nd * md;
		float disc = b * b - a * c;
		if (disc < 0.0) { discard; }
		float root = sqrt(disc);
		float t0 = clamp((-b - root) / a, 0.0, dist);
		float t1 = clamp((-b + root) / a, 0.0, dist);
		float e0 = dot(axis, pa.xyz) / nd;
		float e1 = dot(axis, pa.xyz + axis * pb.w) / nd;
		t0 = max(t0, nd > 0.0 ? e0 : e1);
		t1 = min(t1, nd < 0.0 ? e0 : e1);
		float h = dot(dir * ((t0 + t1) * 0.5), axis) - dot(axis, pa.xyz);	// height of the segment's middle above the base
		float edge = clamp(h * prm.y, 0.0, 1.0) * clamp((pb.w - h) * prm.y, 0.0, 1.0);
		edge = 1.0 - (1.0 - edge) * (1.0 - edge);
		res = fog_value(t1 - t0, prm.x * edge, col);
	} else {
		// fog_planes_fs: convex prism of seven planes (the box has no floor in practice: the volume reaches down through the terrain)
		float far = dist;
		float near = 0.0;
		for (int i = 0; i < 7; i++)
		{
			float dn = dot(pl[i].xyz, dir);
			if (abs(dn) < 1e-7) { dn = dn < 0.0 ? -1e-7 : 1e-7; }
			float t = pl[i].w / dn;
			if (dn < 0.0) { near = max(near, t); } else { far = min(far, t); }
		}
		near = min(near, dist);
		float blur = prm.y * clamp(1.0 / (far * 0.00006), 0.0, 1.0);	// super far distance: softer edges
		vec3 ctr = dir * ((far + near) * 0.5);
		float edge = 1.0;
		for (int i = 0; i < 7; i++) { edge *= clamp((pl[i].w - dot(pl[i].xyz, ctr)) * blur, 0.0, 1.0); }
		edge *= 1.0 + abs(dir.y) * 0.9;
		res = fog_value(far - near, prm.x * edge, col);
		if (near > 0.0)
		{
			// fog the fog: from 12 km out the volume takes the atmosphere fog colour and dissolves into it (post/fog.hlsl:213-221, without the global weather fog)
			float k = clamp((near - 12000.0) * 0.0001, 0.0, 1.0);
			float fa = clamp((near - prm.z) / (prm.w - prm.z), 0.0, 1.0);
			res = mix(res, vec4(atm.rgb, fa * res.a), k);
		}
	}
	if (res.a <= 0.0) { discard; }
	frag_color = vec4(res.rgb * res.a, res.a);
}
@end

@image_sample_type depth_tex unfilterable_float
@sampler_type depth_smp nonfiltering
@program fogvolume fog_vs fog_fs
