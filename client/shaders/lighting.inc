// Shared lighting (included by terrain.glsl, mesh.glsl and sky.glsl with `@include lighting.inc` + `@include_block lighting`; the lit stages add `@include_block lighting_ao`).
// One uniform block, filled once per frame from Sky::light() (src/render/sky.cpp, struct LightParams); every world shader shades with the
// same two functions, so terrain, buildings and features always agree.
@block lighting
layout(binding=2) uniform light_params
{
	vec4 sun_dir;   // xyz = direction towards the sun, clamped to the horizon (SkyX sunDirection), world space
	vec4 sun_col;   // rgb = direct light colour (HDR: sunColour.rgb x alpha), w = SkyX sun intensity alpha
	vec4 zenith;    // rgb = the game's zenithLight (HDR)
	vec4 nadir;     // rgb = the game's nadirLight (HDR)
	vec4 fog_col;   // rgb = atmosphere fog colour (the game's display-referred value, unexposed), w = exposure
	vec4 fog_rng;   // x = fog start, y = fog end (world units), z = retired (k16.2.83: the water light scale; unread, kept 0), w = weather fog density (1 / distance, 0 = off)
	vec4 fog_glob;  // rgb = weather fog colour (the game's display-referred value, unexposed), w = weather fog amount (0 = off)
	vec4 amb_mult;  // rgb = ambientmap.png rgb at the camera (multiplies the irradiance, deferred.hlsl:213); the map alpha x 2 is already folded into sun_col
	vec4 surf;      // camera-blended weather surface state: x = wetness, y = dust amount, z = dust slope (stickiness), w = dust inside (the shared params wetness / dustAmount.xyz)
	vec4 fog_sun;   // xyz = sunDirectionReal (not clamped to the horizon), the light direction of the atmosphere fog scatter; w = horizonClouds.w (the fog colour blends to hcloud with it)
	vec4 hcloud;    // rgb = horizonClouds.rgb (the game's display-referred HDR: the fog blend happens before the composite's clip)
	vec4 pl_head;   // w = the light debugger's mask of switched-off terms (knobs/lightterms.hpp, an integer in a float; 0 = all on, always so in a release build); x = point / spot lights in pl (light_points; WorldBuildings::gatherLights), y = 1: the water reflection pass (reflect_pass), z = water plane y - camera y (wet_under)
	vec4 pl[64];    // 4 per light: pos relative to the camera + radius; colour x power + type (1 = spot); spot direction; cos(inner / 2), cos(outer / 2), falloff
	vec4 amb_sh[9]; // the dynamic ambient (ours, post.sky_ambient; Sky::ambientSH): rgb = irradiance polynomial coefficients, amb_sh[0].w = its weight (0 = the game's cube)
};
layout(binding=12) uniform textureCube spec_cube;	// mp_specularity.dds (speccube.hpp); a view slot every program that calls light_spec keeps free
layout(binding=8) uniform sampler spec_smp;

// The water reflection pass (reflection.hpp): the game draws the world there with its G-buffer techniques into one target, so a surface stores buf0.r,
// the YCoCg luma of its unlit albedo (gbuffer.hlsl:71-76; kenshi-inspect findings/water_reflection_rt.md). light_shade returns that luma, every other
// light term and the fog nothing; emissive terms multiply by emissive_on() (lit_on() and the debugger's emissive switch).
bool reflect_pass() { return pl_head.y > 0.5; }
float lit_on() { return reflect_pass() ? 0.0 : 1.0; }
vec3 albedo_luma(vec3 albedo) { return vec3(dot(albedo, vec3(0.25, 0.5, 0.25))); }

// The light debugger (kenshi-jj3.27.1): pl_head.w holds one bit per switched-off light term (knobs/lightterms.hpp LightBit; docs/CLIENT.md "Light terms"). Bit values here are those enums.
bool term_off(int b) { return ((int(pl_head.w) >> b) & 1) != 0; }
float term_on(int b) { return term_off(b) ? 0.0 : 1.0; }
#define LT_SUN 0
#define LT_SHADOW 1
#define LT_AMBIENT 2
#define LT_AO 3
#define LT_INDIRECT 4
#define LT_BOUNCE 5
#define LT_INTERIOR 6
#define LT_ENVSPEC 7
#define LT_LAMPS 8
#define LT_EMISSIVE 9
#define LT_FOG_ATMOS 10
#define LT_FOG_WEATHER 11
#define LT_SKY 12
#define LT_CUTBAND 13
#define LT_MOTES_OWN 21	// set by `solo dust_motes`: the dust shader lights its motes by sun / lamps whatever those terms say (knobs/lightterms.hpp kLtMotesOwnLight)
float emissive_on() { return reflect_pass() ? 0.0 : term_on(LT_EMISSIVE); }

float scale_fn(float c, float sd)
{
	float x = 1.0 - c;
	return sd * exp(-0.00287 + x * (0.459 + x * (3.83 + x * (-6.80 + x * 5.25))));
}

