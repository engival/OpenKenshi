@vs vs
layout(binding=0) uniform vs_params
{
	mat4 mvp;       // projection * rotation-only view (camera-relative world)
	vec4 origin;    // xyz = chunk origin relative to the camera, w = cell size in world units
	vec4 chunk;     // x = skirt depth, y = grid cells per side, zw = chunk origin xz in world units + 147456
};

layout(binding=0) uniform texture2D hmap;
layout(binding=0) uniform sampler smp;

in vec2 grid;   // vertex grid coordinate, -1 and cells+1 are the skirt ring

out vec3 v_normal;
out float v_height;
out float v_dist;
out vec2 v_uv;      // 0..1 over the whole world
out vec3 v_wpos;    // absolute world position (detail texture coordinates)
out vec3 v_cpos;    // position relative to the camera (shadow lookup)

float fetch_h(ivec2 p)
{
	return texelFetch(sampler2D(hmap, smp), p, 0).r;
}

void main()
{
	float cells = chunk.y;
	vec2 g = clamp(grid, vec2(0.0), vec2(cells));
	bool skirt = any(notEqual(g, grid));
	ivec2 t = ivec2(g) + 1; // texture has a 1 texel border for normals
	float h = fetch_h(t);
	float hx = fetch_h(t + ivec2(1, 0)) - fetch_h(t - ivec2(1, 0));
	float hz = fetch_h(t + ivec2(0, 1)) - fetch_h(t - ivec2(0, 1));
	float step2 = 2.0 * origin.w;
	v_normal = normalize(vec3(-hx, step2, -hz));
	v_height = h;
	if (skirt) { h -= chunk.x; }
	vec3 p = origin.xyz + vec3(g.x * origin.w, h, g.y * origin.w);
	v_dist = length(p);
	v_cpos = p;
	v_uv = (chunk.zw + g * origin.w) / 294912.0;
	v_wpos = vec3(chunk.z - 147456.0 + g.x * origin.w, v_height, chunk.w - 147456.0 + g.y * origin.w);
	gl_Position = mvp * vec4(p, 1.0);
}
@end

@include origin.inc
@include lighting.inc
@include shadow.inc
@include biome.inc
@include interiorclip.inc

@fs fs
layout(binding=9) uniform texture2D shadow_tex;
layout(binding=6) uniform sampler shadow_smp;
@include_block origin
@include_block lighting
@include_block lighting_ao
@include_block shadowing
@include_block interior_clip
layout(binding=1) uniform texture2D biome;
layout(binding=1) uniform sampler bsmp;
layout(binding=2) uniform texture2D ground;
layout(binding=2) uniform sampler gsmp;
layout(binding=3) uniform texture2DArray diffuse_maps;
layout(binding=4) uniform texture2DArray normal_maps;
layout(binding=5) uniform texture2D overlay_map;
layout(binding=6) uniform texture2D blend_map;
layout(binding=7) uniform texture2D biome_table;
layout(binding=8) uniform texture2D ground_map;
layout(binding=3) uniform sampler asmp;
layout(binding=4) uniform sampler lsmp;
layout(binding=5) uniform sampler tsmp;
layout(binding=10) uniform texture2D fog_map;
layout(binding=11) uniform texture2D ground_alpha;
layout(binding=7) uniform sampler fog_smp;
layout(binding=13) uniform texture2D blood_atlas;	// BloodDecals: 8 patch tiles of 1024 (4 x 2)
layout(binding=9) uniform sampler blood_smp;

layout(binding=1) uniform fs_params
{
	vec4 mode;      // x = 0 detail splat, 1 biome map, 2 plain, 3 colour map only; y = biomes in this chunk (0 = distant: ground colour map)
	vec4 brow;      // biome table row of each chunk biome
	vec4 bchan;     // blend map channel of each chunk biome (the first takes the remainder)
	vec4 blood_patch[8];	// per atlas tile: patch origin x, z, 1 / patch size, 1 if used
};

in vec3 v_normal;
in float v_height;
in float v_dist;
in vec2 v_uv;
in vec3 v_wpos;
in vec3 v_cpos;
out vec4 frag_color;

// no layers at hand (distant chunks, plain modes): the colour map's alpha x 1.2 stands in for the gloss
float ground_gloss() { return clamp(texture(sampler2D(ground_alpha, gsmp), v_uv).r * 1.2, 0.0, 1.0); }

float sel(vec4 v, int i) { return i == 0 ? v.x : i == 1 ? v.y : i == 2 ? v.z : v.w; }

@include_block biome

// the game's TerrainBlood pass (blood.hlsl): uv = (worldPos.xz - info.xy) * info.zw in the patch's decal texture (border: nothing outside), blended over the
// g-buffer albedo with its alpha; the gloss is kept
vec3 blood_over(vec3 albedo)
{
	for (int i = 0; i < 8; i++)
	{
		vec4 bp = blood_patch[i];
		vec2 uv = (v_wpos.xz - bp.xy) * bp.z;
		if (bp.w < 0.5 || uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0) { continue; }
		uv = clamp(uv, vec2(0.5 / 1024.0), vec2(1.0 - 0.5 / 1024.0));	// stay inside the tile
		vec4 b = textureLod(sampler2D(blood_atlas, blood_smp), (vec2(float(i % 4), float(i / 4)) + uv) / vec2(4.0, 2.0), 0.0);
		return mix(albedo, b.rgb, b.a);
	}
	return albedo;
}

