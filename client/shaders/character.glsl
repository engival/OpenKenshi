// Skinned characters (src/characters/characters.cpp): the game's hardware skinning (data/materials/deferred/skin.hlsl main_vs: WEIGHTS 3, BONES 60, a 3x4 matrix per bone)
// and its body shader (character.hlsl main_fs: body + head texture sets in one mesh, the head at uv.y + 1), shaded with the shared deferred light (lighting.inc).
@vs char_vs
// One instanced draw per (mesh, material) group (kenshi-k16.2.205.8): the per-view block is only the matrix; everything per character is read from the
// frame's skin buffer (row base = inst_ids.x: 180 bone rows, origin, blood x2, blood_scale, bone_part x15; Characters::kSkinRows) and everything per
// part from the instance record (inst_ids.y, read in the fragment stage).
layout(binding=0) uniform char_vs_view
{
	mat4 mvp;       // projection * rotation-only view (camera-relative)
};
struct row { vec4 v; };
layout(binding=28) readonly buffer skins { row sk[]; };

in vec3 position;
in vec3 normal;
in vec2 texcoord0;
in vec4 tangent;    // xyz, w = handedness
in uvec4 bone_idx;  // up to 4 bone indices (UBYTE4: an integer attribute in sokol, so uvec4); the shader uses the first 3 like the game
in vec4 bone_wgt;
in float part_mask;	// partData.y: the RACE part map bits at the vertex uv (140071f90; 0 off the body mesh)
in uvec2 inst_ids;	// per instance: x = the character's first row in `skins`, y = its record in the fragment stage's `insts`

out vec3 v_normal;
out vec3 v_tangent;
out vec3 v_binormal;
out vec2 v_uv;
out vec3 v_cpos;    // position relative to the camera
out vec4 v_blood;
out vec2 v_blood_amt;	// skin.hlsl oWet.y / z: x = the part's blood amount, y = 1 on hidden skin
flat out uint v_inst;
flat out float v_cover;	// the loading stand-in's dither coverage (sk row 199.y): 1 everywhere but a build that is fading its real look in

vec3 bone_mul(uint base, uint b, vec4 p)
{
	return vec3(dot(sk[base + b * 3u].v, p), dot(sk[base + b * 3u + 1u].v, p), dot(sk[base + b * 3u + 2u].v, p));
}

void main()
{
	const uint base = inst_ids.x;
	// skin.hlsl: blendPos += float4(M p, 1) w over WEIGHTS 3, then the w divide (oWorldPos = blendPos / blendPos.w): the 4th weight is dropped and the rest renormalised
	vec4 bp = vec4(0.0);
	vec3 bn = vec3(0.0), bt = vec3(0.0);
	for (int i = 0; i < 3; i++)
	{
		uint b = bone_idx[i];
		float w = bone_wgt[i];
		bp += vec4(bone_mul(base, b, vec4(position, 1.0)), 1.0) * w;
		bn += bone_mul(base, b, vec4(normal, 0.0)) * w;
		bt += bone_mul(base, b, vec4(tangent.xyz, 0.0)) * w;
	}
	vec3 p = bp.xyz / max(bp.w, 1e-6) + sk[base + 180u].v.xyz;
	v_normal = bn;
	v_tangent = bt;
	// the meshes carry no binormal; severed_limb_vs (character.hlsl) builds it as cross(tangent, normal) x tangent.w
	v_binormal = cross(bt, bn) * tangent.w;
	v_uv = texcoord0;
	v_cpos = p;
	v_inst = inst_ids.y;
	v_cover = sk[base + 199u].v.y;
	// skin.hlsl BLOOD: a cylinder projection of the mesh (bind) position and normal; the amount of the vertex's body part
	int hb = 0;
	for (int i = 1; i < 4; i++) { if (bone_wgt[i] > bone_wgt[hb]) { hb = i; } }
	uint pb = bone_idx[hb];
	vec4 bp4 = sk[base + 184u + pb / 4u].v;	// a local copy: the component index below is dynamic
	int part = int(bp4[pb % 4u]);
	vec4 ba4 = sk[base + 181u + uint(part / 4)].v;
	vec4 bs = sk[base + 183u].v;	// bloodScale: x = RACE blood horizontal, y = blood vertical; z = hiddenMask (140531cc0: the worn items' `hide parts` bits)
	// PART_MASK: oWet.z = (partData.y & hiddenMask) == 0 ? 0 : 1
	v_blood_amt = vec2(ba4[part % 4], (int(part_mask) & int(bs.z)) == 0 ? 0.0 : 1.0);
	float a = atan(normal.x, normal.z) / 3.14159265359 * 0.5;
	v_blood = vec4(position.y * bs.y, a * bs.x, (a > 0.0 ? a - 0.5 : a + 0.5) * bs.x, normal.z);
	gl_Position = mvp * vec4(p, 1.0);
}
@end

