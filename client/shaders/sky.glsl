// Sky: single-scattering atmosphere (O'Neil, "Accurate Atmospheric Scattering", GPU Gems 2 ch. 16, in SkyX's layout: one ray from the
// camera to the outer sphere, `scale()` optical-depth fit, Rayleigh + Henyey-Greenstein-style Mie phase), then night floor, starfield (rotating about the sun path's pole),
// one 2D cloud layer and the two moons. Everything is one fullscreen triangle drawn first; terrain and buildings draw over it.
// Constants come from the exe (docs/reference/SKY_EXE.md section 2), packed by src/sky.cpp so the CPU colour sampling and this shader share them.
@vs sky_vs
in vec2 pos;
out vec2 v_ndc;

void main()
{
	v_ndc = pos;
	gl_Position = vec4(pos, 0.0, 1.0);
}
@end

@include lighting.inc

@fs sky_fs
@include_block lighting
layout(binding=0) uniform texture2D star_tex;
layout(binding=1) uniform texture2D cloud_tile_tex;
layout(binding=4) uniform texture2D cloud_tex;
layout(binding=5) uniform texture2D cloud_nrm_tex;
layout(binding=2) uniform texture2D moon1_tex;
layout(binding=3) uniform texture2D moon2_tex;
layout(binding=0) uniform sampler sky_smp;

layout(binding=1) uniform sky_params
{
	vec4 cam_f;     // xyz = camera forward
	vec4 cam_r;     // xyz = camera right x tan(fov/2) x aspect
	vec4 cam_u;     // xyz = camera up x tan(fov/2)
	vec4 sun_real; // xyz = sun direction, NOT clamped to the horizon (SkyX sunDirectionReal)
	vec4 star_axis; // w = star UV scroll on both axes (SkyX uTime x 0.1, wrapped to 0..1); xyz unused
	vec4 atm0;      // x inner radius, y outer radius, z camera radius (inner + height), w scale depth
	vec4 atm1;      // x Kr4PI, y Km4PI, z KrESun, w KmESun
	vec4 atm2;      // xyz 1/wavelength^4, w exposure of the atmosphere
	vec4 atm3;      // x Mie g, y 1/(outer - inner)
	vec4 sky_mult;  // rgb = weather "sky color mult" (applied to the dome only for non-default weather: approximation)
	vec4 night;     // x = 1 when the star texture is loaded
	vec4 moon_a;    // xyz = moon 1 direction, w = angular radius (rad)
	vec4 moon_b;    // xyz = moon 2 direction, w = angular radius (rad)
	vec4 moon_c;    // x = spin 1 (rad), y = spin 2 (rad), z = texture LOD 1, w = texture LOD 2
	vec4 cloud0;    // x = density offset (clouds density x 1.4 - 0.8), y = darkness (pow(sat(density - 0.5), 0.3)), w = 0 off / 1 on
	vec4 cloud1;    // xy = wind offset (uv): the accumulated wind x 0.00005
	vec4 cloud_sun; // rgb = the exe's sunColour.rgb before its alpha (the layer's direct light)
};

in vec2 v_ndc;
out vec4 frag_color;

const float PI = 3.14159265;
const int SAMPLES = 4;	// the exe's NumberOfSamples

