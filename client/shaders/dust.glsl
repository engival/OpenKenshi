// Dust motes (render.dust_motes, kenshi-jj3.8.1): small drifting specks in the air of the nearest baked building (lightvol.hpp) that show only inside a sunbeam. Ported from kenshi-lab
// stormview/shaders/lit.glsl dust_vs / dust_fs (074dc80): camera facing quads placed by a hash of the instance index in a box and drifted by time in the vertex shader (no CPU
// simulation); the fragment is lit by the sun x one shadow lookup x a forward scattering phase and by nothing else - no ambient - so a mote outside a beam is black on the additive
// blend, i.e. invisible. The sun lookup is shadow_air of shadow.inc, the same per-sample sun visibility the sun shafts' march reads. Differences from the lab: the box is the
// nearest probe volume (an oriented box, camera relative), "indoors" is the probe enclosure (the shafts' gate), and the scene depth is sampled per mote (the pass has no depth attachment).
@include origin.inc
@include lighting.inc
@include shadow.inc

@vs dust_vs
@include_block origin
layout(binding=0) uniform dust_vs_params
{
	vec4 cam_f;	// xyz = camera forward, w = time (seconds)
	vec4 cam_r;	// xyz = camera right, w = 1 / (tan(fovY / 2) x aspect)
	vec4 cam_u;	// xyz = camera up, w = 1 / tan(fovY / 2)
	vec4 box_o;	// xyz = the box's origin (camera relative), w = mote radius (world units)
	vec4 box_x;	// xyz = the box's x edge, w = pixels per radian-ish: viewport height / tan(fovY / 2) / 2
	vec4 box_y;	// xyz = y edge, w = brightness
	vec4 box_z;	// xyz = z edge, w = near fade start
};

out vec3 v_rel;
out vec2 v_uv;
out vec2 v_corner;
out float v_vz;
out float v_gain;

float hash1(uint n)
{
	n ^= n >> 16;
	n *= 0x7feb352du;
	n ^= n >> 15;
	n *= 0x846ca68bu;
	n ^= n >> 16;
	return float(n >> 8) / 16777216.0;
}

void main()
{
	const uint i = uint(gl_InstanceIndex);
	const int k = gl_VertexIndex;
	const int q = k < 3 ? k : (k == 3 ? 0 : (k == 4 ? 2 : 3));
	const vec2 corner = vec2((q == 1 || q == 2) ? 1.0 : -1.0, q >= 2 ? 1.0 : -1.0);
	const vec4 seed = vec4(hash1(i * 4u + 1u), hash1(i * 4u + 2u), hash1(i * 4u + 3u), hash1(i * 4u + 4u));
	const float t = cam_f.w;
	const float ph = seed.w * 6.2831853;
	// every mote wanders on its own, and the room's air turns over gently; a function of the seed and the time only (the camera relative position must not enter it)
	vec3 p = box_o.xyz + seed.x * box_x.xyz + seed.y * box_y.xyz + seed.z * box_z.xyz;
	p += 0.9 * vec3(sin(t * 0.33 + ph * 3.0), 0.5 * sin(t * 0.21 + ph * 5.0), cos(t * 0.27 + ph * 7.0));
	p.xz += 0.5 * vec2(sin(seed.y * 9.0 + t * 0.15), cos(seed.y * 11.0 + t * 0.12));
	const float vx = dot(p, cam_r.xyz), vy = dot(p, cam_u.xyz), vz = dot(p, cam_f.xyz);
	v_rel = p;
	v_vz = vz;
	v_corner = corner;
	v_uv = offscreen_uv(vec2(vx * cam_r.w, vy * cam_u.w) / max(vz, 1e-3) * 0.5 + 0.5);
	// at least ~1 pixel wide, dimmed by the area it gained, so far motes do not flicker in and out
	const float px = box_o.w * box_x.w / max(vz, 1e-3);
	const float grown = max(px, 0.9);
	const float radius = box_o.w * grown / max(px, 1e-4);
	// a tumbling flake glints: the brightness flickers slowly
	const float glint = 0.55 + 0.45 * sin(t * (0.8 + seed.w * 1.7) + ph * 11.0);
	v_gain = box_y.w * glint * smoothstep(box_z.w, box_z.w * 4.0, vz) * (px * px) / (grown * grown);
	if (vz < 0.1) { gl_Position = vec4(2.0, 2.0, 2.0, 1.0); return; }
	gl_Position = vec4((vx + corner.x * radius) * cam_r.w, (vy + corner.y * radius) * cam_u.w, 0.5 * vz, vz);
}
@end

