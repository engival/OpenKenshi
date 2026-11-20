// Grass layers (GRASS records, PagedGeometry GrassLoader in the game): instanced alpha-tested sprite quads (one quad with a random yaw, or two crossed quads),
// the whole sprite texture on each quad. Instance = base position (relative to its tile origin), colour map colour, width/height (world units) and two
// random numbers (yaw, sway phase). Like the game's grass_vs (materials/deferred/foliage.hlsl): the top of the quad sways, and with distance the blade
// grows out of the ground (the vertex shader sinks it); on top of that the client fades it out with the dithered fade of fade.inc, so nothing pops.
@vs grass_vs
layout(binding=0) uniform grass_vs_params
{
	mat4 mvp;       // projection * rotation-only view (camera-relative)
	vec4 base;      // xyz = tile origin relative to the camera
	vec4 camw;      // xyz = camera world position (world = p + camw), w = time (s)
	vec4 fade;      // x = distance where the fade-out starts, y = where it ends
	vec4 wind;      // x = sway length in world units (GRASS `wind factor` x 3), yz = wind direction (xz), w = 1: the original sway (+X, 0.3 pi rad/s, 2.0 rad/unit), 0: the live-wind field
};

in vec3 corner;     // x -0.5..0.5 across the quad, y 0..1 up, z = quad index (0 / 1: the second quad is turned 90 degrees)
in vec3 inst_pos;
in vec4 inst_col;
in vec2 inst_size;  // width, height as a fraction of 64 units
in vec2 inst_rnd;   // yaw / 2pi, sway phase / 2pi

out vec2 v_uv;
out vec3 v_col;
out float v_dist;
out vec3 v_wpos;
out vec3 v_cpos;

void main()
{
	float yaw = (inst_rnd.x + corner.z * 0.25) * 6.2831853;
	vec2 right = vec2(cos(yaw), sin(yaw));
	float w = inst_size.x * 64.0;
	float h = inst_size.y * 64.0;
	vec3 p = base.xyz + inst_pos;
	float d = length(p);
	float sink = clamp((d - fade.x * 0.9) / max(fade.y - fade.x * 0.9, 1.0), 0.0, 1.0);	// grows out of the ground over the last stretch
	vec3 q = p + vec3(right.x * corner.x * w, corner.y * h * (1.0 - 0.8 * sink) - 0.8 * sink * h, right.y * corner.x * w);
	vec2 wp = q.xz + camw.xz;
	// wind.w = 1: the original (kinspect foliage_wind.md 2-3): sin(time + x * 2.0), time = real s * 0.3 pi; else our two-sine field at the live wind's phase
	float sway = wind.w > 0.5 ? sin(camw.w * 0.94247780 + wp.x * 2.0) : (sin(camw.w * 1.9 + dot(wp, vec2(0.021, 0.013)) + inst_rnd.y * 6.2831853) * 0.7 + sin(camw.w * 0.7 + wp.x * 0.004) * 0.3);
	q.xz += wind.yz * (wind.x * sway * corner.y * corner.y);
	v_uv = vec2(corner.x + 0.5, 1.0 - corner.y);
	v_col = inst_col.rgb;
	v_dist = d;
	v_wpos = q + camw.xyz;
	v_cpos = q;
	gl_Position = mvp * vec4(q, 1.0);
}
@end

@include origin.inc
@include lighting.inc
@include shadow.inc
@include fade.inc
@include interiorclip.inc

@fs grass_fs
layout(binding=0) uniform texture2D grass_tex;
layout(binding=0) uniform sampler grass_smp;
layout(binding=4) uniform texture2D shadow_tex;
layout(binding=2) uniform sampler shadow_smp;
layout(binding=5) uniform texture2D fog_map;
layout(binding=3) uniform sampler fog_smp;
@include_block origin
@include_block lighting
@include_block lighting_ao
@include_block shadowing
@include_block interior_clip
layout(binding=1) uniform grass_fs_params
{
	vec4 fade;      // x = fade start, y = fade end (camera distance), z = alpha test threshold (the game's Grass_FP: 0.6)
};

in vec2 v_uv;
in vec3 v_col;
in float v_dist;
in vec3 v_wpos;
in vec3 v_cpos;
out vec4 frag_color;
@include_block fade_ign

void main()
{
	if (interior_clipped()) { discard; }
	if (fade.y > 0.0 && ign(gl_FragCoord.xy) < clamp((v_dist - fade.x) / (fade.y - fade.x), 0.0, 1.0)) { discard; }
	vec4 t = texture(sampler2D(grass_tex, grass_smp), v_uv);
	if (t.a < fade.z) { discard; }
	vec3 base = light_wet(t.rgb * v_col, 0.9, wet_under(v_cpos, 0.5));
	// foliage is lit with the normal (0,1,0) (foliage.hlsl grass_fs writes it); the shadow lookup uses the same normal
	vec3 lit = light_shade(base, vec3(0.0, 1.0, 0.0), shadow_visibility(v_cpos, vec3(0.0, 1.0, 0.0)), light_ao(v_cpos)) + light_points(base, vec3(0.0, 1.0, 0.0), v_cpos, 0.0);
	frag_color = vec4(light_finish(lit, v_cpos, FOG_MAP_RAY(fog_map, fog_smp, v_wpos.xz, v_cpos.xz)), 1.0);
}
@end

@image_sample_type shadow_tex depth
@sampler_type shadow_smp comparison
@sampler_type shadow_raw nonfiltering
@image_sample_type imask_tex unfilterable_float
@sampler_type imask_smp nonfiltering
@program grass grass_vs grass_fs