// The game's deferred main light (data/materials/deferred/deferred.hlsl:197-220, docs/reference/LIGHTING.md). No gamma anywhere in the world path (sRGB conversion is
// off, textures are sampled raw, hdr_to_gamma = false), so this is plain linear arithmetic on the raw texture colour:
//   colour = albedo x (pi x N.L x sunColour.rgb x sunColour.w x 0.96 + irradiance(N) x 0.96 x envColour.w x clamp(5 sunY + 0.2, 0.1, 1)) x exposure
// 0.96 = FresnelDiffuse(dielectric_spec 0.04). The ambient is not the sky: it is the game's static irradiance cube data/materials/mp_irradiance.dds (16^2 BC3 x 6 faces,
// 5 mips, the deferred material's `cubic_texture mp_irradiance.dds` on Main_Lighting_RTW unit 6), sampled at mip 3 by the normal exactly as deferred.hlsl
// CalcEnvironmentLight does: texCUBElod(irr, N, 3) x a x 4 (kenshi-inspect findings rwh141_bdc_irradiance_live.md; the cube carries no hour, weather or sky input).
// The cube is bound as irr_cube by spec_cube_init (src/render/speccube.cpp) and filtered with spec_smp, the linear clamp sampler of the specularity cube next to it (a new
// sampler binding would not fit: SG_MAX_SAMPLER_BINDSLOTS 14, all in use). ambientMult (ambientmap.png) is sampled at the camera by Sky (rgb = amb_mult, alpha x 2
// folded into sun_col; the Hub is white, alpha 127 = x0.996); envColour.w (zenith.w) and ambientParams.w (nadir.w) are scalars the exe feeds (not traced; the
// common.program defaults 1 are used). sun_col is already sunColour.rgb x sunColour.w; fog_col.w = exposure.
layout(binding=24) uniform textureCube irr_cube;	// mp_irradiance.dds; a view slot every program that calls light_shade keeps free

// With the dynamic ambient on (amb_sh[0].w > 0; ours, CLIENT_RENDER_NEXT.md 3.1) the cube is blended towards the sky + ground bounce irradiance, an order-2 SH
// pre-convolved with the cosine lobe (same mean level as the cube). Off: the cube alone, unchanged.
vec3 light_irradiance(vec3 n)
{
	vec4 t = textureLod(samplerCube(irr_cube, spec_smp), n, 3.0);
	vec3 cube = t.rgb * (t.a * 4.0);
	if (amb_sh[0].w <= 0.0) { return cube; }
	vec3 sh = amb_sh[0].rgb + amb_sh[1].rgb * n.y + amb_sh[2].rgb * n.z + amb_sh[3].rgb * n.x + amb_sh[4].rgb * (n.x * n.y) + amb_sh[5].rgb * (n.y * n.z)
		+ amb_sh[6].rgb * (3.0 * n.z * n.z - 1.0) + amb_sh[7].rgb * (n.x * n.z) + amb_sh[8].rgb * (n.x * n.x - n.y * n.y);
	return mix(cube, max(sh, vec3(0.0)), amb_sh[0].w);
}

// shadow = sun visibility 0..1 (shadow.inc): the game multiplies only the sun term (deferred.hlsl:172-175, lightingFunctions.hlsl:90); the ambient is not shadowed.
// ao (light_ao; ours): a = ambient occlusion 0..1 (post.ao), the ambient only, 1 = the game's; rgb = indirect light (post.indirect) added to the ambient, 0 = the game's.
vec3 light_shade(vec3 albedo, vec3 n, float shadow, vec4 ao)
{
	if (reflect_pass()) { return albedo_luma(albedo); }
	vec3 sun = 3.14159265 * max(dot(n, sun_dir.xyz), 0.0) * sun_col.rgb * (nadir.w * (term_off(LT_SHADOW) ? 1.0 : shadow) * term_on(LT_SUN));
	vec3 amb = light_irradiance(n) * amb_mult.rgb * (zenith.w * clamp(5.0 * sun_dir.y + 0.2, 0.1, 1.0) * ao.a * term_on(LT_AMBIENT)) + ao.rgb;
	return albedo * (sun + amb) * (0.96 * fog_col.w);
}

// Environment specular of the deferred pass (lightingFunctions.hlsl CalcEnvironmentLight): LazarovEnvironmentBRDF(gloss, N.V, 0.04) x mp_specularity.dds (rgb x a x 10) looked up
// along the dominant direction lerp(N, R, gloss (sqrt(gloss) + roughness)) at mip (1 - gloss) x 7, then x ambientMult.rgb x envColour.w x clamp(5 sunY + 0.2, 0.1, 1) like the
// diffuse irradiance (deferred.hlsl:212-215). v = unit vector from the surface to the camera. Returns exposed colour (added after the albedo product). ao as for light_shade
// (specular occlusion).
vec3 light_spec(vec3 n, vec3 v, float gloss, float ao)
{
	if (reflect_pass()) { return vec3(0.0); }
	float nv = clamp(dot(n, v), 0.0, 1.0);
	vec4 t = vec4(1.0 / 0.96, 0.475, (0.0275 - 0.25 * 0.04) / 0.96, 0.25) * gloss + vec4(0.0, 0.0, (0.015 - 0.75 * 0.04) / 0.96, 0.75);
	float a0 = t.x * min(t.y, exp2(-9.28 * nv)) + t.z;
	float brdf = clamp(a0 + 0.04 * (t.w - a0), 0.0, 1.0);
	vec3 d = mix(n, 2.0 * dot(n, v) * n - v, gloss * (sqrt(gloss) + 1.0 - gloss * 0.99));
	vec4 c = textureLod(samplerCube(spec_cube, spec_smp), d, (1.0 - gloss) * 7.0);
	return brdf * c.rgb * (c.a * 10.0) * amb_mult.rgb * (zenith.w * clamp(5.0 * sun_dir.y + 0.2, 0.1, 1.0) * ao * term_on(LT_ENVSPEC)) * fog_col.w;
}