// The shadow cascades' skinning: positions and the clip uv only
@vs char_shadow_vs
layout(binding=0) uniform char_vs_view
{
	mat4 mvp;
};
struct row { vec4 v; };
layout(binding=28) readonly buffer skins { row sk[]; };
in vec3 position;
in vec2 texcoord0;
in uvec4 bone_idx;
in vec4 bone_wgt;
in uvec2 inst_ids;
out vec2 v_uv;
flat out uint v_inst;
void main()
{
	const uint base = inst_ids.x;
	vec4 bp = vec4(0.0);
	for (int i = 0; i < 3; i++)
	{
		uint b = bone_idx[i];
		vec4 p4 = vec4(position, 1.0);
		bp += vec4(dot(sk[base + b * 3u].v, p4), dot(sk[base + b * 3u + 1u].v, p4), dot(sk[base + b * 3u + 2u].v, p4), 1.0) * bone_wgt[i];
	}
	v_uv = texcoord0;
	v_inst = inst_ids.y;
	gl_Position = mvp * vec4(bp.xyz / max(bp.w, 1e-6) + sk[base + 180u].v.xyz, 1.0);
}
@end

// The loading stand-in (kenshi-k16.2.258.1, user 2026-11-15: "a body double rendered as a pure black silhouette ... it *looks* like it's loading,
// never gives false impressions as to what its identity is"). A build whose GPU objects are not ready yet is drawn as its race's prewarmed body,
// skinned by its own palette (its drawn node and animation). Stage 1 (Characters::kStandinShape): still in the stand-in's material, the body's
// shape mixes from the race base body (buffer 2, `position1`) into this build's morphed body (buffer 0): Characters::morphed bakes positions only,
// in the source mesh's order, so the two buffers line up vertex for vertex and no index or attribute differs. Stage 2 (kStandinColour): the real
// look fades in while this one dithers out over the same pixels. Row 199 of the skins buffer carries the clocks: x = the shape t, y = the real
// look's dither coverage, z = this one's for the body, w = for the parts (they cannot morph, so they dissolve in during stage 1). inst_ids.y says
// which: 0 = the body, else a part.
@vs char_standin_vs
layout(binding=0) uniform char_vs_view
{
	mat4 mvp;
};
struct row { vec4 v; };
layout(binding=28) readonly buffer skins { row sk[]; };
in vec3 position;	// this build's body (the race base body while it is not drawable)
in vec3 position1;	// the race base body's vertex at the same index (vertex buffer 2)
in vec3 normal;
in uvec4 bone_idx;
in vec4 bone_wgt;
in uvec2 inst_ids;
out vec3 v_normal;
out vec3 v_cpos;
flat out float v_cover;
void main()
{
	const uint base = inst_ids.x;
	const vec3 p = mix(position1, position, sk[base + 199u].v.x);
	vec4 bp = vec4(0.0);
	vec3 bn = vec3(0.0);
	for (int i = 0; i < 3; i++)
	{
		const uint b = bone_idx[i];
		const vec4 p4 = vec4(p, 1.0);
		bp += vec4(dot(sk[base + b * 3u].v, p4), dot(sk[base + b * 3u + 1u].v, p4), dot(sk[base + b * 3u + 2u].v, p4), 1.0) * bone_wgt[i];
		bn += vec3(dot(sk[base + b * 3u].v, vec4(normal, 0.0)), dot(sk[base + b * 3u + 1u].v, vec4(normal, 0.0)), dot(sk[base + b * 3u + 2u].v, vec4(normal, 0.0))) * bone_wgt[i];
	}
	v_normal = bn;
	v_cpos = bp.xyz / max(bp.w, 1e-6) + sk[base + 180u].v.xyz;
	v_cover = inst_ids.y == 0u ? sk[base + 199u].v.z : sk[base + 199u].v.w;
	gl_Position = mvp * vec4(v_cpos, 1.0);
}
@end

