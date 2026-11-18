// The global water plane (y = 100, docs/reference/WATER.md), drawn over every terrain chunk that dips below it. The vertex grid is the terrain's, so the
// per-vertex water depth (100 - terrain height) interpolates over the very triangles the terrain draws: alpha reaches 0 exactly where the terrain crosses 100.
@vs vs
layout(binding=0) uniform vs_params
{
	mat4 mvp;       // projection * rotation-only view (camera-relative world)
	vec4 origin;    // xyz = chunk origin relative to the camera, w = cell size in world units
	vec4 chunk;     // x = unused, y = grid cells per side, zw = chunk origin xz in world units + 147456
	vec4 level;     // x = water level relative to the camera (100 - camera y)
};

layout(binding=0) uniform texture2D hmap;
layout(binding=0) uniform sampler smp;

in vec2 grid;   // terrain vertex grid; the skirt ring is clamped onto the border (degenerate triangles)

out vec3 v_rel;     // camera-relative position
out vec2 v_world;   // world xz
out float v_depth;  // 100 - terrain height at the vertex
out vec2 v_uv;      // 0..1 over the whole world
out vec4 v_clip;    // clip position (the reflection's projected coordinate)

void main()
{
	vec2 g = clamp(grid, vec2(0.0), vec2(chunk.y));
	float h = texelFetch(sampler2D(hmap, smp), ivec2(g) + 1, 0).r;
	vec3 p = vec3(origin.x + g.x * origin.w, level.x, origin.z + g.y * origin.w);
	vec2 m = chunk.zw + g * origin.w;
	v_rel = p;
	v_world = m - 147456.0;
	v_depth = level.x - origin.y - h;
	v_uv = m / 294912.0;
	gl_Position = mvp * vec4(p, 1.0);
	v_clip = gl_Position;
}
@end

@include origin.inc
@include lighting.inc

@fs fs
@include_block origin
@include_block lighting
layout(binding=1) uniform texture2D colour;   // watercolourmap.png (128^2)
layout(binding=2) uniform texture2D flow;      // flowmap.png: rg = flow, b = scum
layout(binding=3) uniform texture2DArray wnormal;	// the biomes' water normal maps (layer 0 = the material's water.png)
layout(binding=4) uniform texture2DArray scum_dif;	// the biomes' scum maps (water.cpp: one layer per scum pair)
layout(binding=5) uniform texture2DArray scum_nml;	// their normals, a = the scum amount
layout(binding=6) uniform texture2D windex;    // biomemap.png as rows of wtable: r x 255 (1024^2, nearest)
layout(binding=9) uniform texture2D wtable;    // 4 texels per biome (water.cpp kTable*): the game's per-biome water uniforms (WATER.md 2.1)
layout(binding=10) uniform texture2DArray wturb;	// the biomes' turbulence maps
layout(binding=11) uniform texture2D rain_map;	// rain-ripples.png (the game's rainMap): r = drop ring radius/weight, gb = ring normal xz, a = drop phase
layout(binding=1) uniform sampler wsmp;        // linear, clamp
layout(binding=2) uniform sampler rsmp;        // linear, repeat, anisotropic
layout(binding=3) uniform sampler psmp;        // nearest, clamp
layout(binding=7) uniform texture2D fog_map;   // weather map (lighting.inc)
layout(binding=13) uniform texture2D scene_depth;	// the opaque scene depth (R32F, hdr.hpp copyDepthForWater): the G-buffer distance the opacity reads
layout(binding=8) uniform texture2D reflection;	// the planar reflection (reflection.hpp): r = albedo luma / sky red, drawn flipped left to right
layout(binding=4) uniform sampler fog_smp;

layout(binding=1) uniform fs_params
{
	vec4 misc;      // x = animation time (s), y = scum on (1/0), z = flat (1 = debug colour only), w = raw sunColour / sun_col (water.hlsl's unscaled sun)
	vec4 refl;      // x = 1: the reflection was drawn this frame (0: rf = 1, as the game's NO_REFLECTION)
	vec4 zinfo;     // x = near, y = far, z = 1 when the depth is already 0..1 clip (Vulkan), w = 1: scene_depth is bound (0: the terrain-grid depth below)
	vec4 rain;      // x = rainAmount 0..1 (water.hlsl's SharedWaterParams: the weather's rain intensity / 50, clamped; 0 = clear, no rings)
};

in vec3 v_rel;
in vec2 v_world;
in float v_depth;
in vec2 v_uv;
in vec4 v_clip;
out vec4 frag_color;

// The game's sampleNormal (forward/water.hlsl): a scrolling layer with a triangular fade, tangent space -> y up.
vec3 layer(vec2 coord, float map, vec2 dir, float speed, float time)
{
	float t = fract(time);
	vec3 n = texture(sampler2DArray(wnormal, rsmp), vec3(coord + dir * speed * t, map)).rgb * 2.0 - 1.0;
	return n.xzy * (1.0 - abs(t * 2.0 - 1.0));
}