// Sun specular of the deferred pass (lightingFunctions.hlsl CalcPunctualLight + LightingFuncGGX_OPT3): lightColor x (N.L x D x FV) / pi with the sun colour of light_shade (x shadow),
// GGX D with roughness 1 - 0.99 gloss (alpha = roughness^2), F0 = 0.04 (dielectric_spec, no metalness map), Schlick on L.H (Horner form) and the Smith-ish visibility 1 / (LH^2 (1 - k^2) + k^2), k = alpha / 2.
// Narrow at high gloss (D peaks at 1 / (pi alpha^4)), so wet or glassy surfaces glint; clamped below the half-float range (the game's HDR target would overflow the same way). v = surface to camera.
vec3 light_spec_sun(vec3 n, vec3 v, float gloss, float shadow)
{
	if (reflect_pass()) { return vec3(0.0); }
	vec3 l = sun_dir.xyz;
	vec3 h = normalize(v + l);
	float nl = clamp(dot(n, l), 0.0, 1.0), lh = clamp(dot(l, h), 0.0, 1.0), nh = clamp(dot(n, h), 0.0, 1.0);
	float alpha = (1.0 - gloss * 0.99) * (1.0 - gloss * 0.99);
	float a2 = alpha * alpha;
	float den = max(nh * nh * (a2 - 1.0) + 1.0, 1e-6);
	float k2 = alpha * alpha * 0.25;
	float vis = 1.0 / (lh * lh * (1.0 - k2) + k2);
	float fv = (0.04 + 0.96 * exp2((-5.55473 * lh - 6.98316) * lh)) * vis;
	return min(sun_col.rgb * (nadir.w * (term_off(LT_SHADOW) ? 1.0 : shadow) * term_on(LT_SUN) * nl * a2 / (3.14159265 * den * den) * fv / 3.14159265) * fog_col.w, vec3(3000.0));
}

// The building lights (LIGHT records; kenshi-inspect light_record.md, deferred.hlsl light_fs): each adds CalcPunctualLight as the sun does (diffuse pi N.L
// and the GGX specular of light_spec_sun) x the game's piecewise falloff of x = d / radius (no constant / linear / quadratic terms), cut at the radius and
// N.L < 0.01. Spots: saturate(pow(saturate(dot(dir, -L) - cos(outer / 2)) / (cos(inner / 2) - cos(outer / 2)), falloff)). No shadows. rel = the fragment
// relative to the camera; returns exposed colour.
// One light of the list (the body of the loop below). v = fragment to camera, a2 / k2 = the GGX terms of the gloss; vis scales the light (1 = the game's, no shadows).
vec3 point_light(int i, vec3 albedo, vec3 n, vec3 rel, vec3 v, float a2, float k2, float vis)
{
	vec4 p = pl[4 * i], c = pl[4 * i + 1];
	vec3 dl = p.xyz - rel;
	float d = length(dl);
	if (d > p.w) { return vec3(0.0); }
	vec3 l = dl / max(d, 1e-4);
	float nl = dot(n, l);
	if (nl < 0.01) { return vec3(0.0); }
	float x = d / p.w;
	float att = x < 0.649 ? 0.8 * (1.0 - x) * (1.0 - x) * (1.0 - x) + 0.2 : x < 0.8 ? 0.242 - 3.0 * (x - 0.6) * (x - 0.6) : 3.0 * (x - 1.0) * (x - 1.0);
	if (c.w > 0.5)
	{
		vec4 s = pl[4 * i + 3];
		float den = s.x - s.y;
		att *= clamp(pow(clamp(dot(pl[4 * i + 2].xyz, -l) - s.y, 0.0, 1.0) / (abs(den) < 1e-6 ? 1e-6 : den), s.z), 0.0, 1.0);
	}
	vec3 h = normalize(v + l);
	float lh = clamp(dot(l, h), 0.0, 1.0), nh = clamp(dot(n, h), 0.0, 1.0);
	float dd = max(nh * nh * (a2 - 1.0) + 1.0, 1e-6);
	float fv = (0.04 + 0.96 * exp2((-5.55473 * lh - 6.98316) * lh)) / (lh * lh * (1.0 - k2) + k2);
	return c.rgb * (att * nl * vis) * (albedo * (3.14159265 * 0.96) + vec3(min(a2 / (3.14159265 * dd * dd) * fv / 3.14159265, 3000.0)));
}

vec3 light_points(vec3 albedo, vec3 n, vec3 rel, float gloss)
{
	if (reflect_pass() || term_off(LT_LAMPS)) { return vec3(0.0); }
	vec3 v = -normalize(rel);
	float alpha = (1.0 - gloss * 0.99) * (1.0 - gloss * 0.99);
	float a2 = alpha * alpha, k2 = a2 * 0.25;
	vec3 sum = vec3(0.0);
	int count = int(pl_head.x);
	for (int i = 0; i < count; i++) { sum += point_light(i, albedo, n, rel, v, a2, k2, 1.0); }
	return sum * fog_col.w;
}

// Metal-aware forms for a gbuffer metalness (deferred.hlsl:204-207): specColor = lerp(0.04, albedo, metal), albedo x (1 - metal). light_shade_f0 takes the
// diffuse albedo already scaled; FresnelDiffuse = saturate(1 - mean(f0)) (lightingFunctions.hlsl), LazarovEnvironmentBRDF and the GGX FV with a colour F0.
vec3 light_shade_f0(vec3 albedo, vec3 n, float shadow, vec3 f0, vec4 ao)
{
	if (reflect_pass()) { return albedo_luma(albedo); }
	vec3 sun = 3.14159265 * max(dot(n, sun_dir.xyz), 0.0) * sun_col.rgb * (nadir.w * (term_off(LT_SHADOW) ? 1.0 : shadow) * term_on(LT_SUN));
	vec3 amb = light_irradiance(n) * amb_mult.rgb * (zenith.w * clamp(5.0 * sun_dir.y + 0.2, 0.1, 1.0) * ao.a * term_on(LT_AMBIENT)) + ao.rgb;
	return albedo * (sun + amb) * (clamp(1.0 - dot(f0, vec3(1.0 / 3.0)), 0.0, 1.0) * fog_col.w);
}