@include origin.inc
@include lighting.inc
@include shadow.inc
@include fade.inc

// Tangent-space normal to view space. Some vertices carry a zero tangent (the waist rim of cargopants_f.mesh): normalize() would give NaN, which the HDR pass shows
// as white, so a degenerate frame falls back to the geometric normal.
@block tbn
vec3 tbn_normal(vec3 ng, vec3 t, vec3 b, vec3 tn)
{
	if (dot(t, t) < 1e-12 || dot(b, b) < 1e-12) { return ng; }
	return normalize(mat3(normalize(t), normalize(b), ng) * tn);
}
@end

@fs char_fs
layout(binding=4) uniform texture2D shadow_tex;
layout(binding=2) uniform sampler shadow_smp;
@include_block origin
@include_block lighting
@include_block lighting_ao
@include_block lighting_vol
@include_block shadowing
@include_block fade_ign
@include_block tbn
layout(binding=0) uniform texture2D body_tex;   // RACE body texture
layout(binding=1) uniform texture2D body_nrm;   // RACE nm (DXT5 normal: x in alpha, y in green)
layout(binding=2) uniform texture2D body_mask;  // RACE body mask: r = skin tone mask
layout(binding=3) uniform texture2D head_tex;   // HEAD texture map, sampled at uv.y + 1
layout(binding=6) uniform texture2D head_nrm;   // HEAD normal map
layout(binding=7) uniform texture2D head_mask;  // HEAD mask map: r = skin tone mask, g = glow (eyes)
layout(binding=8) uniform texture2D vest_tex;   // clothing texture set (nothing.dds: none)
layout(binding=9) uniform texture2D vest_nrm;   // clothing normal, alpha = coverage
layout(binding=14) uniform texture2D body_blend;	// bodyBlendNormal: the RACE skinny or strong nm, mixed over body_nrm by muscleBlend
layout(binding=15) uniform texture2D blood_tex;	// bloodMap: materials/bloody.png (character.material, wrap)
layout(binding=1) uniform sampler blood_smp;
layout(binding=13) uniform texture2D vest_cmap;	// colorMap: the shirt's `vest colormap`, r = tint coverage (nothing.dds: uncoloured, white.dds: no map)
layout(binding=10) uniform texture2D hair_tex;  // hairMap: the hair ATTACHMENT's head texture (stubble), at the head uvs (nothing.dds: none)
layout(binding=11) uniform texture2D beard_tex;	// beardMap: the beard ATTACHMENT's head texture (nothing.dds: none)
layout(binding=5) uniform texture2D fog_map;
layout(binding=0) uniform sampler border_smp;   // character.material: tex_address_mode border, colour 0 0 0 0 (the body and head halves add up)
layout(binding=3) uniform sampler fog_smp;
struct row { vec4 v; };
layout(binding=29) readonly buffer insts { row ir[]; };	// per (character, part) records, 9 rows (Characters::kInstRows)
layout(binding=1) uniform view_fs { vec4 cam; };	// xyz = camera world position

in vec3 v_normal;
in vec3 v_tangent;
in vec3 v_binormal;
in vec2 v_uv;
in vec3 v_cpos;
in vec4 v_blood;
in vec2 v_blood_amt;
flat in uint v_inst;
flat in float v_cover;
out vec4 frag_color;

