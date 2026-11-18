// Sun shadow lookup (cascaded shadow map atlas, see src/render/shadow.cpp and docs/reference/LIGHTING.md "Shadows").
// The including fragment shader declares, before `@include_block shadowing`, the atlas and its comparison sampler:
//   layout(binding=N) uniform texture2D shadow_tex;  layout(binding=M) uniform sampler shadow_smp;
@block shadowing
layout(binding=13) uniform sampler shadow_raw;	// the same atlas as plain depth (the PCSS blocker search, render.shadow_soft)
layout(binding=3) uniform shadow_params
{
	mat4 sm[4];      // camera-relative world position -> atlas uv (xy) and depth (z), per cascade (the set being drawn)
	mat4 sm_prev[4]; // the same for the previous sun step's set, read while sinfo.w < 1 (render.shadow_crossfade)
	vec4 split;      // outer camera distance of each cascade
	vec4 texel;      // world size of one shadow texel, per cascade
	vec4 sinfo;      // x = strength (0 = shadows off), y = 1 / atlas size (uv per texel), z = PCF radius in texels, w = crossfade 0..1 old -> new (1 = no previous map)
	vec4 ldir;       // xyz = direction towards the sun used by the shadow map (sun_dir with a minimum elevation), w = tan(sun half angle) of render.shadow_soft (0 = off)
	vec4 sm_tile[4];      // atlas uv rect (x0, y0, x1, y1) of each cascade's tile in the drawn set (the PCSS blocker search stays inside it)
	vec4 sm_tile_prev[4]; // the same for the previous step's set
	vec4 xinfo;           // x = camera travel since the step that started the crossfade (the previous map is only used inside its ball)
};

float shadow_pick(vec4 v, int i) { return i == 0 ? v.x : i == 1 ? v.y : i == 2 ? v.z : v.w; }

// Visibility of the sun in one cascade from one atlas set (prev: the previous sun step's map, render.shadow_crossfade): 8 rotated taps (Poisson
// disc), each a hardware 2x2 comparison, so the edge is about 4 texels wide.
float shadow_cascade_map(int c, vec3 p, vec3 ng, bool prev)
{
	mat4 m = sm[c];
	vec4 rect = sm_tile[c];
	if (prev) { m = sm_prev[c]; rect = sm_tile_prev[c]; }
	float tx = shadow_pick(texel, c);
	float nl = clamp(dot(ng, ldir.xyz), 0.0, 1.0);
	// receiver offset (normal offset + toward the light) instead of a caster-side depth bias: the offset grows with the grazing angle
	vec3 q = p + ng * (tx * (1.0 + 2.5 * sqrt(1.0 - nl * nl)) + 0.03) + ldir.xyz * (tx * 1.5);
	vec4 s = m * vec4(q, 1.0);
	s.xy = offscreen_uv(s.xy);	// origin.inc: the atlas was drawn with GL's NDC, its rows are the backend's
	float ang = 6.2831853 * fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
	vec2 cs = vec2(cos(ang), sin(ang));
	mat2 rot = mat2(cs.x, cs.y, -cs.y, cs.x);
	float r = sinfo.y * sinfo.z;
	if (ldir.w > 0.0 && c < 2)
	{
		// PCSS (render.shadow_soft, kenshi-jj3.6.1): 8 plain depth taps over a search radius give the mean blocker depth; the tap radius becomes
		// (receiver - blocker) x tan(sun half angle) in world units, in texels between today's width and a maximum.
		// one texel inside the cascade's tile in the storage's uv (the rect is in the logical uv of sm's rows), so no tap reads a neighbour's
		vec2 a = offscreen_uv(rect.xy), b = offscreen_uv(rect.zw);
		vec2 lo = min(a, b) + sinfo.y, hi = max(a, b) - sinfo.y;
		float zs = length(vec3(m[0].z, m[1].z, m[2].z));	// normalised depth per world unit
		float sr = sinfo.y * 10.0;
		vec4 b0 = vec4(-0.326, -0.406, -0.840, -0.074) * sr, b1 = vec4(-0.696, 0.457, -0.203, 0.621) * sr, b2 = vec4(0.962, -0.195, 0.473, -0.480) * sr, b3 = vec4(0.519, 0.767, 0.185, -0.893) * sr;
		float bsum = 0.0, bn = 0.0;
		vec2 bo[8] = vec2[8](b0.xy, b0.zw, b1.xy, b1.zw, b2.xy, b2.zw, b3.xy, b3.zw);
		for (int i = 0; i < 8; i++)
		{
			vec2 u = clamp(s.xy + rot * bo[i], lo, hi);
			float bz = texture(sampler2D(shadow_tex, shadow_raw), u).x;
			if (bz < s.z) { bsum += bz; bn += 1.0; }
		}
		if (bn > 0.0)
		{
			float gap = (s.z - bsum / bn) / zs;	// world units between the blocker and the receiver
			r = sinfo.y * clamp(gap * ldir.w / tx, sinfo.z, 12.0);
		}
	}
	vec4 o0 = vec4(-0.326, -0.406, -0.840, -0.074) * r, o1 = vec4(-0.696, 0.457, -0.203, 0.621) * r, o2 = vec4(0.962, -0.195, 0.473, -0.480) * r, o3 = vec4(0.519, 0.767, 0.185, -0.893) * r;
	float v = 0.0;
	v += texture(sampler2DShadow(shadow_tex, shadow_smp), vec3(s.xy + rot * o0.xy, s.z));
	v += texture(sampler2DShadow(shadow_tex, shadow_smp), vec3(s.xy + rot * o0.zw, s.z));
	v += texture(sampler2DShadow(shadow_tex, shadow_smp), vec3(s.xy + rot * o1.xy, s.z));
	v += texture(sampler2DShadow(shadow_tex, shadow_smp), vec3(s.xy + rot * o1.zw, s.z));
	v += texture(sampler2DShadow(shadow_tex, shadow_smp), vec3(s.xy + rot * o2.xy, s.z));
	v += texture(sampler2DShadow(shadow_tex, shadow_smp), vec3(s.xy + rot * o2.zw, s.z));
	v += texture(sampler2DShadow(shadow_tex, shadow_smp), vec3(s.xy + rot * o3.xy, s.z));
	v += texture(sampler2DShadow(shadow_tex, shadow_smp), vec3(s.xy + rot * o3.zw, s.z));
	return v * 0.125;
}