vec3 light_spec_f0(vec3 n, vec3 v, float gloss, vec3 f0, float ao)
{
	if (reflect_pass()) { return vec3(0.0); }
	float nv = clamp(dot(n, v), 0.0, 1.0);
	vec4 t = vec4(1.0 / 0.96, 0.475, (0.0275 - 0.25 * 0.04) / 0.96, 0.25) * gloss + vec4(0.0, 0.0, (0.015 - 0.75 * 0.04) / 0.96, 0.75);
	float a0 = t.x * min(t.y, exp2(-9.28 * nv)) + t.z;
	vec3 brdf = clamp(a0 + f0 * (t.w - a0), 0.0, 1.0);
	vec3 d = mix(n, 2.0 * dot(n, v) * n - v, gloss * (sqrt(gloss) + 1.0 - gloss * 0.99));
	vec4 c = textureLod(samplerCube(spec_cube, spec_smp), d, (1.0 - gloss) * 7.0);
	return brdf * c.rgb * (c.a * 10.0) * amb_mult.rgb * (zenith.w * clamp(5.0 * sun_dir.y + 0.2, 0.1, 1.0) * ao * term_on(LT_ENVSPEC)) * fog_col.w;
}

vec3 light_spec_sun_f0(vec3 n, vec3 v, float gloss, float shadow, vec3 f0)
{
	if (reflect_pass()) { return vec3(0.0); }
	vec3 l = sun_dir.xyz;
	vec3 h = normalize(v + l);
	float nl = clamp(dot(n, l), 0.0, 1.0), lh = clamp(dot(l, h), 0.0, 1.0), nh = clamp(dot(n, h), 0.0, 1.0);
	float alpha = (1.0 - gloss * 0.99) * (1.0 - gloss * 0.99);
	float a2 = alpha * alpha;
	float den = max(nh * nh * (a2 - 1.0) + 1.0, 1e-6);
	float k2 = alpha * alpha * 0.25;
	float vis = 1.0 / (lh * lh * (1.0 - k2) + k2);
	vec3 fv = (f0 + (1.0 - f0) * exp2((-5.55473 * lh - 6.98316) * lh)) * vis;
	return min(sun_col.rgb * (nadir.w * (term_off(LT_SHADOW) ? 1.0 : shadow) * term_on(LT_SUN) * nl * a2 / (3.14159265 * den * den) / 3.14159265) * fv * fog_col.w, vec3(3000.0));
}

// makeWet (materials/common/wet.hlsl): albedo x (1 - min((1 - 1 / (wet + 0.7)) x absorbance + 0.2 under, 0.4) x 0.5), at most x0.8; wet = min(wetness + under, 1).
// The formula is negative for wet < 0.3 (1 - 1 / (wet + 0.7) < 0), so dry ground is brightened (x1.13 at absorbance 0.6, wet 0). Kept as in the game: the live
// shared params read wetness 0 in clear and dust weather (kenshi-inspect, Sho-Battai 10:17), so the game applies that gain everywhere. The gloss half is wet_gloss.
// absorbance = 1 - gloss (x (1 - metalness)). under = wet_under: river beds and anything else below the water darken.
vec3 light_wet(vec3 albedo, float absorbance, float under)
{
	float wet = min(surf.x + under, 1.0);
	return albedo * (1.0 - min((1.0 - 1.0 / (wet + 0.7)) * absorbance + under * 0.2, 0.4) * 0.5);
}

// The other half of makeWet: gloss = lerp(gloss, 0.5, saturate(shine)), shine = 2 x max(0, wet - 0.15 absorbance) ^ (1 + 4 absorbance), none under water
// ("make sure underwater stuff doesn't go all shiny"). Rain turns dull ground into 0.5-gloss glints. Feeds light_spec.
float wet_gloss(float gloss, float absorbance, float under)
{
	float shine = 2.0 * pow(max(0.0, min(surf.x + under, 1.0) - 0.15 * absorbance), 1.0 + 4.0 * absorbance) * clamp((1.0 - under) * 100.0, 0.0, 1.0);
	return mix(gloss, 0.5, clamp(shine, 0.0, 1.0));
}

// makeWet's underwater term, saturate((waterHeightRel - y + edge) / edge): 1 from edge units below the water plane down. cpos = the fragment relative to the camera,
// pl_head.z = the water plane relative to the camera. edge: 2 on the terrain (terrainfp4.hlsl), 0.5 on objects, characters and foliage.
float wet_under(vec3 cpos, float edge)
{
	return clamp((pl_head.z - cpos.y + edge) / edge, 0.0, 1.0);
}

// Weather map (weathermap.hpp): fog_map is a 256^2 RGBA16F texture over the whole world, bound next to the other textures of each world shader (terrain, buildings /
// foliage, water); fm below is its mean along the camera-to-fragment ray (FOG_MAP_RAY). rgb = fog colour x density x 1e4, a = density x 1e4, blurred, so rgb / a is the
// colour. amb_mult.w = 1 selects it; 0 uses the uniform weather fog (a manual weather). Both give the same colour rule: raw fog colour x max(sun alpha, nadirLight), the game's display-referred value and unexposed (the composite's exposure applies once, k16.2.81).
vec2 fog_map_uv(vec2 xz)
{
	return (xz + 147456.0) / 294912.0;
}

