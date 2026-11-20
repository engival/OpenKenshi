@vs mesh_vs
layout(binding=0) uniform mesh_vs_params
{
	mat4 mvp;       // projection * rotation-only view (camera-relative)
	vec4 origin;    // xyz = model origin relative to the camera
};

in vec3 position;
in vec3 normal;
in vec2 texcoord0;

out vec3 v_normal;
out vec2 v_uv;

void main()
{
	v_normal = normal;
	v_uv = texcoord0;
	gl_Position = mvp * vec4(position + origin.xyz, 1.0);
}
@end

@fs mesh_fs
layout(binding=0) uniform texture2D diffuse_tex;
layout(binding=0) uniform sampler diffuse_smp;
layout(binding=1) uniform mesh_fs_params
{
	vec4 sun;       // xyz = direction towards the sun (model space)
};

in vec3 v_normal;
in vec2 v_uv;
out vec4 frag_color;

void main()
{
	vec3 n = normalize(v_normal);
	if (!gl_FrontFacing) { n = -n; }
	float diff = max(dot(n, normalize(sun.xyz)), 0.0);
	vec3 base = texture(sampler2D(diffuse_tex, diffuse_smp), v_uv).rgb;
	frag_color = vec4(base * (0.55 + 0.65 * diff), 1.0);
}
@end

@program mesh mesh_vs mesh_fs

// Instanced world buildings and hand-placed features: per-instance camera-relative position + scale, rotation quaternion and a per-axis
// stretch (non-uniform feature scale); per-part model offset and spin.
@vs inst_vs
layout(binding=0) uniform inst_vs_params
{
	mat4 mvp;       // projection * rotation-only view (camera-relative)
	vec4 tile;      // xy = UV tiling, z = a swaying leaves mesh's top y (0: no sway)
	vec4 offset;    // xyz = part offset in mesh space (applied after the spin)
	vec4 spin;      // xyz = unit spin axis (mesh space), w = angle in radians
	vec4 camw;      // xyz = camera world position (float: world position = p + camw, for triplanar/terrain mapping), w = the wind clock (s)
	vec4 base;      // xyz = added to every instance position (instances stored relative to a cell origin; zero for buildings)
	vec4 aim;       // a wind-aimed part or one nested in it: xyz = the pivot (mesh space), w = 1 (0: not aimed)
	vec4 wind;      // a wind-aimed part's turn: xy = the world wind direction (toward) x, z it turns from, zw = the one it turns to
	vec4 turn;      // x = game seconds since the turn began, y = the turn rate, rad/s (`rotation speed max`; 0: the turn takes one second)
	vec4 limb;      // x = 1: a ground severed limb, so the cylindrical blood UVs of severed_limb_vs; 0 every other part; y = 1: a tree's leaves mesh (frond motion)
	vec4 sway;      // render.leaf_sway (tile.z > 0): x = the live wind strength (0 still), yz = its unit direction x, z, w = the integrated sway clock
};

in vec3 position;
in vec3 normal;
in vec2 texcoord0;
in vec4 colour;     // vertex colour, alpha blends the dual texture set
in vec4 inst_pos;   // xyz = origin relative to the camera, w = scale
in vec4 inst_quat;  // x y z w
in vec4 inst_stretch;   // xyz = per-axis scale factor on top of w, w = biome table row (TERRAIN-mode features: detail.cpp rowAt); w < 0: under construction, -(H + progress); <= -200000: glass (peeked, ghosted), -(200000 + opacity x 1000) (worldbuildings.cpp)
in vec4 tangent;    // xyz, w = handedness: binormal = cross(tangent, normal) x w (objects.hlsl:40)

out vec3 v_normal;
out vec3 v_tangent;
out vec3 v_binormal;
out vec2 v_uv;
out float v_dist;
out float v_valpha;
out vec3 v_vcol;
out vec3 v_wpos;    // world position
out vec3 v_lpos;    // object-space position (scaled)
out vec3 v_lnormal; // object-space normal
out vec3 v_cpos;    // position relative to the camera (shadow lookup)
out float v_row;    // biome table row, constant per instance
out vec3 v_build;   // construction: x = progress 0..1 (< 0: complete), y = building-local height / building height H, z = H; complete (x < 0): y = the building-local height itself (before the scale, the part's offset in), the foundation test of xray_fnd
out float v_glass;  // the opacity of a peeked part (renderGlass), else 1
out vec4 v_blood;   // a severed limb: the blood map's cylindrical UVs (severed_limb_vs: x = mesh x * 0.3, zw = the wrapped angle, w = mesh normal z)
flat out vec2 v_oxz;	// the instance origin's world xz (the dust coat's biome)

