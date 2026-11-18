// Sun shafts (render.sun_shafts, kenshi-jj3.7.1): sunbeams in the air of the nearest baked buildings. A full-screen pass over the finished scene (the scene depth is sampled, like fogvolume.glsl):
// each pixel's ray is clipped to the probe volume boxes of lightvol.hpp and to the scene depth, and marched; at every step the sun's shadow map says lit or not, the density is a
// small open-air value plus an indoor value that follows the probe's enclosure, and the light is the sun's irradiance x the Henyey-Greenstein phase. Ported from kenshi-lab stormview/shaders/lit.glsl
// comp_fs (074dc80) without its ambient veil and lamp glow. Added to the HDR scene (alpha 0, kenshi-jj3.7.3; see the end of main). A ray that misses every box costs the box tests.
@include origin.inc
@include lighting.inc
@include shadow.inc

@vs shaft_vs
out vec2 v_ndc;

void main()
{
	vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
	v_ndc = p * 2.0 - 1.0;
	gl_Position = vec4(v_ndc, 0.0, 1.0);
}
@end

@fs shaft_fs
@include_block origin
@include_block lighting
layout(binding=4) uniform texture2D shadow_tex;
layout(binding=2) uniform sampler shadow_smp;
@include_block shadowing
@include_block lighting_vol
layout(binding=0) uniform texture2D depth_tex;
layout(binding=1) uniform sampler depth_smp;
layout(binding=0) uniform shaft_params
{
	vec4 cam_f;	// xyz = camera forward
	vec4 cam_r;	// xyz = camera right x tan(fovY / 2) x aspect
	vec4 cam_u;	// xyz = camera up x tan(fovY / 2)
	vec4 zinfo;	// x = near, y = far, z = 1 when the depth is already 0..1 clip (Vulkan)
	vec4 shaft;	// x = open air scattering per unit, y = extra indoors, z = phase g, w = steps per volume
	vec4 dbg;	// y = render.lamp_beams x the cone gain (0 = no lamp cones), z = 1 when the sun beams are on (kenshi-jj3.28.1); x = render.sun_shafts_debug (0 normal; 1 = the lit fraction of the marched path; 2 = the sun visibility at the scene surface; 3 = normal with the sun forced lit; 4 = the enclosure along the path; kenshi-jj3.7.3)
};
in vec2 v_ndc;
out vec4 frag_color;

const float kPi = 3.14159265;

float phase_hg(float c, float g)
{
	return (1.0 - g * g) / (4.0 * kPi * pow(max(1.0 + g * g - 2.0 * g * c, 1e-4), 1.5));
}