// Mean of the (premultiplied) fog map over the view ray, 4 midpoint samples: the fog of a region counts only for the part of the ray inside it, so a ridge just over
// a dust region's border is not fogged as if seen from inside (greenlands_bridge), while a storm region seen from outside still grows into a wall. Mine: the exe's global
// fog is one value for the camera's weather. wxz = the fragment's world xz, rxz = its xz relative to the camera.
#define FOG_MAP_RAY(tex, smp, wxz, rxz) (0.25 * ( \
	texture(sampler2D(tex, smp), fog_map_uv((wxz) - 0.875 * (rxz))) + \
	texture(sampler2D(tex, smp), fog_map_uv((wxz) - 0.625 * (rxz))) + \
	texture(sampler2D(tex, smp), fog_map_uv((wxz) - 0.375 * (rxz))) + \
	texture(sampler2D(tex, smp), fog_map_uv((wxz) - 0.125 * (rxz)))))

// The atmosphere fog colour (post/atmospherefog.hlsl calculateAtmosphereFog: a per-pixel Rayleigh in-scatter along the camera-to-fragment ray, 4 samples), HDR.
// rel = fragment position relative to the camera. Constants: the live SkyXFogParams (kenshi-inspect, greenlands_bridge 20:24). uCameraPos.y = inner + 0.0052 there (the exe
// feeds it from the camera, the rule is not traced: held at the measured value). The ray is at most the skydome radius (70000) long.
vec3 atm_fog_hdr(vec3 rel)
{
	const vec3 inv_wl = vec3(9.47328472, 18.8380127, 26.6802139);
	const float inner = 9.77501011, cam_y = 9.78022289, sd = 0.260644913, sosd = 7.35989285, scl = 1.91831863;
	const float kr4pi = 0.0276460163, km4pi = 0.00848230068, kresun = 0.066, expo = 1.4;
	vec3 l = fog_sun.xyz;
	vec3 cam = vec3(0.0, cam_y, 0.0);
	vec3 p = rel / 70000.0;
	p.y += inner;
	p.y = mix(max(p.y, inner), p.y, clamp(p.x * p.x + p.z * p.z, 0.0, 1.0));
	vec3 ray = p - cam;
	float far = length(ray);
	ray /= far;
	far = min(far, 1.0);
	if (ray.y < -0.3) { ray = vec3(0.0, -0.3, 0.953); }
	float start_off = exp(sosd * (inner - cam_y)) * scale_fn(dot(ray, cam) / cam_y, sd);
	float sl = far * 0.25;
	vec3 sray = ray * sl;
	vec3 sp = cam + sray * 0.5;
	vec3 col = vec3(0.0);
	for (int i = 0; i < 4; i++)
	{
		float h = length(sp);
		float depth = exp(sosd * (inner - h));
		float scatter = start_off + depth * (scale_fn(dot(l, sp) / h, sd) - scale_fn(dot(ray, sp) / h, sd));
		col += exp(-scatter * (inv_wl * kr4pi + km4pi)) * (depth * sl * scl);
		sp += sray;
	}
	vec3 dir = cam - p;
	float c = -dot(l, dir) / length(dir);
	return 0.75 * (1.0 + 0.5 * c * c) * col * (inv_wl * kresun) * expo;
}

// Atmosphere fog colour in the scene's display-referred HDR (x the baked exposure, like lit): the scatter blended to horizonClouds by its alpha (atmospherefog.hlsl:94-95).
// The game blends it into the scene before the composite's exposure and clip; no display encode.
vec3 atm_fog(vec3 rel)
{
	return mix(atm_fog_hdr(rel), hcloud.rgb, fog_sun.w) * fog_col.w;
}

// fog over a display-referred scene colour: the atmosphere term (sat((d - 3000) / (30000 - 3000)), pFogParams) and the weather's global fog
// (post/fog.hlsl fogValue: S curve of saturate(dist x density) x amount), combined as fog.hlsl does: rgb = lerp(atmosphere, global, global.a), a = saturate(sum).
// rel = the fragment position relative to the camera. No display encode (tone.inc does it once, at the end).
vec3 light_finish(vec3 lit, vec3 rel, vec4 fm)
{
	if (reflect_pass()) { return lit; }	// the atmosphere fog is a compositor quad, not in the reflection
	float dist = length(rel);
	float fa = clamp((dist - fog_rng.x) / (fog_rng.y - fog_rng.x), 0.0, 1.0);
	float rho = fog_rng.w;
	float on = fog_glob.w;
	vec3 gcol = fog_glob.rgb;
	if (amb_mult.w > 0.5)
	{
		rho = fm.a * 1e-4;
		on = 1.0;
		gcol = clamp(fm.rgb / max(fm.a, 1e-5) * max(vec3(sun_col.w), nadir.rgb), 0.0, 1.0);
	}
	if (term_off(LT_FOG_WEATHER)) { on = 0.0; }
	float amount = clamp(dist * rho, 0.0, 1.0);
	float curve = amount < 0.5 ? 2.0 * amount * amount : 1.0 - 2.0 * (amount - 1.0) * (amount - 1.0);
	float g = curve * on;
	if (term_off(LT_FOG_ATMOS)) { fa = 0.0; }
	return mix(lit, mix(fa > 0.0 ? atm_fog(rel) : vec3(0.0), gcol, g), clamp(fa + g, 0.0, 1.0));
}
@end