void main()
{
	// the stand-in's two-stage switch (kenshi-k16.2.258.1): the real look dithers in where the noise is below its coverage; the stand-in is
	// drawn over the rest of the same pixels with the complementary rule
	if (ign(gl_FragCoord.xy) >= v_cover) { discard; }
	const uint ib = v_inst * 9u;
	const vec4 skintone = ir[ib].v;	// rgb = the skintone uniform (body.rgb *= 1 - skintone x mask), w = muscleBlend
	const vec4 hair_col = ir[ib + 1u].v;	// hairColor (rgb)
	const vec4 hair_mult = ir[ib + 2u].v;	// hairMult: one-hot channel of the hair map's colour amount (ATTACHMENT head channel)
	const vec4 hair_alpha = ir[ib + 3u].v;	// hairAlpha: one-hot channel of its coverage (head alpha channel)
	const vec4 vest_col = ir[ib + 4u].v;	// color: the shirt colour (COLOR_DATA color 1)
	const vec4 beard_alpha = ir[ib + 5u].v;	// beardAlpha: one-hot coverage channel of the beard ATTACHMENT's head texture (its head channel; zero = none)
	const vec4 blood_col = ir[ib + 6u].v;	// bloodColour: RACE blood colour, alpha 1
	// character.hlsl: clip(-wet.z): skin under clothing that hides its part (a triangle with one hidden corner goes whole)
	if (v_blood_amt.y > 0.0) { discard; }
	vec2 uv = v_uv;
	// Ogre's default addressing is wrap: an animal mesh (the goat's u runs 0..2, its right half) reads the texture again past 1; the border sampler is for
	// the humans' head set at v < 0 only, and would give black here (kenshi-k16.2.168)
	if (uv.x > 1.0) { uv.x = fract(uv.x); }
	vec4 body = texture(sampler2D(body_tex, border_smp), uv);
	vec4 bodyN = mix(texture(sampler2D(body_nrm, border_smp), uv), texture(sampler2D(body_blend, border_smp), uv), skintone.w);
	float skin = texture(sampler2D(body_mask, border_smp), uv).r;
	// head: the same mesh, its uvs at v < 0; border 0 makes the two texture sets add up
	vec2 huv = uv + vec2(0.0, 1.0);
	body += texture(sampler2D(head_tex, border_smp), huv);
	bodyN += texture(sampler2D(head_nrm, border_smp), huv);
	vec2 hm = texture(sampler2D(head_mask, border_smp), huv).rg;
	skin += hm.r;
	body.rgb *= 1.0 - skintone.rgb * skin;
	// hair and beard painted on the head (character.hlsl hairMap / beardMap, both in the hair colour)
	vec4 hair = texture(sampler2D(hair_tex, border_smp), huv);
	float hairValue = clamp(dot(hair, hair_alpha), 0.0, 1.0);
	body.rgb = mix(body.rgb, hair_col.rgb * clamp(dot(hair, hair_mult), 0.0, 1.0), hairValue);
	float beardValue = clamp(dot(texture(sampler2D(beard_tex, border_smp), huv), beard_alpha), 0.0, 1.0);
	body.rgb = mix(body.rgb, hair_col.rgb * beardValue, beardValue);
	float gloss = body.a;
	// clothing
	vec4 vest = texture(sampler2D(vest_tex, border_smp), uv);
	vec4 vestN = texture(sampler2D(vest_nrm, border_smp), uv);
	vest.rgb = mix(vest.rgb, vest_col.rgb * ((vest.r + vest.g + vest.b) / 3.0), texture(sampler2D(vest_cmap, border_smp), uv).r);
	body.rgb = mix(body.rgb, vest.rgb, vestN.a);
	bodyN.wy = mix(bodyN.wy, vestN.xy, vestN.a);
	gloss = mix(gloss, vest.a, vestN.a);
	gloss *= mix(1.0, 0.4, max(hairValue, beardValue));	// "Stop texture hair being super shiny"
	// blood: the texture's alpha against the part's amount (bv = a - 1 + amount); the back half reads the wrapped column
	vec2 bc = v_blood.xy;
	if (v_blood.w < 0.0) { bc.y = v_blood.z; }
	vec4 bcol = texture(sampler2D(blood_tex, blood_smp), bc) * blood_col;
	body.rgb = mix(body.rgb, bcol.rgb, clamp(bcol.a - 1.0 + clamp(v_blood_amt.x, 0.0, 1.0), 0.0, 1.0));
	// DXT5 normal
	vec3 tn;
	tn.xy = bodyN.wy * 2.0 - 1.0;
	tn.z = sqrt(max(1.0 - dot(tn.xy, tn.xy), 0.0));
	vec3 ng = normalize(v_normal);
	vec3 n = tbn_normal(ng, v_tangent, v_binormal, tn);
	// makeWet: skin absorbs less than clothing
	float absorb = (1.0 - gloss * 2.0) * (clamp(vestN.a + 1.0 - skin, 0.0, 1.0) * 0.8 + 0.2);
	float under = wet_under(v_cpos, 0.5);
	vec3 base = light_wet(body.rgb * 0.7, absorb, under);	// DARKEN 0.7
	gloss = clamp(wet_gloss(gloss, absorb, under), 0.0, 1.0);
	float sh = shadow_visibility(v_cpos, ng);
	vec3 vw = -normalize(v_cpos);
	vec4 ao = light_ao(v_cpos);
	vec3 bounce;	// render.sun_bounce
	const vec2 vol = light_vol(v_cpos, n, ng, bounce);	// interior light (lighting.inc lighting_vol)
	const float ao_spec = ao.a * vol.y;
	const float ao_bounce = ao.a;
	ao.a *= vol.x;
	// glowy eyes: buf1.a = headMask.g x 0.5 + 0.5 where g > 0; deferred.hlsl:153 reads emissive = (a - 0.5) x 6.4 = g x 3.2 and adds albedo x emissive (exposed)
	vec3 lit = light_shade(base, n, sh, ao) + light_bounce(base, bounce, ao_bounce) + base * (hm.g * 3.2 * fog_col.w * emissive_on()) + light_spec(n, vw, gloss, ao_spec) + light_spec_sun(n, vw, gloss, sh) + light_points_vol(base, n, v_cpos, gloss);
	vec3 wpos = v_cpos + cam.xyz;
	frag_color = vec4(light_finish(lit, v_cpos, FOG_MAP_RAY(fog_map, fog_smp, wpos.xz, v_cpos.xz)), 1.0);
}
@end