// The game's rainRipple (forward/water.hlsl:50-59): one drop's expanding ring read from the packed rain-ripples map. The alpha channel is a per-drop
// time offset, r the ring's radius/weight, gb the ring's normal in xz; the sine of the ring phase flips the slope as it expands, so the ring reads as a
// growing wave. Coord is in the map's tiles (wrap).
vec3 rain_ripple(vec2 coord, float time, float weight)
{
	vec4 ripple = texture(sampler2D(rain_map, rsmp), coord);
	float drop_time = fract(ripple.a + time * 0.5);
	float drop_frac = drop_time - 1.0 + ripple.r * 0.5 + 0.5;
	float s = -sin(clamp(drop_frac * 16.0, 0.0, 2.0) * 3.14159);
	vec3 n = ripple.grb;
	n.xz *= s;
	return n * weight * ripple.r;
}

void main()
{
	float dist = length(v_rel);
	vec3 view = -v_rel / dist;
	int row = int(texelFetch(sampler2D(windex, psmp), clamp(ivec2(v_uv * 1024.0), ivec2(0), ivec2(1023)), 0).r * 255.0 + 0.5);
	vec4 t0 = texelFetch(sampler2D(wtable, psmp), ivec2(0, row), 0);	// gloss, invOpacity, invStrength, glow
	vec4 t1 = texelFetch(sampler2D(wtable, psmp), ivec2(1, row), 0);	// the VP scale: water scale X, Y, scum scale X, Y, each / 5000
	vec4 t2 = texelFetch(sampler2D(wtable, psmp), ivec2(2, row), 0);	// distortion (water, scum / mean scum scale), scum layer (< 0: none), normal layer
	float turb_layer = texelFetch(sampler2D(wtable, psmp), ivec2(3, row), 0).x;	// < 0: none
	float gloss = t0.x;
	float inv_strength = t0.z;
	vec3 wcol = texture(sampler2D(colour, wsmp), v_uv).rgb;
	vec4 info = texture(sampler2D(flow, wsmp), v_uv);

	// flow: direction and speed as the game derives them: direction = flow.rg (+ 0.6 x the biome's turbulence map at 0.234 x the normal coordinate) x speedMult
	// (the water scale), speed / distortion.x, time = gameTime x distortion.x.
	// gameTime's unit is untraced; seconds / 120 is ours (a 2 s cycle at distortion 60). The drift per second is then |direction| / 120 whatever the distortion.
	vec2 tc = v_world * t1.xy;
	vec2 dir = info.rg * 2.0 - 1.0;
	if (turb_layer >= 0.0) { dir += (texture(sampler2DArray(wturb, rsmp), vec3(tc * 0.234, turb_layer)).xy * 2.0 - 1.0) * 0.6; }
	dir *= t1.xy * 5000.0;
	float spd = length(dir);
	dir /= max(spd, 1e-4);
	spd /= t2.x;
	float time = misc.x / 120.0 * t2.x;
	vec3 n = vec3(0.0, 1.0, 0.0);
	n += layer(tc, t2.w, dir, spd, time);
	n += layer(tc + vec2(0.1, 0.3), t2.w, dir, spd, time + 0.33);
	n += layer(tc + vec2(0.4, 0.7), t2.w, dir, spd, time + 0.66);
	n.y *= inv_strength;
	// Rain ripples (water.hlsl:168-174, the material's rainMap): three tiling layers of the ring map at the game's world coordinates (map.zw = world xz x 0.01),
	// the second sampled on the swapped axes and shifted half a drop cycle; the weight fades with the view angle and the distance (the rain is seen looking down,
	// and fades out far away). rainAmount = the weather's rain intensity / 50, clamped: 0 in clear weather, so no rings.
	float rain_amt = rain.x * clamp(view.y * 2.0 - dist * 0.0001, 0.0, 1.0);
	if (rain_amt > 0.0)
	{
		vec2 rc = v_world * 0.01;
		n += rain_ripple(rc * 6.0, time * 1.5, rain_amt * 2.0);
		n += rain_ripple(rc.yx * 6.0, time * 1.5 + 0.5, rain_amt * 2.0);
		n += rain_ripple(rc * 2.0, time + 0.3, rain_amt * 0.8);
	}
	n = normalize(n);

	// reflection (water.hlsl:180-188): the projected coordinate (D3D: u right, v down, before the divide) moved by normal.xz x 60 and down by 12 x saturate(dist / 200)
	// ("to remove seams"), then rf = its red channel. Ours is GL (v up) and the target is flipped left to right.
	float rf = 1.0;
	if (refl.x > 0.5)
	{
		vec2 d3d = (vec2(v_clip.x, -v_clip.y) * 0.5 + 0.5 * v_clip.w + n.xz * 60.0 + vec2(0.0, 12.0 * clamp(dist * 0.005, 0.0, 1.0))) / v_clip.w;
		rf = clamp(texture(sampler2D(reflection, wsmp), offscreen_uv(vec2(1.0 - d3d.x, 1.0 - d3d.y))).r, 0.0, 1.0);	// the game's target is 8 bit
	}

	// scum (the biome's texture scum, none without one): opaque, rougher, blended in by the flow map's blue channel x the scum normal's alpha;
	// its coordinate moves with the water normal x distortion.y
	float scum = 0.0;
	if (misc.y > 0.5 && t2.z >= 0.0)
	{
		vec3 sc = vec3(v_world * t1.zw + n.xz * t2.y, t2.z);
		vec4 sd = texture(sampler2DArray(scum_dif, rsmp), sc);
		vec4 sn = texture(sampler2DArray(scum_nml, rsmp), sc);
		scum = info.b * sn.a;
		gloss = mix(gloss, sd.a, clamp(scum * 4.0, 0.0, 1.0));
		wcol = mix(wcol, sd.rgb, scum);
		n = normalize(mix(n, sn.xzy, scum));	// water.hlsl:195 uses the sampled scum normal raw (not *2-1 as the water normal map)
	}
	n = normalize(mix(n, vec3(0.0, 1.0, 0.0), smoothstep(6000.0, 40000.0, dist)));	// far away the ripples would only shimmer

	float ndv = max(dot(view, n), 0.0);
	// water.hlsl: colour = (sunDiffuse + envDiffuse) x albedo + sunSpec + envSpec x rf, the deferred light functions (CalcPunctualLight GGX, no shadow) with the
	// raw sunColour.rgb (misc.w scales sun_col back to it, through the shadow factor); rf = the planar reflection's red channel (above)
	// water.hlsl:204-206 scales the env specular by envColour.w twice (rgb x w, then rf x w; the diffuse once): light_spec has the first factor, env_w2 the second
	float env_w2 = zenith.w * clamp(5.0 * sun_dir.y + 0.2, 0.1, 1.0);
	vec3 col = light_shade(wcol, n, misc.w, vec4(0.0, 0.0, 0.0, 1.0)) + light_spec(n, view, gloss, 1.0) * rf * env_w2 + light_spec_sun(n, view, gloss, misc.w);
	col += t0.w * texture(sampler2D(colour, wsmp), v_uv).rgb * (1.0 - scum) * emissive_on();	// water glow x the water colour, not on scum

	// the game's alpha: transparent looking down (fresnel ~ 1), opaque at grazing angles and where scum covers; soft shore
	// water.hlsl:154-157: depth = max(0, G-buffer distance - distance of the water point), the euclidean length along the view ray to whatever is opaque behind (the
	// terrain further along the ray, a bridge pier), not the vertical depth under the point. The scene point lies on the same pixel ray, so its distance is dist x
	// (linear z of the scene / linear z of the water). A sky pixel (depth cleared) is farClip: opaque.
	float depth_ray = v_depth / clamp(view.y, 0.15, 1.0);	// without the scene depth: the vertical depth under the point, along the view ray
	float depth_gb = v_depth;
	if (zinfo.w > 0.5)
	{
		ivec2 sz = textureSize(sampler2D(scene_depth, psmp), 0);
		float d = texelFetch(sampler2D(scene_depth, psmp), min(ivec2(gl_FragCoord.xy), sz - 1), 0).r;
		float n = zinfo.x;
		float fz = zinfo.y;
		float zs = d >= 0.99999 ? fz : (zinfo.z > 0.5 ? n * fz / (fz - d * (fz - n)) : 2.0 * n * fz / ((fz + n) - (d * 2.0 - 1.0) * (fz - n)));
		depth_ray = max(0.0, dist * (zs / v_clip.w - 1.0));
		depth_gb = depth_ray;
	}
	float f = (1.0 - pow(1.0 - ndv, 2.0)) * (1.0 - scum);
	float a = mix(1.0, clamp(depth_ray * t0.y, 0.0, 1.0), f) * clamp(depth_gb / 2.0, 0.0, 1.0);
	frag_color = vec4(misc.z > 0.5 ? wcol : light_finish(col, v_rel, FOG_MAP_RAY(fog_map, fog_smp, v_world, v_rel.xz)), a);
}
@end

@image_sample_type hmap unfilterable_float
@image_sample_type scene_depth unfilterable_float
@sampler_type smp nonfiltering
@image_sample_type windex unfilterable_float
@image_sample_type wtable unfilterable_float
@sampler_type psmp nonfiltering

@program water vs fs