// Ambient occlusion (ao.hpp, post.ao; ours, the game has none): the previous frame's screen-space AO of this camera, reprojected. rel + ao_delta.xyz is the fragment
// relative to the previous camera; a texel counts only where its stored view depth matches the fragment's within 2 % (a joint-bilateral upsample that also rejects
// what was hidden last frame: newly revealed pixels and the ground a character just left get no AO for a frame). Off (ao_delta.w = 0): exactly 1.
@block lighting_ao
layout(binding=4) uniform ao_params
{
	mat4 ao_prev_vp;	// the previous frame's camera-relative projection x view
	vec4 ao_delta;		// xyz = this camera's position - the previous one's, w = 1: on
	vec4 ao_par;		// x = strength 0..1, yz = AO texture size, w = indirect strength (0 = off)
};
layout(binding=16) uniform texture2D ao_tex;	// r = AO, g = view depth (ao.glsl); a view slot every lit program keeps free
layout(binding=17) uniform texture2D il_tex;	// rgb = indirect light (ao.glsl); the same
layout(binding=10) uniform sampler ao_smp;

// rgb = indirect light, a = AO at a camera-relative position (light_shade's ao), from the previous frame's textures through its camera; (0, 0, 0, 1) = none.
vec4 light_ao_raw(vec3 rel)
{
	if (ao_delta.w == 0.0) { return vec4(0.0, 0.0, 0.0, 1.0); }
	vec4 c = ao_prev_vp * vec4(rel + ao_delta.xyz, 1.0);
	if (c.w <= 0.0) { return vec4(0.0, 0.0, 0.0, 1.0); }
	vec2 p = offscreen_texel((c.xy / c.w * 0.5 + 0.5) * ao_par.yz - 0.5, ao_par.z);	// origin.inc: the AO target's rows (the texel fetches below)
	ivec2 b = ivec2(floor(p));
	vec2 f = p - floor(p);
	float sum = 0.0, wsum = 0.0;
	vec3 isum = vec3(0.0);
	for (int i = 0; i < 4; i++)
	{
		ivec2 o = ivec2(i & 1, i >> 1);
		ivec2 q = clamp(b + o, ivec2(0), ivec2(ao_par.yz) - 1);
		vec2 t = texelFetch(sampler2D(ao_tex, ao_smp), q, 0).rg;
		float w = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y);
		if (abs(t.y - c.w) < 0.02 * c.w)
		{
			sum += w * t.x;
			isum += w * texelFetch(sampler2D(il_tex, ao_smp), q, 0).rgb;
			wsum += w;
		}
	}
	return wsum > 1e-3 ? vec4(isum / wsum * ao_par.w, mix(1.0, sum / wsum, ao_par.x)) : vec4(0.0, 0.0, 0.0, 1.0);
}
// the light debugger's ao / indirect switches (knobs/lightterms.hpp)
vec4 light_ao(vec3 rel)
{
	vec4 r = light_ao_raw(rel);
	return vec4(term_off(LT_INDIRECT) ? vec3(0.0) : r.rgb, term_off(LT_AO) ? 1.0 : r.a);
}
@end

// Interior light (render.interior_light, kenshi-jj3.2.1; lightvol.hpp): the baked SKY TRANSFER of up to 8 buildings (lightbake.hpp: L1 spherical harmonics per probe, how much of a
// uniform white sky reaches the probe, ground bounce included) multiplies the ambient term, so a room is dark where the openings do not reach and bright beside them. Ported
// from kenshi-lab stormview/shaders/lit.glsl probe_light (074dc80: the trilinear lookup divided by the filtered `usable` so a probe inside a wall never leaks, the lookup
// point pushed one probe spacing off the surface along its geometry normal, the enclosure gate that keeps the light indoors, and the weight that fades the volume out at its
// edge). The lab's output is light; ours is a FACTOR on the client's own ambient: the sky transfer over what an open field gives for the same
// normal (the bake's own model: sky radiance 1 above the horizon, the ground bouncing `a` below it), so outside every volume, and in the open, it is exactly 1.
// Included only by the programs that call light_vol (mesh, character); every one of them binds vol_sh / vol_use / vol_smp and light_volumes (lightvol.cpp).
@block lighting_vol
layout(binding=5) uniform light_volumes
{
	vec4 vol_head;	// x = volume count (0 = off), y = strength 0..1 (render.interior_light: 1 - its value), z = the open-ground albedo of the bake, w = strength 0..1 (render.sun_bounce)
	vec4 vol_atlas;	// xyz = the sky atlas size in probes, w = the sun atlas' page width in probes (the latest bake; the page it replaces follows it along x)
	vec4 vol_m[64];	// per volume, 8 vec4: [0..2] camera-relative position -> probe index (xyz coefficients, w offset); [3..5] the world -> building rotation rows (xyz), w = probes per axis;
					// [6] x = atlas x offset in probes, y = blend-in 0..1 (a bake that just arrived fades in over a second), z = the sun bake's cross-fade 0..1, w = 1: the volume has a sun term
};
layout(binding=19) uniform texture3D vol_sh;	// rgba16f: the sky transfer SH (L00, L1y, L1z, L1x) x usable
layout(binding=20) uniform texture3D vol_use;	// rg16f: r = usable (1, 0 for a probe inside a wall or under the ground), g = enclosure x usable
layout(binding=21) uniform texture3D vol_sun0;	// render.sun_bounce (kenshi-jj3.4.1): the sun transfer's R, G, B (one 3D texture per channel, rgba16f: L00, L1y, L1z, L1x; the atlas is two pages wide)
layout(binding=22) uniform texture3D vol_sun1;
layout(binding=23) uniform texture3D vol_sun2;
layout(binding=11) uniform sampler vol_smp;