// terrainfp4.hlsl:261 absorb = 1 - albedo.a + biome.absorbance; biome.absorbance is the layers' record value (compute_biome out_abs), 0 where no layers are sampled
void main()
{
	if (interior_clipped()) { discard; }
	vec3 n = normalize(v_normal);
	float slope = 1.0 - n.y;
	float sh = shadow_visibility(v_cpos, n);
	vec4 ao = light_ao(v_cpos);
	vec3 lit, albedo, spec_n = n;
	float gloss = ground_gloss();
	float babs = 0.0;
	float under = wet_under(v_cpos, 2.0);
	if (mode.x > 2.5)
	{
		// colour-map-only view (the pre-splat look): the colour map is pre-lit, only a little relief shading on top
		vec3 base = mix(texture(sampler2D(ground, gsmp), v_uv).rgb, vec3(0.40, 0.36, 0.32), smoothstep(0.30, 0.60, slope) * 0.25);
		albedo = light_wet(blood_over(base), 1.0 - gloss, under);
		lit = light_shade(albedo, n, sh, ao);
	} else if (mode.x < 0.5) {
		vec3 ground_rgb = texture(sampler2D(ground_map, lsmp), v_uv).rgb;
		if (mode.y < 0.5)
		{
			albedo = light_wet(blood_over(ground_rgb), 1.0 - gloss, under);
			lit = light_shade(albedo, n, sh, ao);
		} else {
			vec4 map = texture(sampler2D(overlay_map, lsmp), v_uv);
			map.r = max(map.r, map.g);
			vec4 colour = vec4(texture(sampler2D(ground, gsmp), v_uv).rgb, texture(sampler2D(ground_alpha, gsmp), v_uv).r) * 1.2;
			vec4 wt = texture(sampler2D(blend_map, lsmp), v_uv);
			vec2 cb = pow(max((abs(normalize(n.xz + vec2(1e-5))) - 0.2) * 7.0, 0.0), vec2(2.0));
			cb = n.y > 0.995 ? vec2(0.5) : cb / max(cb.x + cb.y, 1e-5);
			vec3 alb = vec3(0.0), nt = vec3(0.0);
			float gl = 0.0, ab = 0.0;
			float rest = 1.0;
			for (int i = 3; i >= 0; i--)
			{
				if (float(i) >= mode.y) { continue; }
				float w = i == 0 ? rest : sel(wt, int(sel(bchan, i) + 0.5));
				if (i != 0) { rest -= w; }
				if (w <= 0.002) { continue; }
				vec3 bn;
				float bab;
				vec4 bl = compute_biome(int(sel(brow, i) + 0.5), n, cb, map, colour, v_dist, ground_rgb, v_wpos, bn, bab);
				ab += bab * w;
				alb += bl.rgb * w;
				gl += bl.a * w;
				nt += bn * w;
			}
			vec3 b = normalize(cross(n, vec3(-1.0, 0.0, 0.0)));
			vec3 t = cross(b, n);
			vec3 tn = nt * 2.0 - 1.0;
			vec3 wn = normalize(t * tn.x + b * tn.y + n * tn.z);
			gloss = clamp(gl, 0.0, 1.0);
			babs = ab;
			albedo = light_wet(blood_over(alb), 1.0 - gloss + babs, under);
			lit = light_shade(albedo, wn, sh, ao);
			spec_n = wn;
		}
	} else {
		vec3 tex = mode.x < 1.5 ? texture(sampler2D(biome, bsmp), v_uv).rgb : vec3(0.62, 0.58, 0.50);
		vec3 base = mix(tex, vec3(0.40, 0.36, 0.32), smoothstep(0.15, 0.45, slope) * 0.6);
		albedo = light_wet(blood_over(base), 1.0 - gloss, under);
		lit = light_shade(albedo, n, sh, ao);
	}
	float wg = wet_gloss(gloss, 1.0 - gloss + babs, under);
	lit += light_spec(spec_n, -normalize(v_cpos), wg, ao.a) + light_spec_sun(spec_n, -normalize(v_cpos), wg, sh) + light_points(albedo, spec_n, v_cpos, wg);
	frag_color = vec4(light_finish(lit, v_cpos, FOG_MAP_RAY(fog_map, fog_smp, v_wpos.xz, v_cpos.xz)), 1.0);
}
@end

@image_sample_type hmap unfilterable_float
@image_sample_type imask_tex unfilterable_float
@sampler_type imask_smp nonfiltering
@sampler_type smp nonfiltering
@image_sample_type biome_table unfilterable_float
@image_sample_type shadow_tex depth
@sampler_type shadow_smp comparison
@sampler_type shadow_raw nonfiltering
@sampler_type tsmp nonfiltering

// Shadow map pass: the same vertex shader with mvp = the cascade matrix, no fragment output.
// The shadow caster (the only use of terrain_depth): depth plus the game's caster bias.
@fs fs_depth
@include_block caster_bias
layout(binding=1) uniform depth_fs_params
{
	vec4 bias;	// caster_bias (shadow.inc)
};
in vec3 v_normal;
in float v_height;
in float v_dist;
in vec2 v_uv;
in vec3 v_wpos;
in vec3 v_cpos;
void main() { caster_bias(bias); }
@end

@program terrain vs fs
@program terrain_depth vs fs_depth