// Sky colour seen along unit direction d (d.y >= 0) with the light along unit direction l (any sign of l.y). HDR, atmosphere exposure applied.
vec3 atm_sky(vec3 d, vec3 l, out float thickness)
{
	// SkyX_Skydome.hlsl main_vp: the ray runs from the camera to the dome point (unit direction + inner radius), so its length is about 1, NOT the distance
	// to the outer shell (3.2 at the horizon: that gave a whitish, too bright low sky; kenshi-inspect findings/sky_dawn_exposure.md). Its tilt is kept for the phase too.
	float rc = atm0.z;
	float sosd = atm3.y / atm0.w;
	vec3 ray = d + vec3(0.0, atm0.x - rc, 0.0);
	float far = length(ray);
	d = ray / far;
	float off = exp(sosd * (atm0.x - rc)) * scale_fn(d.y, atm0.w);
	float sl = far / float(SAMPLES);
	vec3 sray = d * sl;
	vec3 p = vec3(0.0, rc, 0.0) + sray * 0.5;
	vec3 front = vec3(0.0);
	thickness = 0.0;
	for (int i = 0; i < SAMPLES; i++)
	{
		float h = length(p);
		float depth = exp(sosd * (atm0.x - h));
		float la = dot(l, p) / h;
		float ca = dot(d, p) / h;
		float scatter = off + depth * (scale_fn(la, atm0.w) - scale_fn(ca, atm0.w));
		front += exp(-scatter * (atm2.xyz * atm1.x + atm1.y)) * (depth * sl * atm3.y);
		thickness += depth * sl * atm3.y;
		p += sray;
	}
	// SkyX phase: cos is taken against the vector from the sky point back to the camera, so the Mie lobe sits on the sun
	float c = -dot(d, l);
	float rp = 0.75 * (1.0 + 0.5 * c * c);
	float g = atm3.x;
	float mp = 1.5 * ((1.0 - g * g) / (2.0 + g * g)) * (1.0 + c * c) / pow(1.0 + g * g - 2.0 * g * c, 1.5);
	return atm2.w * (rp * front * atm2.xyz * atm1.z + mp * front * atm1.w);
}

vec3 rot_axis(vec3 v, vec3 k, float a)
{
	return v * cos(a) + cross(k, v) * sin(a) + k * (dot(k, v) * (1.0 - cos(a)));
}

// moon.hlsl main_vs: Rayleigh in-scatter along the ray from uCameraPos to the unit-sphere point (dir + inner radius), 4 samples, added to the lit texture by main_fs.
// Constants: the LIVE SkyXFogParams (kenshi-inspect, greenlands_bridge 20:24), not the .program defaults (inner radius 1, exposure 2, wavelength G 11.76) the client
// used before: the live exposure is 1.4, inner radius 9.775, camera 9.7802, scale 1.918, scale depth 0.2606. The moon is not atmosphere fogged (that pass skips depth == far).
vec3 moon_scatter(vec3 d, vec3 l)
{
	const vec3 inv_wl = vec3(9.47328472, 18.8380127, 26.6802139);
	const float inner = 9.77501011, cam_y = 9.78022289, sd = 0.260644913, sosd = 7.35989285, scl = 1.91831863;
	const float kr4pi = 0.0276460163, km4pi = 0.00848230068, kresun = 0.066, expo = 1.4;
	vec3 start = vec3(0.0, cam_y, 0.0);
	vec3 ray = d + vec3(0.0, inner - cam_y, 0.0);
	float far = length(ray);
	ray /= far;
	float so = exp(sosd * (inner - cam_y)) * scale_fn(dot(ray, start) / cam_y, sd);
	float sl = far * 0.25;
	vec3 sray = ray * sl;
	vec3 sp = start + sray * 0.5;
	vec3 col = vec3(0.0);
	for (int i = 0; i < 4; i++)
	{
		float h = length(sp);
		float depth = exp(sosd * (inner - h));
		float scatter = so + depth * (scale_fn(dot(l, sp) / h, sd) - scale_fn(dot(ray, sp) / h, sd));
		col += exp(-scatter * (inv_wl * kr4pi + km4pi)) * (depth * sl * scl);
		sp += sray;
	}
	float c = dot(l, ray);
	return 0.75 * (1.0 + 0.5 * c * c) * col * (inv_wl * kresun) * expo;
}

// Equirect lookup of a mesh-space sphere normal (planet01.mesh is a UV sphere; the pole axis is the mesh Y), lod chosen on the CPU from the disc size.
// The mesh's own UVs: u = atan(x, z) / 2pi + 0.0185 on every vertex (seam near +z), v as below (kenshi-k16.2.47).
vec3 moon_sample(texture2D tex, vec3 n, float lod)
{
	float u = atan(n.x, n.z) / (2.0 * PI) + 0.0185;
	float v = 0.5 - asin(clamp(n.y, -1.0, 1.0)) / PI;
	return textureLod(sampler2D(tex, sky_smp), vec2(u, v), lod).rgb;	// raw like the world textures (forward/moon.material sets no gamma)
}