// render.sun_bounce: the RGB irradiance of the sun transfer at the probe g along the building-local normal nl, the latest bake mixed over the one it replaced by
// `fade`. The bake's transfer is for a white sun of strength 1 (kenshi-lab bake.cpp's sunE = 1: the surfaces it lights read albedo x cos / pi); the client's
// deferred sun term is albedo x pi N.L sun_col, so the same source radiance is sun_col x pi^2 here and the ambient bracket (light_shade: radiance x pi) takes
// pi x this. off = the volume's page A x offset in probes, dims = (2 x page width, height, depth).
vec3 light_vol_sun(vec3 g, vec3 nl, float off, float fade)
{
	const vec3 dims = vec3(2.0 * vol_atlas.w, vol_atlas.y, vol_atlas.z);
	const vec3 uvwA = (g + vec3(0.5 + off, 0.5, 0.5)) / dims;
	const vec3 uvwB = uvwA + vec3(vol_atlas.w / dims.x, 0.0, 0.0);
	const vec4 a0 = textureLod(sampler3D(vol_sun0, vol_smp), uvwA, 0.0), b0 = textureLod(sampler3D(vol_sun0, vol_smp), uvwB, 0.0);
	const vec4 a1 = textureLod(sampler3D(vol_sun1, vol_smp), uvwA, 0.0), b1 = textureLod(sampler3D(vol_sun1, vol_smp), uvwB, 0.0);
	const vec4 a2 = textureLod(sampler3D(vol_sun2, vol_smp), uvwA, 0.0), b2 = textureLod(sampler3D(vol_sun2, vol_smp), uvwB, 0.0);
	const vec4 s0 = mix(b0, a0, fade), s1 = mix(b1, a1, fade), s2 = mix(b2, a2, fade);
	// cosine-convolved L1, as the sky factor: flat + (2 pi / 3) Y1 (L1 . n), never below 0
	const vec3 e = vec3(0.886227 * s0.x + 1.023328 * dot(s0.yzw, nl.yzx), 0.886227 * s1.x + 1.023328 * dot(s1.yzw, nl.yzx), 0.886227 * s2.x + 1.023328 * dot(s2.yzw, nl.yzx));
	return max(e, vec3(0.0));
}

// x = the factor for the diffuse ambient along n, y = the factor for the specular environment (the SH mean); both exactly 1 outside every volume. rel = camera-relative position.
// ng = the surface's GEOMETRY normal (not the normal-mapped one): the lookup point is pushed one probe spacing off the surface along it, as the lab's probe_light does
// (lit.glsl: grid_uvw(p + ng * probeSpacing)), so a face never interpolates with the probes behind its wall. Without it an exterior wall took the dark probes of the
// room it encloses (kenshi-k16.2.256.8.5: the BDC dome's walls and door at 0.4-0.7 x the original). +1 on a probe-index axis is one spacing.
// bounce = render.sun_bounce's irradiance along n (sun_col and the daylight factor already in, 0 outside every volume or with the switch off): ADD it to the ambient.
vec2 light_vol(vec3 rel, vec3 n, vec3 ng, out vec3 bounce)
{
	vec2 f = vec2(1.0);
	bounce = vec3(0.0);
	const int cnt = int(vol_head.x);
	const bool no_interior = term_off(LT_INTERIOR), no_bounce = term_off(LT_BOUNCE);
	for (int i = 0; i < cnt; i++)
	{
		const vec4 a0 = vol_m[i * 8], a1 = vol_m[i * 8 + 1], a2 = vol_m[i * 8 + 2], r0 = vol_m[i * 8 + 3], r1 = vol_m[i * 8 + 4], r2 = vol_m[i * 8 + 5], inf = vol_m[i * 8 + 6];
		const vec3 g = vec3(dot(a0.xyz, rel) + a0.w, dot(a1.xyz, rel) + a1.w, dot(a2.xyz, rel) + a2.w);	// probe index
		const vec3 top = vec3(r0.w, r1.w, r2.w) - 1.0;
		const vec3 gp = g + vec3(dot(r0.xyz, ng), dot(r1.xyz, ng), dot(r2.xyz, ng));	// one spacing out along the geometry normal, in probe index
		// the weight: 1 inside, 0 past the grid's outer margin (two probes: the bake's margin round the building); no fade at the bottom (the floor sits on the grid's lowest layer)
		const float sx = smoothstep(0.0, 2.0, min(gp.x, top.x - gp.x)), sz = smoothstep(0.0, 2.0, min(gp.z, top.z - gp.z));
		const float sy = smoothstep(0.0, 2.0, top.y - gp.y) * clamp(gp.y + 1.0, 0.0, 1.0);
		float w = sx * sy * sz * inf.y * vol_head.y;
		if (w <= 0.0) { continue; }
		const vec3 uvw = (clamp(gp, vec3(0.0), top) + vec3(0.5 + inf.x, 0.5, 0.5)) / vol_atlas.xyz;	// clamped to the volume's own probes: no bleed into the next volume of the atlas
		const vec2 uu = textureLod(sampler3D(vol_use, vol_smp), uvw, 0.0).rg;	// r = usable, g = enclosure x usable (the sun shafts' channel)
		const float use = uu.x;
		w *= smoothstep(0.0, 0.02, use);
		if (w <= 0.0) { continue; }
		// an INDOOR light: the factor applies only where the sky at the lookup point is under cover. The bake's interior probes read an enclosure of 0.9-1.0 and the open
		// ones below 0.5; the 0.5-0.9 band is the sheltered outdoors (a niche, a face under an overhang, the fringe of a doorway), so a face there gets exactly 1 (the
		// shader's contract; kenshi-k16.2.256.8.5; the lab's surf_enclosure gate in lit.glsl, smoothstep(0.6, 0.9), starts lower on its own bake).
		w *= smoothstep(0.7, 0.9, uu.y / max(use, 0.05));
		if (w <= 0.0) { continue; }
		const vec4 s = textureLod(sampler3D(vol_sh, vol_smp), uvw, 0.0) / max(use, 0.05);
		const vec3 nl = vec3(dot(r0.xyz, n), dot(r1.xyz, n), dot(r2.xyz, n));
		const float a = vol_head.z;
		const float flat_term = 0.886227 * s.x;
		const float e = max(flat_term + 1.023328 * dot(s.yzw, nl.yzx), 0.1 * flat_term);	// the irradiance of the sky transfer along the normal
		const float open_e = 1.570796 * ((1.0 + a) + (1.0 - a) * n.y);	// the same for an open field: radiance 1 above the horizon, `a` below
		if (!no_interior) { f = mix(f, vec2(clamp(e / open_e, 0.0, 1.0), clamp(flat_term / (1.570796 * (1.0 + a)), 0.0, 1.0)), w); }
		// the sun bounce: only with render.sun_bounce on, a bake present and the sun up (below the horizon it is zero)
		if (vol_head.w > 0.0 && inf.w > 0.0 && !no_bounce)
		{
			const float up = sun_dir.y > 1e-4 ? 1.0 : 0.0;
			bounce += 3.14159265 * light_vol_sun(clamp(gp, vec3(0.0), top), nl, inf.x, inf.z) * (sun_col.rgb * (nadir.w * up * vol_head.w * w));
		}
	}
	return f;
}