void main()
{
	// render.sun_shafts_debug: 1 = the lit fraction of the path, 2 = the sun visibility at the surface, 4 = the enclosure along the path, 5 = the volume weight, 6 = the indoor
	// density gate, 7 = the accumulated in-scatter x8 (1, 2, 4, 5, 6 replace the scene); 3 keeps the scene with the sun forced lit
	const bool dbg_surface = dbg.x > 1.5 && dbg.x < 2.5;
	const bool dbg_enc = dbg.x > 3.5 && dbg.x < 4.5;
	const bool dbg_w = dbg.x > 4.5 && dbg.x < 5.5;
	const bool dbg_gate = dbg.x > 5.5 && dbg.x < 6.5;
	const bool dbg_dens = dbg.x > 7.5 && dbg.x < 8.5;
	const bool view = (dbg.x > 0.5 && dbg.x < 2.5) || dbg_enc || dbg_w || dbg_gate || dbg_dens;
	const bool forced = dbg.x > 2.5 && dbg.x < 3.5;
	const bool dbg_beam = dbg.x > 6.5 && dbg.x < 7.5;
	const bool sun_on = dbg.z > 0.5 && sinfo.x > 0.0;
	if (!view && ((!sun_on && dbg.y <= 0.0) || vol_head.x < 0.5)) { discard; }
	float d = textureLod(sampler2D(depth_tex, depth_smp), offscreen_uv(v_ndc * 0.5 + 0.5), 0.0).r;
	float n = zinfo.x;
	float f = zinfo.y;
	float zv = zinfo.z > 0.5 ? n * f / (f - d * (f - n)) : 2.0 * n * f / ((f + n) - (d * 2.0 - 1.0) * (f - n));
	vec3 ray = cam_f.xyz + cam_r.xyz * v_ndc.x + cam_u.xyz * v_ndc.y;
	float rl = length(ray);
	vec3 dir = ray / rl;
	float dist = d >= 0.99999 ? f : min(zv * rl, f);
	float jitter = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));	// interleaved gradient noise: grain, not stripes
	// The air's light (kenshi-jj3.7.3). light_shade treats sun_col as a radiance (its sun term is pi x N.L x sun_col), so the irradiance perpendicular to the rays that the medium
	// scatters is pi x sun_col and the phase gives the radiance towards the eye; the lab's sun_col is the irradiance already, so its comp_fs has no pi. That pi (and the indoor
	// path length: a Kenshi room is a fraction of the lab's hall) was why the beams were invisible.
	const vec3 lit = sun_col.rgb * (nadir.w * fog_col.w * (kPi * phase_hg(dot(dir, ldir.xyz), shaft.z)));
	vec3 scattered = vec3(0.0);
	// render.lamp_beams (kenshi-jj3.28.1): the shaded lamps of the scene's light list (lighting.inc point_light): a SPOT (c.w > 0.5) whose axis points down, with a visibility code (a baked
	// building volume, so the cone stops at the lamp's walls and floors via lamp_vis). An open lamp is a point light (no cone); its angle is the spot's outer / inner angle.
	int lamp_n = 0;
	int lamp_i[16];
	if (!view && dbg.y > 0.0)
	{
		const int pn = int(pl_head.x);
		for (int i = 0; i < pn && lamp_n < 16; i++)
		{
			if (pl[4 * i + 1].w > 0.5 && pl[4 * i + 2].y < -0.5 && int(pl[4 * i + 3].w + 0.5) > 0) { lamp_i[lamp_n++] = i; }
		}
	}
	float vis_sum = 0.0, len_sum = 0.0, enc_sum = 0.0, w_sum = 0.0, gate_sum = 0.0, dens_sum = 0.0;	// debug 1/4/5/6/8: the marched path's averages
	const int cnt = int(vol_head.x);
	const int steps = int(shaft.w);
	for (int i = 0; i < cnt; i++)
	{
		const vec4 a0 = vol_m[i * 7], a1 = vol_m[i * 7 + 1], a2 = vol_m[i * 7 + 2], r0 = vol_m[i * 7 + 3], r1 = vol_m[i * 7 + 4], r2 = vol_m[i * 7 + 5], inf = vol_m[i * 7 + 6];
		// the ray in probe index space (light_vol's affine map): g(t) = g0 + gd t, the same t as the camera relative ray
		const vec3 g0 = vec3(a0.w, a1.w, a2.w);
		const vec3 gd = vec3(dot(a0.xyz, dir), dot(a1.xyz, dir), dot(a2.xyz, dir));
		const vec3 top = vec3(r0.w, r1.w, r2.w) - 1.0;
		const vec3 inv = 1.0 / mix(gd, vec3(1e-6), lessThan(abs(gd), vec3(1e-6)));
		const vec3 ta = (vec3(0.0) - g0) * inv, tb = (top - g0) * inv;
		const vec3 tlo = min(ta, tb), thi = max(ta, tb);
		const float t0 = max(max(tlo.x, tlo.y), max(tlo.z, 0.0));
		const float t1 = min(min(thi.x, thi.y), min(thi.z, dist));
		if (t1 <= t0 || inf.y <= 0.0) { continue; }
		const float dt = (t1 - t0) / float(steps);
		for (int k = 0; k < steps; k++)
		{
			const float t = t0 + (float(k) + jitter) * dt;
			const vec3 g = g0 + gd * t;
			const float sx = smoothstep(0.0, 2.0, min(g.x, top.x - g.x)), sz = smoothstep(0.0, 2.0, min(g.z, top.z - g.z));
			const float sy = smoothstep(0.0, 2.0, top.y - g.y) * clamp(g.y + 1.0, 0.0, 1.0);
			// no volume blend factor (kenshi-jj3.7.3): inf.y is lightvol's one-second fade-in of a volume whose interior light just appeared, and the beams rode it, so a
			// shot taken a few frames after a scene jump saw 0.1-0.3 of the haze. The beams are there as soon as the volume is live (inf.y > 0, the check below).
			const float w = sx * sy * sz;
			if (!view && w <= 0.0) { continue; }
			const vec3 uvw = (clamp(g, vec3(0.0), top) + vec3(0.5 + inf.x, 0.5, 0.5)) / vol_atlas.xyz;
			const vec2 u = textureLod(sampler3D(vol_use, vol_smp), uvw, 0.0).rg;	// r = usable, g = enclosure x usable
			const float enclosure = clamp(u.g / max(u.r, 0.05), 0.0, 1.0);
			const float gate = smoothstep(0.55, 0.85, enclosure);
			if (view)
			{
				vis_sum += (dbg_enc ? 0.0 : forced ? 1.0 : shadow_air(dir * t)) * dt;
				enc_sum += enclosure * dt;
				w_sum += w * dt;
				gate_sum += w * gate * dt;
				dens_sum += (shaft.x + shaft.y * gate) * w * dt;
				len_sum += dt;
				continue;
			}
			const float sunv = !sun_on ? 0.0 : forced ? 1.0 : shadow_air(dir * t);
			const float density = (shaft.x + shaft.y * gate) * w;
			const float seg = exp(-density * dt);
			if (lamp_n > 0)
			{
				const vec3 rel = dir * t;
				vec3 lc = vec3(0.0);
				for (int j = 0; j < lamp_n; j++)
				{
					const int i = lamp_i[j];
					const vec4 lp = pl[4 * i], lcol = pl[4 * i + 1], ls = pl[4 * i + 3];
					const vec3 dl = lp.xyz - rel;
					const float dd = length(dl);
					if (dd > lp.w) { continue; }
					const vec3 l = dl / max(dd, 1e-4);
					const float x = dd / lp.w;
					// the beam's own falloff (not the surface light's 0.2 floor, which fills a 200-unit radius): the cone fades to 0 by 60% of the radius, so it reads as a beam under the shade, not a fogged room
					const float xr = clamp(x / 0.6, 0.0, 1.0);
					float att = (1.0 - xr) * (1.0 - xr);
					const float den = ls.x - ls.y;
					att *= clamp(pow(clamp(dot(pl[4 * i + 2].xyz, -l) - ls.y, 0.0, 1.0) / (abs(den) < 1e-6 ? 1e-6 : den), ls.z), 0.0, 1.0);
					if (att <= 0.0) { continue; }
					att *= lamp_vis(rel, int(ls.w + 0.5));
					lc += lcol.rgb * (att * phase_hg(dot(dir, l), shaft.z));
				}
				// an indoor-only haze (the gate is the sun beams' open-air test) at the sun beams' default strength; kPi: the irradiance the medium scatters, as `lit` above
				const float ld = shaft.y * w;
				scattered += (1.0 - exp(-ld * dt)) * lc * (kPi * dbg.y * fog_col.w);
			}
			// No front-to-back attenuation (kenshi-jj3.7.3): the air in shadow does not absorb the slice beyond it. Extinction over the shadowed stretch first (the room air between the
			// camera and the window) made a denser haze DARKER where it mattered - at render.sun_shafts 3 the frame was dimmer than at 1 - and it is what the additive output avoids anyway.
			scattered += (1.0 - seg) * lit * sunv;
		}
	}
	if (dbg_surface)
	{
		frag_color = vec4(vec3(shadow_air(dir * max(dist - 1.0, 1.0))), 1.0);
		return;
	}
	if (view)
	{
		const float v = len_sum > 0.0 ? (dbg_enc ? enc_sum : dbg_w ? w_sum : dbg_gate ? gate_sum : dbg_dens ? dens_sum * 5.0 : vis_sum) / len_sum : 0.0;
		frag_color = vec4(vec3(v), 1.0);
		return;
	}
	if (dbg_beam) { frag_color = vec4(scattered * 8.0, 1.0); return; }
	// The beams are added to the scene, not blended over it (alpha 0): a room's walls are lit by the unoccluded ambient and read brighter than air a beam could ever reach
	// (the in-scatter peaks at the sun's own radiance x the phase), so extinguishing the scene would grey a room out without the beam ever covering it.
	if (dot(scattered, vec3(1.0)) <= 0.0) { discard; }
	frag_color = vec4(scattered, 0.0);
}
@end

@image_sample_type depth_tex unfilterable_float
@sampler_type depth_smp nonfiltering
@image_sample_type shadow_tex depth
@sampler_type shadow_smp comparison
@sampler_type shadow_raw nonfiltering
@program sunshafts shaft_vs shaft_fs