vec3 qrot(vec4 q, vec3 v)
{
	return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

vec2 ya = vec2(0.0, 1.0);	// the wind aim of this instance: sin, cos of the yaw (main() sets it)

// the wind aim's yaw about the building's vertical: local +Z goes to (sin, 0, cos)
vec3 yaw_rot(vec3 v)
{
	return vec3(v.x * ya.y + v.z * ya.x, v.y, v.z * ya.y - v.x * ya.x);
}

// the part's own spin, then the wind aim's yaw (directions; positions add the offset and pivot in main)
vec3 spin_rot(vec3 v)
{
	float c = cos(spin.w);
	float s = sin(spin.w);
	return yaw_rot(v * c + cross(spin.xyz, v) * s + spin.xyz * dot(spin.xyz, v) * (1.0 - c));
}

void main()
{
	// `rotation function` 4 (kinspect rotation_function4_wind_aim.md, live: windmill03 heads): a yaw about the building's vertical that puts the
	// part's local +Z on minus the wind, taken into the building frame and flattened; its nested parts (the rotor) turn with it about its origin.
	// A wind change turns it at a constant rate, the exe's linear slerp at `rotation speed max` x a factor in 0.95..1.05 picked once per part
	// (ours: per instance, hashed from its position, so a building's head and rotor share it); a yaw-only turn, so the slerp is a linear yaw.
	vec3 pv = vec3(0.0);
	if (aim.w > 0.5)
	{
		const float pi = 3.14159265;
		float a0 = atan(wind.x, wind.y);
		float d = mod(atan(wind.z, wind.w) - a0 + pi, 2.0 * pi) - pi;	// the short way round
		float h = fract(sin(dot(floor(inst_pos.xyz + camw.xyz + base.xyz), vec3(12.9898, 78.233, 37.719))) * 43758.5453);
		float dur = turn.y > 0.0 ? abs(d) / (turn.y * (0.95 + 0.1 * h)) : 1.0;
		float a = a0 + d * (dur > 0.0 ? clamp(turn.x / dur, 0.0, 1.0) : 1.0);
		vec3 w = qrot(vec4(-inst_quat.xyz, inst_quat.w), vec3(sin(a), 0.0, cos(a)));
		ya = normalize(-w.xz + vec2(0.0, 1e-6));
		pv = aim.xyz;
	}
	vec3 local = (spin_rot(position) + yaw_rot(offset.xyz - pv) + pv) * inst_stretch.xyz;
	vec3 p = inst_pos.xyz + base.xyz + qrot(inst_quat, local * inst_pos.w);
	// render.leaf_sway (a deliberate deviation: the original's trees do not move). tile.z = the tree's height, the tallest mesh of its record, so the
	// trunk and the leaves mesh take the same bend and stay joined. Every amplitude is the live wind strength (sway.x, 0 = still).
	// 1 the main bend from the ground along the wind (height^2); 2 the fronds: the leaves mesh's vertices away from the trunk axis swing more;
	// 3 a small fast flutter weighted like 2. kinspect: no authored per-vertex weights are used, the radial distance stands in.
	if (tile.z > 0.0 && sway.x > 0.0)
	{
		float H = tile.z;
		vec2 wp = (inst_pos.xyz + base.xyz + camw.xyz).xz;	// the tree's own phase offset: neighbours do not move in step
		float ph = sway.w * 1.9 + dot(wp, vec2(0.021, 0.013));
		float gust = sin(ph) * 0.7 + sin(sway.w * 0.7 + wp.x * 0.004) * 0.3;
		float hh = clamp(local.y / H, 0.0, 1.0);
		vec2 off = sway.yz * (H * 0.05 * sway.x * (0.6 + 0.4 * gust) * hh * hh);
		if (limb.y > 0.5)
		{
			float rw = clamp(length(local.xz) / (0.25 * H), 0.0, 1.0) * hh;	// frond weight: 0 at the trunk axis
			off += sway.yz * (H * 0.02 * sway.x * sin(ph * 1.7 + local.y * 0.15) * rw);
			off += vec2(sin(sway.w * 7.3 + local.x * 0.9), cos(sway.w * 6.1 + local.z * 0.9)) * (H * 0.004 * sway.x * rw);
		}
		p.xz += off * inst_pos.w;
	}
	vec3 ln = spin_rot(normal) / inst_stretch.xyz;
	v_normal = qrot(inst_quat, ln);
	// the tangent frame turns like positions (objects.hlsl: the binormal is made in mesh space, then both go through the world matrix)
	v_tangent = qrot(inst_quat, spin_rot(tangent.xyz) * inst_stretch.xyz);
	v_binormal = qrot(inst_quat, spin_rot(cross(tangent.xyz, normal) * tangent.w) * inst_stretch.xyz);
	v_lnormal = ln;
	v_lpos = local * inst_pos.w;
	v_wpos = p + camw.xyz;
	v_cpos = p;
	v_uv = texcoord0 * tile.xy;
	v_dist = length(p);
	v_valpha = colour.a;
	v_row = max(inst_stretch.w, 0.0);
	float h = floor(-inst_stretch.w);
	// a glass part (worldbuildings.cpp kGlassBuild): complete
	// a ghosted occluder's solid zone (wbdraw.cpp kGlassFndBuild / kFndOnlyBuild): -(210000 + opacity x 1000) its glass without the zone (v_build.z = 4),
	// -220000 the zone alone, opaque (v_build.z = 3); the fragment shader splits them by the height under xray_fnd
	float gw = -inst_stretch.w;
	float gbase = gw >= 210000.0 ? 210000.0 : 200000.0;
	v_glass = gw >= 220000.0 ? 1.0 : gw >= 200000.0 ? clamp((gw - gbase) / 1000.0, 0.0, 1.0) : 1.0;
	v_build = gw >= 200000.0 ? vec3(-1.0, local.y, gw >= 220000.0 ? 3.0 : gw >= 210000.0 ? 4.0 : 2.0) : inst_stretch.w < 0.0 ? vec3(-inst_stretch.w - h, local.y / h, h) : vec3(-1.0, local.y, 1.0);
	v_vcol = colour.rgb;
	v_oxz = (inst_pos.xyz + base.xyz + camw.xyz).xz;
	// a severed limb: severed_limb_vs (character.hlsl) maps the blood like a cylinder round the mesh's x axis, in the mesh (bind) frame
	v_blood = vec4(0.0);
	if (limb.x > 0.5)
	{
		float a = atan(normal.y, normal.z) / 3.14159265359 * 0.5;
		v_blood = vec4(position.x * 0.3, a, a > 0.0 ? a - 0.5 : a + 0.5, normal.z);
	}
	gl_Position = mvp * vec4(p, 1.0);
}
@end

@include origin.inc
@include lighting.inc
@include shadow.inc
@include fade.inc
@include biome.inc

@fs inst_fs
layout(binding=4) uniform texture2D shadow_tex;
layout(binding=2) uniform sampler shadow_smp;
@include_block origin
@include_block lighting
@include_block lighting_ao
@include_block lighting_vol
@include_block shadowing
layout(binding=0) uniform texture2D inst_tex;
layout(binding=1) uniform texture2D inst_nrm;
layout(binding=2) uniform texture2D inst_tex2;
layout(binding=3) uniform texture2D inst_ground;
layout(binding=14) uniform texture2D inst_nrm2;	// DUAL: `normal map 2`, texture 2's normal (flat without one)
layout(binding=0) uniform sampler inst_smp;
layout(binding=1) uniform sampler ground_smp;
layout(binding=5) uniform texture2D fog_map;
layout(binding=3) uniform sampler fog_smp;
layout(binding=6) uniform texture2DArray diffuse_maps;
layout(binding=7) uniform texture2DArray normal_maps;
layout(binding=8) uniform texture2D overlay_map;
layout(binding=9) uniform texture2D biome_table;
layout(binding=10) uniform texture2D ground_map;
layout(binding=11) uniform texture2D ground_alpha;
layout(binding=13) uniform texture2D dust_noise;	// data/materials/Turbulent.dds, the game's dustNoise unit (kenshi-inspect findings/surface_dust_coat.md)
layout(binding=15) uniform texture2D inst_mask;	// a severed limb's `color map` (skinMask: r = skin tone mask; character.material SeveredLimb)
layout(binding=18) uniform texture2D inst_blood;	// a severed limb's bloodMap: materials/bloody.png (wrap, the same inst_smp)
layout(binding=4) uniform sampler asmp;
layout(binding=5) uniform sampler lsmp;
layout(binding=6) uniform sampler tsmp;
@include_block biome
layout(binding=1) uniform inst_fs_params
{
	vec4 misc;      // x = alpha test cutoff 0..1 on the texture alpha (0 = none); y = mode: 0 plain, 1 ALPHA, 2 EMISSIVE, 3 DUAL, 4 TRIPLANAR, 5 TERRAIN, 6 DUAL_TRIPLANAR; z = 1: FOLIAGE mode (COLOURING: x vertex colour); w = 1: x tests the normal map's alpha (FOLIAGE)
	vec4 tri;       // xy = tile X/Y, z = 1: map in object space instead of world space, w = world units per texture repeat at tile 1
	vec4 fade;      // x = camera distance where the fade-out starts, y = where it ends (y = 0: no fade); z = how: 0 dithered, 1 the opaque pass (the band left out), 2 the band pass (blended)
	vec4 xray;      // the occluder fade: xyz = the focus character relative to the camera, w = the cone's widest radius (0: off)
	vec4 xray_ex0;  // the cone's exemption box (the building the camera is in / cut open): xyz = centre relative to the camera, w = 1 on
	vec4 xray_ex1;  // xyz = half extents, w = the yaw (rad)
	vec4 xray_fnd;  // x = the part's solid top, building-local height before scale: the foundation and the ground floor's slab (WorldBuildings::infoFor, interior bottom + kFloorTop); the fragments under it never fade (cone and glass); -1e30: none
	vec4 skin;      // a severed limb: skinTone rgb (SeveredLimbItem custom 2 = 1 - the victim's Skin Tone, from the wire)
	vec4 blood;     // a severed limb: bloodColour rgb (custom 3 = the race's `blood colour` 0..1, from the wire)
	vec4 misc2;     // x = 1: an interior material (the exe's INTERIOR flag, objects.hlsl:213): the dust coat takes dustAmount.y = surf.w
	vec4 flip;      // x > 0: the cut-away flip fade (renderFlip): seconds between the eye samples; the fragment fades by when its face last faced the eye
	vec4 flip_eye[8];   // xyz = the eye (k + 1) x flip.x seconds ago, relative to the eye now
};

in vec3 v_normal;
in vec3 v_tangent;
in vec3 v_binormal;
in vec2 v_uv;
in float v_dist;
in float v_valpha;
in vec3 v_vcol;
in vec3 v_wpos;
in vec3 v_lpos;
in vec3 v_lnormal;
in vec3 v_cpos;
in float v_row;
in vec3 v_build;
in float v_glass;
in vec4 v_blood;
flat in vec2 v_oxz;
out vec4 frag_color;
@include_block fade_ign

// Triplanar sample of the first (second = false) or second diffuse texture (rgb + gloss alpha): three axis projections blended by the squared normal.
vec4 tri_colour(vec3 p, vec3 n, bool second)
{
	vec3 w = n * n;
	w = w / (w.x + w.y + w.z);
	vec3 c = p * vec3(tri.x, tri.x, tri.y) / tri.w;	// triplanar.hlsl: (worldPos + worldOffset) / 5000 * tiling.xxy
	vec2 ux = c.yz;
	vec2 uy = c.xz;
	vec2 uz = c.xy;
	if (second)
	{
		return texture(sampler2D(inst_tex2, inst_smp), ux) * w.x + texture(sampler2D(inst_tex2, inst_smp), uy) * w.y + texture(sampler2D(inst_tex2, inst_smp), uz) * w.z;
	}
	return texture(sampler2D(inst_tex, inst_smp), ux) * w.x + texture(sampler2D(inst_tex, inst_smp), uy) * w.y + texture(sampler2D(inst_tex, inst_smp), uz) * w.z;
}

// Surface dust coat (deferred/objects.hlsl:211-225, triplanar.hlsl:134-143; the exe compiles DUST into every object, building and triplanar feature
// material, kenshi-inspect findings/surface_dust_coat.md): albedo towards dustColour on up-facing, matt, noise-selected spots; returns the dust weight in .a.
// dustColour is the BIOMES 'ground colour' of the biome under the object's origin (one biomemap texel, not blended), dustNoise Turbulent.dds.
vec4 dust_coat(vec3 albedo, float gloss, vec3 n, vec2 xz)
{
	float amount = misc2.x > 0.5 ? surf.w : surf.y;	// INTERIOR (objects.hlsl:213): interior parts take the weather's `dust inside`
	float noise = texture(sampler2D(dust_noise, inst_smp), xz * 0.002).x;
	float d = (clamp((n.y - 0.7 + surf.z) * 4.0, 0.0, 1.0) - gloss * 6.0 + 2.2 - noise * 6.0) * 2.0 + amount - 1.0;
	float w = clamp(d * min(1.0, amount * 8.0), 0.0, 1.0);
	ivec2 sz = textureSize(sampler2D(ground_map, lsmp), 0);
	vec3 col = texelFetch(sampler2D(ground_map, lsmp), clamp(ivec2((v_oxz + 147456.0) / 294912.0 * vec2(sz)), ivec2(0), sz - 1), 0).rgb;
	return vec4(mix(albedo, col, w), w);
}

void main()
{
	// the distance fade (kenshi-k16.2.63): a screen door (z = 0) shows its 1-px pattern on a still frame, so foliage draws the band apart instead: the opaque
	// pass leaves it out (z = 1) and the band pass (z = 2, after everything opaque: nearest surface into depth, then blended at that depth) gives it alpha
	float fadeT = fade.y > 0.0 ? clamp((v_dist - fade.x) / (fade.y - fade.x), 0.0, 1.0) : 0.0;
	if (fade.z == 0.0 ? ign(gl_FragCoord.xy) < fadeT : fade.z == 1.0 ? fadeT > 0.0 : fadeT <= 0.0 || fadeT >= 1.0) { discard; }
	// the occluder fade (docs/CLIENT.md "Camera modes"; ours): a dithered see-through cone from the focus character to the camera, narrow at the character so
	// the floor around it stays (the slope stays under the camera's pitch), nothing behind it
	bool xex = false;
	if (xray.w > 0.0 && xray_ex0.w > 0.0)
	{
		vec3 e = v_cpos - xray_ex0.xyz;
		float cy = cos(xray_ex1.w), sy = sin(xray_ex1.w);
		xex = abs(cy * e.x - sy * e.z) <= xray_ex1.x && abs(e.y) <= xray_ex1.y && abs(sy * e.x + cy * e.z) <= xray_ex1.z;
	}
	// a building's foundation never fades (kenshi-k16.2.411.1, user 2026-11-19): the part's fragments under the ground floor skip the cone, and a ghosted
	// occluder's glass draws them opaque; the base mesh holds foundation and ground walls in one, so this is per fragment, not per part
	bool fnd = v_build.x < 0.0 && v_build.y < xray_fnd.x;
	if (v_build.x < 0.0 && ((v_build.z > 2.5 && v_build.z < 3.5 && !fnd) || (v_build.z > 3.5 && fnd))) { discard; }	// the ghosted occluder's two draws: the solid zone opaque, the rest glass
	if (xray.w > 0.0 && !xex && !fnd)
	{
		float t = dot(v_cpos, xray.xyz) / dot(xray.xyz, xray.xyz);
		float back = (1.0 - t) * length(xray.xyz);	// distance from the character towards the camera
		float r = min(xray.w, 0.35 * back);
		float d = length(v_cpos - xray.xyz * t);
		if (t < 1.0 && back > 8.0 && ign(gl_FragCoord.xy) < 0.85 * (1.0 - smoothstep(0.7 * r, r, d))) { discard; }
	}
	// the building loading stand-in (kenshi-k16.2.258.4, wbdraw.cpp; misc2.y): 1 = flat black, unlit, no texture, no fog (the character silhouette, char_standin_fs);
	// the glass coding of the instance's stretch w carries the switch's cover in v_glass (the real look's share, 0..1): the black dithers away where
	// ign < cover, 2 = the real look dithers in where ign < cover, so the two draws of one instance partition the pixels. 0 = neither (an ordinary draw).
	if (misc2.y > 0.5)
	{
		bool real = misc2.y > 1.5;
		float t = ign(gl_FragCoord.xy);
		if (real ? t >= v_glass : t < v_glass) { discard; }
		if (!real) { frag_color = vec4(0.0, 0.0, 0.0, 1.0); return; }
	}
	// construction (docs/CLIENT.md "Construction"; ours, not the game's scaffold lattice): above the cut at progress x H a dithered ghost (a third of the pixels)
	// two-sided: the normal faces the viewer. gl_FrontFacing is not usable for this: the decoded meshes' winding is mirrored, so the outside of a roof reads as a back face
	// and was lit as facing down (found while adding shadows: roofs got no sun term). The test is the triangle's own plane (screen derivatives, turned to the viewer),
	// not the smoothed vertex normal: a deck vertex normal tilted 30 deg into a kerb points away from a low camera on a face seen from the front (greenlands_bridge
	// lanes went brown, reflecting the ground). Taken before any discard (derivatives).
	vec3 face = cross(dFdx(v_cpos), dFdy(v_cpos));
	face = dot(face, v_cpos) > 0.0 ? -face : face;
	bool back = dot(v_normal, face) < 0.0;
	bool ghost = v_build.x >= 0.0 && v_build.y > v_build.x;
	if (ghost && ign(gl_FragCoord.xy) > 0.33) { discard; }
	// the cut-away flip fade (renderFlip draws the faces the cull drops): the newest past eye this face's winding side faced, by the crossing time between samples;
	// alpha falls from 1 at the turn to 0 over the sample window, a face no sample saw is not drawn
	float flip_a = 1.0;
	if (flip.x > 0.0)
	{
		vec3 nw = -normalize(face);	// the side the winding shows: away from the eye, as this fragment is a culled one
		float f0 = dot(nw, -v_cpos);
		flip_a = 0.0;
		for (int k = 0; k < 8; k++)
		{
			float f1 = dot(nw, flip_eye[k].xyz - v_cpos);
			if (f1 > 0.0)
			{
				flip_a = 1.0 - (float(k) + clamp(-f0 / (f1 - f0), 0.0, 1.0)) / 8.0;
				break;
			}
			f0 = f1;
		}
		if (flip_a <= 0.0) { discard; }
	}
	// Some meshes carry zero vertex normals (24 vertices of TechRustyJunk_05.mesh): a triangle of them interpolates to 0 and normalize() gives NaN, one
	// pixel that the bloom spread into a black octagon (kenshi-k16.2.62). Such a pixel takes the triangle's own plane, already turned to the viewer.
	vec3 n = dot(v_normal, v_normal) > 1e-12 ? normalize(v_normal) : normalize(face + vec3(0.0, 1e-20, 0.0));
	if (back) { n = -n; }
	float mode = misc.y;
	vec3 base;
	float alpha = 1.0;
	float biome_abs = 0.0;	// TERRAIN mode: biome.absorbance (terrainfp4.hlsl:312)
	float gloss = 0.0;	// the gbuffer gloss, drives the environment specular: the diffuse alpha (objects.hlsl, triplanar.hlsl), the biome layers' gloss (TERRAIN mode)
	if (mode > 6.5)
	{
		// a ground severed limb (character.material SeveredLimb, character.hlsl severed_limb_fs): SeveredLimbItem binds the item's texture, normal and
		// color maps; body.rgb *= 1 - skinTone x mask.r, then the cylindrical blood x bloodColour laid over it, gloss = body.a, metalness 0 (the shared
		// light has F0 0.04 only). No dust and no makeWet: the original's SeveredLimb pass has neither.
		vec4 body = texture(sampler2D(inst_tex, inst_smp), v_uv);
		float mask = texture(sampler2D(inst_mask, inst_smp), v_uv).r;
		body.rgb *= 1.0 - skin.rgb * mask;
		gloss = body.a;
		vec2 bc = v_blood.w < 0.0 ? v_blood.xz : v_blood.xy;
		vec4 bl = texture(sampler2D(inst_blood, inst_smp), bc) * blood;
		bl.a += max(-v_blood.x * 2.0, 0.0);	// the cut end is all blood
		body.rgb = mix(body.rgb, bl.rgb, min(bl.a, 1.0));
		base = body.rgb * 0.7;	// DARKEN
	}
	else if (mode > 3.5)
	{
		// triplanar family: world (or object) space projection; the object-space normal is used for the blend in object space
		vec3 p = tri.z > 0.5 ? v_lpos : v_wpos;
		vec3 bn = tri.z > 0.5 ? normalize(v_lnormal) : n;
		if (mode < 4.5)
		{
			vec4 tc = tri_colour(p, bn, false);
			base = tc.rgb * v_vcol;	// triplanar.hlsl COLOURING: diffuse.rgb *= vertex colour
			gloss = tc.a;
		}
		else if (mode < 5.5)
		{
			// TERRAIN (mapfeature_fs, terrainfp4.hlsl:281): the record's texture is ignored; the mesh is textured like the terrain, with computeBiome of the one biome
			// under the instance (v_row), the overlay map for grass / dirt (road forced off) and the colour map x 1.2, the biome normal map perturbing the normal
			vec2 uv = (v_wpos.xz + 147456.0) / 294912.0;
			vec4 map = texture(sampler2D(overlay_map, lsmp), uv);
			map.r = max(map.r, map.g);
			map.a = 0.0;
			vec2 cb = pow(max((abs(normalize(n.xz + vec2(1e-5))) - 0.2) * 7.0, 0.0), vec2(2.0));
			cb = n.y > 0.9 ? vec2(1.0, 0.0) : cb / max(cb.x + cb.y, 1e-5);
			vec3 tn;
			vec4 bl = compute_biome(int(v_row + 0.5), n, cb, map, vec4(texture(sampler2D(inst_ground, ground_smp), uv).rgb, texture(sampler2D(ground_alpha, ground_smp), uv).r) * 1.2, v_dist, texture(sampler2D(ground_map, lsmp), uv).rgb, v_wpos, tn, biome_abs);
			base = bl.rgb;
			gloss = clamp(bl.a, 0.0, 1.0);
			vec3 b = normalize(cross(n, vec3(-1.0, 0.0, 0.0)));
			n = normalize(cross(b, n) * (tn.x * 2.0 - 1.0) + b * (tn.y * 2.0 - 1.0) + n * (tn.z * 2.0 - 1.0));
		} else {
			vec4 tc = mix(tri_colour(p, bn, true), tri_colour(p, bn, false), v_valpha);
			base = tc.rgb * v_vcol;
			gloss = tc.a;
		}
	} else {
		vec4 t = texture(sampler2D(inst_tex, inst_smp), v_uv);
		base = t.rgb;
		alpha = t.a;
		gloss = t.a;
		if (mode > 2.5)
		{
			vec4 t2 = mix(texture(sampler2D(inst_tex2, inst_smp), v_uv), t, v_valpha);
			base = t2.rgb * v_vcol;	// Object_Dual_FP = COLOURING + DUAL_TEXTURE (objects.hlsl): the blend, then diffuse.rgb *= vertex colour (black: the InteriorBLACK caps)
			gloss = t2.a;
		}
		else if (misc.z > 0.5) { base *= v_vcol; }	// FOLIAGE mode, Object_*Coloured_FP (COLOURING): diffuse.rgb *= vertex colour (white for meshes without)
	}
	if (misc.x > 0.0 && (misc.w > 0.5 ? texture(sampler2D(inst_nrm, inst_smp), v_uv).a : alpha) < misc.x) { discard; }
	// the normal map (objects.hlsl:175-190; objects are never DXT5NORMAL): rgb x 2 - 1 in the tangent frame, then turned to the viewer like n. A part or
	// submesh without one binds a flat map. DUAL: objects.hlsl lerps `normal map 2` in by vertex alpha, as the textures (vertex alpha 1: the first).
	// A degenerate frame (no UVs, a zero tangent) keeps the geometric normal.
	vec3 ng = n;
	if (mode > 6.5 && dot(v_tangent, v_tangent) > 1e-12 && dot(v_binormal, v_binormal) > 1e-12)
	{
		// a severed limb's normal map is the race body normal, DXT5 (x in alpha, y in green): the expansion of severed_limb_fs
		vec4 bodyN = texture(sampler2D(inst_nrm, inst_smp), v_uv);
		vec2 tn2 = bodyN.wy * 2.0 - 1.0;
		vec3 tn = vec3(tn2, sqrt(max(1.0 - dot(tn2, tn2), 0.0)));
		vec3 g = back ? -n : n;
		vec3 nm = normalize(mat3(normalize(v_tangent), normalize(v_binormal), g) * tn);
		n = back ? -nm : nm;
	}
	else if (mode < 3.5 && dot(v_tangent, v_tangent) > 1e-12 && dot(v_binormal, v_binormal) > 1e-12)
	{
		vec3 tn = texture(sampler2D(inst_nrm, inst_smp), v_uv).rgb * 2.0 - 1.0;
		if (mode > 2.5) { tn = mix(texture(sampler2D(inst_nrm2, inst_smp), v_uv).rgb * 2.0 - 1.0, tn, v_valpha); }
		vec3 g = back ? -n : n;	// normalize(v_normal), or the plane fallback above
		vec3 nm = normalize(mat3(normalize(v_tangent), normalize(v_binormal), g) * tn);
		n = back ? -nm : nm;
	}
	float glow = 0.0;
	if (mode > 0.5 && mode < 1.5) { float a = texture(sampler2D(inst_nrm, inst_smp), v_uv).a; if (a < 0.5) { discard; } }
	if (mode > 1.5 && mode < 2.5) { glow = texture(sampler2D(inst_nrm, inst_smp), v_uv).a; }
	// weather surface state (wetness.. dust coat, lighting.inc light_wet, makeWet): objects.hlsl (shader modes 0-3, FOLIAGE parts too: a StaticObject clone,
	// kinspect foliage_alpha_threshold.md) wets with absorbance 1 - gloss; mapfeature_fs (mode 5, terrainfp4.hlsl:313) wets like the terrain; triplanar.hlsl
	// (modes 4, 6) has no makeWet. Dust: not on TERRAIN features, nor on the construction ghost (the scaffold materials have no DUST); interiors would take
	// dustAmount.y (no interior flag here). The metalness map is not loaded (black: metal 0).
	if (mode < 6.5 && mode != 5.0 && surf.y > 0.0 && !ghost)	// not the TERRAIN features, nor a severed limb (SeveredLimb has no DUST)
	{
		vec4 dust = dust_coat(base, gloss, n, v_wpos.xz);
		base = dust.rgb;
		n = normalize(mix(n, ng, clamp(dust.a * 0.5, 0.0, 1.0)));	// objects.hlsl:225: dust flattens the normal map
	}
	bool wet = mode < 3.5 || mode == 5.0;
	float absorb = 1.0 - gloss + biome_abs;
	if (wet)
	{
		float under = wet_under(v_cpos, 0.5);
		base = light_wet(base, absorb, under);
		gloss = wet_gloss(gloss, absorb, under);
	}
	vec3 ln = n;
	// shared lighting: the game's deferred main light model (lighting.inc)
	float sh = shadow_visibility(v_cpos, n);
	vec4 ao = light_ao(v_cpos);
	vec3 bounce;	// render.sun_bounce: the sun's indirect light along the normal
	const vec2 vol = light_vol(v_cpos, ln, ng, bounce);	// interior light: the factor on the ambient (diffuse along the normal, specular the mean); ng pushes the lookup off the surface
	const float ao_spec = ao.a * vol.y;
	const float ao_bounce = ao.a;
	ao.a *= vol.x;
	vec3 vw = -normalize(v_cpos);
	if (ghost) { base = mix(vec3(dot(base, vec3(0.3, 0.59, 0.11))), vec3(0.55, 0.72, 0.9), 0.6); }
	vec3 lit = light_shade(base, ln, sh, ao) + light_bounce(base, bounce, ao_bounce) + base * (glow * emissive_on()) + light_spec(ln, vw, clamp(gloss, 0.0, 1.0), ao_spec) + light_spec_sun(ln, vw, clamp(gloss, 0.0, 1.0), sh)
		+ light_points_vol(base, ln, v_cpos, clamp(gloss, 0.0, 1.0));
	if (ghost) { lit *= 1.0 + 2.0 * pow(1.0 - abs(dot(n, vw)), 3.0); }	// fresnel rim: the outline reads at a glance
	else if (v_build.x >= 0.0 && !reflect_pass() && !term_off(LT_CUTBAND)) { lit += vec3(1.0, 0.6, 0.25) * 1.5 * (1.0 - smoothstep(0.0, 3.0, (v_build.x - v_build.y) * v_build.z)); }	// warm band in the 3 units under the cut
	frag_color = vec4(light_finish(lit, v_cpos, FOG_MAP_RAY(fog_map, fog_smp, v_wpos.xz, v_cpos.xz)), v_glass * flip_a * (fade.z == 2.0 ? 1.0 - fadeT : 1.0));
}
@end

@image_sample_type shadow_tex depth
@sampler_type shadow_smp comparison
@sampler_type shadow_raw nonfiltering
@image_sample_type biome_table unfilterable_float
@sampler_type tsmp nonfiltering

// Shadow map pass: the instance vertex shader with mvp = the cascade matrix; the fragment shader only discards cut-out texels (alpha test, ALPHA-mode normal map alpha).
@fs shadow_fs
layout(binding=0) uniform texture2D inst_tex;
layout(binding=1) uniform texture2D inst_nrm;
layout(binding=0) uniform sampler inst_smp;
layout(binding=1) uniform shadow_fs_params
{
	vec4 cut;       // x = alpha test cutoff 0..1 (0 = none), y = 1: ALPHA mode, discard where the normal map alpha < 0.5
	vec4 fade;      // x = fade start distance, y = fade end distance from the camera (y = 0: none), as in inst_fs
	vec4 bias;      // caster_bias (shadow.inc)
};

in vec3 v_normal;
in vec3 v_tangent;
in vec3 v_binormal;
in vec2 v_uv;
in float v_dist;
in float v_valpha;
in vec3 v_vcol;
in vec3 v_wpos;
in vec3 v_lpos;
in vec3 v_lnormal;
in vec3 v_cpos;
in float v_row;
in vec3 v_build;
in float v_glass;
in vec4 v_blood;	// declared, unused: sokol-shdc wants the vs outputs and the fs inputs to match one to one
flat in vec2 v_oxz;
@include_block fade_ign
@include_block caster_bias

void main()
{
	caster_bias(bias);
	if (fade.y > 0.0 && ign(gl_FragCoord.xy) < clamp((v_dist - fade.x) / (fade.y - fade.x), 0.0, 1.0)) { discard; }
	if (v_build.x >= 0.0 && v_build.y > v_build.x) { discard; }	// the construction ghost casts no shadow
	if (cut.x > 0.0 && texture(sampler2D(inst_tex, inst_smp), v_uv).a < cut.x) { discard; }
	if (cut.y > 0.5 && texture(sampler2D(inst_nrm, inst_smp), v_uv).a < 0.5) { discard; }
}
@end

// Cursor ID pass (idbuffer.hpp): the instance vertex shader again, depth-tested EQUAL against the scene depth, so only the surface the scene shows
// writes; r = the building's id (instance index + 1), gba = the position relative to the camera.
@fs id_fs
layout(binding=1) uniform id_fs_params
{
	vec4 id;        // x = building instance index + 1
};

in vec3 v_normal;
in vec3 v_tangent;
in vec3 v_binormal;
in vec2 v_uv;
in float v_dist;
in float v_valpha;
in vec3 v_vcol;
in vec3 v_wpos;
in vec3 v_lpos;
in vec3 v_lnormal;
in vec3 v_cpos;
in float v_row;
in vec3 v_build;
in float v_glass;
in vec4 v_blood;	// declared, unused: sokol-shdc wants the vs outputs and the fs inputs to match one to one
flat in vec2 v_oxz;
out vec4 frag_color;

void main()
{
	frag_color = vec4(id.x, v_cpos);
}
@end

@program building inst_vs inst_fs
@program building_shadow inst_vs shadow_fs
@program building_id inst_vs id_fs