// render.lamp_occlusion (kenshi-jj3.9.1): the lamps of a baked building as the bake saw them (lightbake.hpp Occluder: one ray per probe per lamp). layout(binding=25): rgba8, twice the atlas wide,
// lamp 0..3 of a probe in the left half's channels and 4..7 in the right's, 255 = the probe sees the lamp (stored times usable like the sky transfer, so the filtered value is divided by the filtered usable).
// A light's code is pl[4 i + 3].w = 1 + volume * 8 + lamp (WorldBuildings::fillLights); 0 = none. The visibility is soft (probe spacing): light stops at a floor or a wall, it does not draw a chair's shadow.
layout(binding=25) uniform texture3D vol_lamp;

float lamp_vis(vec3 rel, int code)
{
	const int vi = ((code - 1) >> 3) * 8, lamp = (code - 1) & 7;
	const vec4 a0 = vol_m[vi], a1 = vol_m[vi + 1], a2 = vol_m[vi + 2], r0 = vol_m[vi + 3], r1 = vol_m[vi + 4], r2 = vol_m[vi + 5], inf = vol_m[vi + 6];
	const vec3 g = vec3(dot(a0.xyz, rel) + a0.w, dot(a1.xyz, rel) + a1.w, dot(a2.xyz, rel) + a2.w);
	const vec3 top = vec3(r0.w, r1.w, r2.w) - 1.0;
	// the same edge weight as light_vol; past the grid's margin the lamp is as the game draws it
	const float sx = smoothstep(0.0, 2.0, min(g.x, top.x - g.x)), sz = smoothstep(0.0, 2.0, min(g.z, top.z - g.z));
	const float sy = smoothstep(0.0, 2.0, top.y - g.y) * clamp(g.y + 1.0, 0.0, 1.0);
	float w = sx * sy * sz * inf.y;
	if (w <= 0.0) { return 1.0; }
	const vec3 gc = clamp(g, vec3(0.0), top);
	const vec3 uvw = (gc + vec3(0.5 + inf.x, 0.5, 0.5)) / vol_atlas.xyz;
	const float use = textureLod(sampler3D(vol_use, vol_smp), uvw, 0.0).r;
	w *= smoothstep(0.0, 0.3, use);
	if (w <= 0.0) { return 1.0; }
	const vec3 uvl = vec3((gc.x + 0.5 + inf.x + (lamp >= 4 ? vol_atlas.x : 0.0)) / (2.0 * vol_atlas.x), uvw.y, uvw.z);
	const vec4 t = textureLod(sampler3D(vol_lamp, vol_smp), uvl, 0.0);
	const float seen = clamp((lamp & 3) == 0 ? t.x / max(use, 0.05) : (lamp & 3) == 1 ? t.y / max(use, 0.05) : (lamp & 3) == 2 ? t.z / max(use, 0.05) : t.w / max(use, 0.05), 0.0, 1.0);
	return mix(1.0, seen, w);
}

// light_points with the baked lamp visibility on the lights that carry a code; the others, and every light with the switch off, exactly as light_points.
vec3 light_points_vol(vec3 albedo, vec3 n, vec3 rel, float gloss)
{
	if (reflect_pass() || term_off(LT_LAMPS)) { return vec3(0.0); }
	vec3 v = -normalize(rel);
	float alpha = (1.0 - gloss * 0.99) * (1.0 - gloss * 0.99);
	float a2 = alpha * alpha, k2 = a2 * 0.25;
	vec3 sum = vec3(0.0);
	int count = int(pl_head.x);
	for (int i = 0; i < count; i++)
	{
		const int code = int(pl[4 * i + 3].w + 0.5);
		sum += point_light(i, albedo, n, rel, v, a2, k2, code > 0 ? lamp_vis(rel, code) : 1.0);
	}
	return sum * fog_col.w;
}

// render.sun_bounce: the bounce irradiance added to the ambient of light_shade (albedo x it, exposed and ambient-occluded like the rest of the bracket).
vec3 light_bounce(vec3 albedo, vec3 bounce, float ao)
{
	return albedo * bounce * (0.96 * fog_col.w * ao);
}
@end