// The crossfade weight for cascade c at camera distance d: the previous map covers a ball around the camera of its step, so past that ball
// (xinfo.x = the camera's travel since the step) only the current map is used; the drop is ramped over the cascade's outer tenth.
float shadow_fade(int c, float d)
{
	if (sinfo.w >= 1.0 || xinfo.x <= 0.0) { return sinfo.w; }	// no previous map, or a still camera: the plain crossfade weight
	float r = shadow_pick(split, c);
	return mix(1.0, sinfo.w, clamp((r - xinfo.x - d) / (0.1 * r), 0.0, 1.0));
}

// The cascade's visibility, blending the previous sun step's map in while render.shadow_crossfade fades it out (sinfo.w 0 -> 1): a thin caster's
// shadow morphs between steps instead of re-rolling every frame or re-shaping at each step. One map read per tap outside a fade.
float shadow_cascade(int c, vec3 p, vec3 ng, float d)
{
	float v = shadow_cascade_map(c, p, ng, false);
	float w = shadow_fade(c, d);
	if (w < 1.0) { v = mix(shadow_cascade_map(c, p, ng, true), v, w); }
	return v;
}

// p = position relative to the camera, ng = geometric (unperturbed) normal. 1 = lit, 0 = fully shadowed; cascades blend over the last 15% of their range.
float shadow_visibility(vec3 p, vec3 ng)
{
	if (sinfo.x <= 0.0) { return 1.0; }
	float d = length(p);
	if (d >= split.w) { return 1.0; }
	int c = d < split.x ? 0 : d < split.y ? 1 : d < split.z ? 2 : 3;
	float v = shadow_cascade(c, p, ng, d);
	float t = clamp((d - shadow_pick(split, c) * 0.85) / (shadow_pick(split, c) * 0.15), 0.0, 1.0);
	if (t > 0.0) { v = mix(v, c == 3 ? 1.0 : shadow_cascade(c + 1, p, ng, d), t); }
	// The original's receiver fade (deferred.hlsl:173, rwh.27 rtw_shadow_dump.md): edgeBias = saturate((distance - 0.6 x Shadow Range) x 0.001) is added to the 3e-5
	// bias, so a receiver past 0.6 x range + 1000 (4000 for the 5000 range, split.w) is fully lit whatever the map holds. The exe grows a bias (the lit fraction depends
	// on the receiver-to-caster depth gap); MINE: a plain lerp to lit over the same 1000 units, here the camera distance of the lookup point.
	v = mix(v, 1.0, clamp((d - 0.6 * split.w) * 0.001, 0.0, 1.0));
	return mix(1.0, v, sinfo.x);
}
// The sun's visibility at a point of the air (camera relative p): one hardware 2x2 comparison of the cascade at its distance, no normal offset. Shared by the sun shafts' march
// (sunshafts.glsl) and the dust motes (dust.glsl): "inside a sunbeam" is this lookup at the sample.
float shadow_air_map(int c, vec3 p, bool prev)
{
	vec4 s = (prev ? sm_prev[c] : sm[c]) * vec4(p + ldir.xyz * (shadow_pick(texel, c) * 1.5), 1.0);
	s.xy = offscreen_uv(s.xy);
	return texture(sampler2DShadow(shadow_tex, shadow_smp), vec3(s.xy, s.z));
}
float shadow_air(vec3 p)
{
	float d = length(p);
	if (d >= split.w) { return 1.0; }
	int c = d < split.x ? 0 : d < split.y ? 1 : d < split.z ? 2 : 3;
	float v = shadow_air_map(c, p, false);
	float w = shadow_fade(c, d);
	if (w < 1.0) { v = mix(shadow_air_map(c, p, true), v, w); }
	return v;
}
@end

// Caster side, every shadow map fragment shader: the game's ShadowCaster_FP (common/shadowcaster.hlsl; kenshi-inspect findings/terrain_shadow_casting.md,
// rwh.23) lifts the stored depth by min(cap, slope x |depth gradient|) + fixed, the terrain included. This is what keeps a sun-facing slope lit at a grazing
// sun (kenshi-k16.2.57). Call it first, before any discard (derivatives). b: x = slope (RTW 6.0), y = cap in this cascade's normalised depth (src/render/shadow.cpp),
// z = fixed (RTW 0).
@block caster_bias
void caster_bias(vec4 b)
{
	float z = gl_FragCoord.z;
	gl_FragDepth = z + min(b.y, b.x * length(vec2(dFdx(z), dFdy(z)))) + b.z;
}
@end