// One moon, ray-cast as a sphere of angular radius `radius` towards unit direction m. HDR rgb as moon.hlsl main_fs: texture x saturate(N . sunDirectionReal) + scatter,
// no fog, no ambient. N is the world normal; the texture is looked up with the mesh normal, which the spin about +Y rotated into N. w = disc coverage.
vec4 moon_disc(texture2D tex, vec3 d, vec3 m, float radius, float spin, float lod)
{
	float cs = dot(d, m);
	if (cs < cos(radius * 1.2)) { return vec4(0.0); }
	vec3 right = normalize(cross(vec3(0.0, 1.0, 0.0), m));
	vec3 up = cross(m, right);
	vec3 t = d / cs - m;
	vec2 q = vec2(dot(t, right), dot(t, up)) / tan(radius);
	float r2 = dot(q, q);
	float cover = 1.0 - smoothstep(0.97, 1.0, sqrt(r2));
	if (cover <= 0.0) { return vec4(0.0); }
	float z = sqrt(max(1.0 - r2, 0.0));
	vec3 n = right * q.x + up * q.y - m * z;
	vec3 mn = rot_axis(n, vec3(0.0, 1.0, 0.0), -spin);
	vec3 hdr = moon_sample(tex, mn, lod) * clamp(dot(n, sun_real.xyz), 0.0, 1.0) + moon_scatter(d, sun_real.xyz);
	return vec4(hdr, cover);
}