@image_sample_type shadow_tex depth
@sampler_type shadow_smp comparison
@sampler_type shadow_raw nonfiltering

// Worn armour meshes: skin.hlsl main_fs (the Skinned material: colorMap default colormap_default.dds). Not yet: COLOURING (color1 / color2 by cmap.r / g; where the exe
// takes the colours from is not traced) and metalness (cmap.b; the shared lighting has F0 0.04 only, like mesh.glsl).
@fs armour_fs
layout(binding=4) uniform texture2D shadow_tex;
layout(binding=2) uniform sampler shadow_smp;
@include_block origin
@include_block lighting
@include_block lighting_ao
@include_block lighting_vol
@include_block shadowing
@include_block tbn
@include_block fade_ign
layout(binding=0) uniform texture2D armour_tex;	// MATERIAL_SPECS_CLOTHING texture map (alpha = gloss)
layout(binding=1) uniform texture2D armour_nrm;	// normal map: rgb, green flipped; alpha < 0.6 cut out
layout(binding=2) uniform texture2D armour_cmap;	// MATERIAL_SPECS_CLOTHING color map: r / g = color1 / color2 coverage, b = metalness (nothing.dds: neither)
layout(binding=5) uniform texture2D fog_map;
layout(binding=0) uniform sampler wrap_smp;
layout(binding=3) uniform sampler fog_smp;
struct row { vec4 v; };
layout(binding=29) readonly buffer insts { row ir[]; };	// per (character, part) records, 9 rows (Characters::kInstRows)
layout(binding=1) uniform view_fs { vec4 cam; };	// xyz = camera world position

in vec3 v_normal;
in vec3 v_tangent;
in vec3 v_binormal;
in vec2 v_uv;
in vec3 v_cpos;
in vec4 v_blood;	// unused here (char_vs feeds the body)
in vec2 v_blood_amt;
flat in uint v_inst;
flat in float v_cover;
out vec4 frag_color;