@fs dust_fs
@include_block origin
@include_block lighting
layout(binding=4) uniform texture2D shadow_tex;
layout(binding=2) uniform sampler shadow_smp;
@include_block shadowing
@include_block lighting_vol
layout(binding=0) uniform texture2D depth_tex;
layout(binding=1) uniform sampler depth_smp;
layout(binding=1) uniform dust_fs_params
{
	vec4 zinfo;	// x = near, y = far, z = 1 when the depth is already 0..1 clip (Vulkan), w = phase g
	vec4 dinfo;	// x = 1 when the sun lights motes (sun up, shadow map live), y = the lamp cones' gain (render.lamp_beams x the cone gain; 0 = off)
};
in vec3 v_rel;
in vec2 v_uv;
in vec2 v_corner;
in float v_vz;
in float v_gain;
out vec4 frag_color;

const float kPi = 3.14159265;

float phase_hg(float c, float g)
{
	return (1.0 - g * g) / (4.0 * kPi * pow(max(1.0 + g * g - 2.0 * g * c, 1e-4), 1.5));
}

// How indoors the air at p (camera relative) is: the shafts' gate (the baked enclosure of the probe volume x the box's edge ramp), the best of the live volumes.
float dust_indoor(vec3 p)
{
	float best = 0.0;
	const int cnt = int(vol_head.x);
	for (int i = 0; i < cnt; i++)
	{
		const vec4 a0 = vol_m[i * 7], a1 = vol_m[i * 7 + 1], a2 = vol_m[i * 7 + 2], r0 = vol_m[i * 7 + 3], r1 = vol_m[i * 7 + 4], r2 = vol_m[i * 7 + 5], inf = vol_m[i * 7 + 6];
		if (inf.y <= 0.0) { continue; }
		const vec3 g = vec3(dot(a0.xyz, p) + a0.w, dot(a1.xyz, p) + a1.w, dot(a2.xyz, p) + a2.w);
		const vec3 top = vec3(r0.w, r1.w, r2.w) - 1.0;
		const float sx = smoothstep(0.0, 2.0, min(g.x, top.x - g.x)), sz = smoothstep(0.0, 2.0, min(g.z, top.z - g.z));
		const float sy = smoothstep(0.0, 2.0, top.y - g.y) * clamp(g.y + 1.0, 0.0, 1.0);
		const float w = sx * sy * sz;
		if (w <= 0.0) { continue; }
		const vec3 uvw = (clamp(g, vec3(0.0), top) + vec3(0.5 + inf.x, 0.5, 0.5)) / vol_atlas.xyz;
		const vec2 u = textureLod(sampler3D(vol_use, vol_smp), uvw, 0.0).rg;	// r = usable, g = enclosure x usable
		const float enclosure = clamp(u.g / max(u.r, 0.05), 0.0, 1.0);
		best = max(best, w * smoothstep(0.55, 0.85, enclosure) * smoothstep(0.0, 0.3, u.r));
	}
	return best;
}