void main()
{
	vec3 d = normalize(cam_f.xyz + v_ndc.x * cam_r.xyz + v_ndc.y * cam_u.xyz);
	vec3 l = sun_real.xyz;
	float dy = max(d.y, 0.0);
	vec3 dd = normalize(vec3(d.x, max(d.y, 0.0005), d.z));

	float thick;
	vec3 raw = atm_sky(dd, l, thick);
	vec3 col = raw;	// SkyX_Skydome.hlsl has no weather tint: `sky color mult` only scales the ambient samples (SKY2_EXE), so sky_mult is no longer read here

	// SkyX_Skydome.hlsl, HDR variant: where the scattered sky is dark (nightmult) a dark blue floor and the starfield are added. The stars are not rotated
	// (SKY4_EXE): the dome's UV, fixed to the world axes, slides diagonally by the uTime scroll.
	float nm = clamp(1.0 - max(raw.r, max(raw.g, raw.b)) * 10.0, 0.0, 1.0) * (1.0 - clamp(thick * atm1.x, 0.0, 1.0));
	float h = 1.0 - dy;
	col += pow(vec3(0.05, 0.05, 0.10) * (2.0 - 0.75 * clamp(-l.y, 0.0, 1.0)) * (h * h * h), vec3(2.2)) * nm;
	vec3 stars = vec3(0.0);
	if (dy > 0.0 && night.x > 0.5)
	{
		float theta = acos(clamp(d.y, -1.0, 1.0));
		float phi = atan(d.z, d.x);
		// SkyX dome UV is polar, zenith (0.4, 0.4) after the VS x0.1, radius 0.4 at the horizon (theta 90 deg), u along world +x, v along +z (kenshi-inspect
		// findings/sky.md: the live dome mesh, 95 rings x 70 steps at 0.95 far clip); + uTime x 0.1; repeat sampler.
		vec2 uv = 0.4 + star_axis.w + 0.4 * (theta / (0.5 * PI)) * vec2(cos(phi), sin(phi));
		// sampled raw: the material's `gamma` flag would linearise it, but the game's night pixels (kinspect findings/sky_night.md, 00:06 N) match the raw
		// bytes (sky median / p90 1 / 7 both); linearised the stars came out ~9x too dim
		vec3 tex = texture(sampler2D(star_tex, sky_smp), uv).rgb;
		stars = tex * (0.35 + clamp(-l.y * 0.45, 0.0, 1.0)) * 2.0 * nm;
	}

	vec4 m1 = moon_disc(moon1_tex, d, moon_a.xyz, moon_a.w, moon_c.x, moon_c.z);
	vec4 m2 = moon_disc(moon2_tex, d, moon_b.xyz, moon_b.w, moon_c.y, moon_c.w);

	// The cloud layer: SkyX's one CloudsManager layer (SKY_EXE C33-C35), following the HDR branch of the game's SkyX_Clouds.hlsl; the live uniforms are in
	// kenshi-inspect findings/sky_dawn_exposure.md (height 100, uv scale 0.001, density x 3, volume 0.25, displacement 0.01, horizon fade 0.05). The density
	// is looked up twice: once on the flat plane, then on a plane raised where the density is thin and nudged along the normal map. It is lit by
	// zenithLight + sunColour (no shading towards the sun), darkened by the overcast term, alpha-blended over the dome.
	float calpha = 0.0;
	vec3 ccol = vec3(0.0);
	if (cloud0.w > 0.5 && d.y > 0.0)
	{
		const float height = 100.0, uvScale = 0.001, densMul = 3.0, volume = 0.25, displace = 0.01, fadeStart = 0.05;
		float off = cloud0.x;
		vec2 uv = d.xz * (height / d.y) * uvScale + cloud1.xy;
		float flat_dens = clamp((texture(sampler2D(cloud_tex, sky_smp), uv).r + off) * densMul, 0.0, 1.0);
		vec3 nt = 1.0 - 2.0 * texture(sampler2D(cloud_nrm_tex, sky_smp), uv).rgb;	// the normal map, negated; its green is the horizontal z
		vec3 dn = normalize(d + displace * d.y * vec3(nt.x, 0.0, nt.y));
		vec2 uv2 = dn.xz * ((height + height * (1.0 - flat_dens) * volume * d.y) / dn.y) * uvScale;
		float dens = (texture(sampler2D(cloud_tex, sky_smp), uv2 + cloud1.xy + vec2(0.2, 0.6)).r + off) * densMul;
		float tile = texture(sampler2D(cloud_tile_tex, sky_smp), uv2 - cloud1.xy).r;
		dens += tile * 0.1;
		ccol = zenith.rgb + cloud_sun.rgb * (1.0 - dens * 0.1);
		float fade = clamp(10.0 * clamp(d.y - fadeStart, 0.0, 1.0), 0.0, 1.0);	// 0 at the horizon, full from 0.15 up
		dens += fade;
		ccol = clamp(ccol * (1.0 - clamp(dens, 0.0, 1.0) * cloud0.y), 0.0, 1.0) * sqrt(atm2.w);
		calpha = clamp(mix(off + 0.5, dens * clamp(1.0 - tile + off, 0.0, 1.0), fade), 0.0, 1.0);
	}

	// compose the scene's display-referred HDR: sky, stars (hidden by clouds), cloud, moons (opaque, no fog) — the game's raw values, unexposed; the composite
	// then applies the exposure and the original's hard clip (tone.inc, k16.2.82). The cloud layer is the game's value too (the old pow(x e, 1/2.2) / e
	// compensation is gone).
	// The moons are opaque bodies behind the cloud layer (kenshi-k16.2.256.5): the layer's alpha, from the weather's `clouds density`, covers them as it covers the dome.
	col = mix(col, m1.rgb, m1.w);
	col = mix(col, m2.rgb, m2.w);
	col += stars * (1.0 - calpha) * (1.0 - max(m1.w, m2.w));
	col = mix(col, ccol, calpha);
	col = max(col * fog_col.w, vec3(0.0));	// HDR: the sun's core stays brighter than its glow; the display transform rolls it to white
	// fog.hlsl: far-clip pixels get no atmosphere term; only rays below the horizon (`edge`) take it, at the ground-plane hit
	frag_color = vec4(d.y < 0.0 ? atm_fog(d * 50000.0) : col * term_on(LT_SKY), 1.0);
}
@end

@program sky sky_vs sky_fs