void main()
{
	if (ign(gl_FragCoord.xy) >= v_cover) { discard; }	// the stand-in's switch (character.glsl char_fs)
	const uint ib = v_inst * 9u;
	const vec4 color1 = ir[ib].v;	// COLOR_DATA color 1 (setAsARGB of a 24-bit int: a = 0, so the tint takes the texture's grey level)
	const vec4 color2 = ir[ib + 1u].v;
	const vec4 gloss = ir[ib + 2u].v;	// x = specular mult (glossMult)
	vec4 diffuse = texture(sampler2D(armour_tex, wrap_smp), v_uv, -2.0);	// the Skinned material's mipmap_bias -2
	vec4 nt = texture(sampler2D(armour_nrm, wrap_smp), v_uv);
	if (nt.a < 0.6) { discard; }
	// skin.hlsl COLOURING: the colours scaled by mix(intensity, 1, color.a), color2 laid over color1
	vec3 cmap = texture(sampler2D(armour_cmap, wrap_smp), v_uv).rgb;
	float intensity = (diffuse.r + diffuse.g + diffuse.b) / 3.0;
	diffuse.rgb = mix(diffuse.rgb, color1.rgb * mix(intensity, 1.0, color1.a), cmap.r);
	diffuse.rgb = mix(diffuse.rgb, color2.rgb * mix(intensity, 1.0, color2.a), cmap.g);
	float metal = cmap.b;
	nt.g = 1.0 - nt.g;
	vec3 ng = normalize(v_normal);
	vec3 n = tbn_normal(ng, v_tangent, v_binormal, nt.rgb * 2.0 - 1.0);
	float g = diffuse.a * gloss.x;
	float absorb = (1.0 - g * 2.0) * (1.0 - metal);
	float under = wet_under(v_cpos, 0.5);
	vec3 base = light_wet(diffuse.rgb * 0.8, absorb, under);	// DARKEN 0.8
	g = clamp(wet_gloss(g, absorb, under), 0.0, 1.0);
	float sh = shadow_visibility(v_cpos, ng);
	vec3 vw = -normalize(v_cpos);
	vec4 ao = light_ao(v_cpos);
	vec3 bounce;	// render.sun_bounce
	const vec2 vol = light_vol(v_cpos, n, ng, bounce);	// interior light (lighting.inc lighting_vol)
	const float ao_spec = ao.a * vol.y;
	const float ao_bounce = ao.a;
	ao.a *= vol.x;
	vec3 f0 = mix(vec3(0.04), base, metal);
	vec3 lit = light_shade_f0(base * (1.0 - metal), n, sh, f0, ao) + light_bounce(base, bounce, ao_bounce) + light_spec_f0(n, vw, g, f0, ao_spec) + light_spec_sun_f0(n, vw, g, sh, f0);
	vec3 wpos = v_cpos + cam.xyz;
	frag_color = vec4(light_finish(lit, v_cpos, FOG_MAP_RAY(fog_map, fog_smp, wpos.xz, v_cpos.xz)), 1.0);
}
@end

// Hair meshes: character.hlsl hair_fs (the Hair material, cull none). Brightness and coverage are channels of one texture; the geometric normal, metalness 0, no makeWet.
@fs hair_fs
layout(binding=4) uniform texture2D shadow_tex;
layout(binding=2) uniform sampler shadow_smp;
@include_block origin
@include_block lighting
@include_block lighting_ao
@include_block lighting_vol
@include_block shadowing
@include_block fade_ign
layout(binding=0) uniform texture2D hair_map;	// ATTACHMENT texture map
layout(binding=5) uniform texture2D fog_map;
layout(binding=0) uniform sampler wrap_smp;
layout(binding=3) uniform sampler fog_smp;
struct row { vec4 v; };
layout(binding=29) readonly buffer insts { row ir[]; };	// per (character, part) records, 9 rows (Characters::kInstRows)
layout(binding=1) uniform view_fs { vec4 cam; };	// xyz = camera world position

in vec3 v_normal;
in vec3 v_tangent;
in vec3 v_binormal;
in vec2 v_uv;
in vec3 v_cpos;
in vec4 v_blood;	// unused here (char_vs feeds the body)
in vec2 v_blood_amt;
flat in uint v_inst;
flat in float v_cover;
out vec4 frag_color;

void main()
{
	if (ign(gl_FragCoord.xy) >= v_cover) { discard; }	// the stand-in's switch (character.glsl char_fs)
	const uint ib = v_inst * 9u;
	const vec4 diffuse_ch = ir[ib].v;	// diffuseChannel: one-hot (hair diffuse channel)
	const vec4 spec = ir[ib + 1u].v;	// x = specularMult, y = mip bias (ATTACHMENT mipmap bias)
	const vec4 color = ir[ib + 2u].v;	// rgb = hair colour, w = threshold (alpha rejection / 255)
	const vec4 alpha_ch = ir[ib + 7u].v;	// alphaChannel: one-hot (hair alpha channel)
	vec4 hair = texture(sampler2D(hair_map, wrap_smp), v_uv, spec.y);
	if (dot(hair, alpha_ch) < color.w) { discard; }
	float brightness = dot(hair, diffuse_ch);
	vec3 n = normalize(v_normal);
	if (gl_FrontFacing == false) { n = -n; }	// two-sided cards: the gbuffer would take the face's normal as it is; flipped so the back lights like the front
	vec3 base = brightness * color.rgb * 0.7;	// DARKEN 0.7
	float g = clamp(brightness * spec.x, 0.0, 1.0);
	float sh = shadow_visibility(v_cpos, n);
	vec3 vw = -normalize(v_cpos);
	vec4 ao = light_ao(v_cpos);
	vec3 bounce;	// render.sun_bounce
	const vec2 vol = light_vol(v_cpos, n, n, bounce);	// interior light (lighting.inc lighting_vol)
	const float ao_spec = ao.a * vol.y;
	const float ao_bounce = ao.a;
	ao.a *= vol.x;
	vec3 lit = light_shade(base, n, sh, ao) + light_bounce(base, bounce, ao_bounce) + light_spec(n, vw, g, ao_spec) + light_spec_sun(n, vw, g, sh) + light_points_vol(base, n, v_cpos, g);
	vec3 wpos = v_cpos + cam.xyz;
	frag_color = vec4(light_finish(lit, v_cpos, FOG_MAP_RAY(fog_map, fog_smp, wpos.xz, v_cpos.xz)), 1.0);
}
@end