void main()
{
	const float round_mask = smoothstep(1.0, 0.1, dot(v_corner, v_corner));
	if (round_mask <= 0.0 || v_gain <= 0.0) { discard; }
	// behind a surface: the mote's centre against the scene depth (a mote is a few centimetres wide)
	const float d = textureLod(sampler2D(depth_tex, depth_smp), v_uv, 0.0).r;
	const float n = zinfo.x, f = zinfo.y;
	const float zv = zinfo.z > 0.5 ? n * f / (f - d * (f - n)) : 2.0 * n * f / ((f + n) - (d * 2.0 - 1.0) * (f - n));
	if (d < 0.99999 && v_vz > zv) { discard; }
	const float indoor = dust_indoor(v_rel);
	if (indoor <= 0.0) { discard; }
	const vec3 dir = normalize(v_rel);
	vec3 lit = vec3(0.0);
	// The sun beam (kenshi-jj3.8.11): the sun term of the light debugger gates it (sun_direct off = no sun on a mote), and a mote the shadow map does not light has nothing.
	if (dinfo.x > 0.5 && term_on(LT_SUN) > 0.5)
	{
		// Only the core of a fairly bright beam lights a mote (kenshi-jj3.8.4): the sun lookup at the mote and at four taps a hand's breadth around it, perpendicular to the sun, the
		// minimum of the five, through a threshold. A thin lit sliver (a shadow-map leak at a roof / wall join, a penumbra edge) is not a beam and shows no mote.
		const vec3 sx = normalize(cross(ldir.xyz, abs(ldir.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
		const vec3 sy = cross(ldir.xyz, sx);
		const float kTap = 0.35;
		const float core = min(min(shadow_air(v_rel + sx * kTap), shadow_air(v_rel - sx * kTap)), min(shadow_air(v_rel + sy * kTap), shadow_air(v_rel - sy * kTap)));
		const float sun = shadow_air(v_rel) * smoothstep(0.75, 0.95, min(core, 1.0));
		// the same light as the shafts' air: pi x the sun radiance x the phase towards the eye
		lit += sun_col.rgb * (nadir.w * fog_col.w * (kPi * phase_hg(dot(dir, ldir.xyz), zinfo.w) * sun));
	}
	// The lamps' cones (render.lamp_beams; the shafts' march, sunshafts.glsl): the spot lamps of the light list pointing down, cut by their building's walls (lamp_vis).
	if (dinfo.y > 0.0 && !term_off(LT_LAMPS))
	{
		const int pn = int(pl_head.x);
		vec3 lc = vec3(0.0);
		int n = 0;
		for (int i = 0; i < pn && n < 16; i++)
		{
			if (!(pl[4 * i + 1].w > 0.5 && pl[4 * i + 2].y < -0.5 && int(pl[4 * i + 3].w + 0.5) > 0)) { continue; }
			n++;
			const vec4 lp = pl[4 * i], lcol = pl[4 * i + 1], ls = pl[4 * i + 3];
			const vec3 dl = lp.xyz - v_rel;
			const float dd = length(dl);
			if (dd > lp.w) { continue; }
			const vec3 l = dl / max(dd, 1e-4);
			const float xr = clamp(dd / lp.w / 0.6, 0.0, 1.0);
			float att = (1.0 - xr) * (1.0 - xr);
			const float den = ls.x - ls.y;
			att *= clamp(pow(clamp(dot(pl[4 * i + 2].xyz, -l) - ls.y, 0.0, 1.0) / (abs(den) < 1e-6 ? 1e-6 : den), ls.z), 0.0, 1.0);
			if (att <= 0.0) { continue; }
			att *= lamp_vis(v_rel, int(ls.w + 0.5));
			lc += lcol.rgb * (att * phase_hg(dot(dir, l), zinfo.w));
		}
		lit += lc * (kPi * dinfo.y * fog_col.w);
	}
	if (dot(lit, vec3(1.0)) <= 0.0) { discard; }
	frag_color = vec4(lit * (v_gain * indoor * round_mask), 0.0);
}
@end

@image_sample_type depth_tex unfilterable_float
@sampler_type depth_smp nonfiltering
@image_sample_type shadow_tex depth
@sampler_type shadow_smp comparison
@program dust dust_vs dust_fs