// render.loading_standin silhouette (the default): flat black, unlit, no textures, no fog. It *looks* like it is loading and never reads as an
// identity (user 2026-11-15). No depth trick: it writes depth like a body.
@fs char_standin_fs
@include_block fade_ign
in vec3 v_normal;
in vec3 v_cpos;
flat in float v_cover;
out vec4 frag_color;
void main()
{
	if (ign(gl_FragCoord.xy) < v_cover) { discard; }
	frag_color = vec4(0.0, 0.0, 0.0, 1.0);
}
@end

// render.loading_standin double: the race body in a plain neutral material, under the shared light (the geometric normal: no maps, so nothing of
// the character's own look shows).
@fs char_standin_lit_fs
layout(binding=4) uniform texture2D shadow_tex;
layout(binding=2) uniform sampler shadow_smp;
@include_block origin
@include_block lighting
@include_block lighting_ao
@include_block lighting_vol
@include_block shadowing
@include_block fade_ign
layout(binding=5) uniform texture2D fog_map;
layout(binding=3) uniform sampler fog_smp;
layout(binding=1) uniform view_fs { vec4 cam; };
in vec3 v_normal;
in vec3 v_cpos;
flat in float v_cover;
out vec4 frag_color;
void main()
{
	if (ign(gl_FragCoord.xy) < v_cover) { discard; }
	vec3 n = normalize(v_normal);
	vec3 base = vec3(0.5);	// MINE: a neutral grey, the plain double's own material
	float sh = shadow_visibility(v_cpos, n);
	vec3 vw = -normalize(v_cpos);
	vec4 ao = light_ao(v_cpos);
	vec3 bounce;	// render.sun_bounce
	const vec2 vol = light_vol(v_cpos, n, n, bounce);	// interior light (lighting.inc lighting_vol)
	const float ao_spec = ao.a * vol.y;
	const float ao_bounce = ao.a;
	ao.a *= vol.x;
	vec3 lit = light_shade(base, n, sh, ao) + light_bounce(base, bounce, ao_bounce) + light_spec(n, vw, 0.2, ao_spec) + light_spec_sun(n, vw, 0.2, sh);
	vec3 wpos = v_cpos + cam.xyz;
	frag_color = vec4(light_finish(lit, v_cpos, FOG_MAP_RAY(fog_map, fog_smp, wpos.xz, v_cpos.xz)), 1.0);
}
@end

// Shadow cascades: the skinning vertex shader with mvp = the cascade matrix, depth only; cut-outs (armour normal alpha, hair alpha channel) clip like the main pass.
@fs char_shadow_fs
layout(binding=0) uniform texture2D clip_tex;
layout(binding=0) uniform sampler clip_smp;
struct row { vec4 v; };
layout(binding=29) readonly buffer insts { row ir[]; };
layout(binding=1) uniform shadow_view
{
	vec4 bias;      // caster_bias (shadow.inc)
};
in vec2 v_uv;
flat in uint v_inst;

@include_block caster_bias

void main()
{
	caster_bias(bias);
	const uint ib = v_inst * 9u;
	if (dot(texture(sampler2D(clip_tex, clip_smp), v_uv), ir[ib + 7u].v) < ir[ib + 8u].v.x) { discard; }
}
@end

@program character char_vs char_fs
@program character_armour char_vs armour_fs
@program character_hair char_vs hair_fs
@program character_shadow char_shadow_vs char_shadow_fs
@program character_standin char_standin_vs char_standin_fs
@program character_standin_lit char_standin_vs char_standin_lit_fs
