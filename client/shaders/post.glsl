// Optional post effects (post.hpp), after the exposure composite (hdr.glsl hdr_comp) wrote the exposed scene into an RGBA16F target. Values are display
// values as the game writes them (no gamma: LIGHTING.md section 4), so above 1.0 is what the swapchain clips.
//   post_grade : the display clip and the grade into its own target, clamped to 0..1 (what the swapchain shows); only when SMAA runs, so it sees graded values
//   smaa_edge / smaa_weight / smaa_blend : SMAA 1x, preset HIGH (smaa.inc, upstream unmodified; the tables from smaa_lut.cpp)
//   bloom_pre / bloom_down / bloom_up / bloom_comp : bloom from the unclipped exposed scene (ours; the original's chain has BLOOM_MAGNITUDE 0, LIGHTING.md
//                section 10): soft-knee threshold above display white into half res, then a dual-filter pyramid (Bjorge, "Bandwidth-Efficient Rendering",
//                SIGGRAPH 2015; our code) down to 1/32 and back up, each level added onto the next larger; scene_comp adds it to the scene like Composite_fp4's
//                bloom term
//   clarity_down / clarity_blur : clarity's base (ours): log2 luminance of the exposed scene at 1/4 res, then a separable 13-tap blur that weights each tap
//                by how close its log luminance is to the centre's (about 1 stop), so the base does not straddle a skyline or a lit wall against shade
//   scene_comp : the scene the grade reads: + bloom x strength, then clarity: luminance x 2^(clarity x (log luminance - base)), the base fetched by a joint
//                bilateral upsample (the 4 nearest base texels, weighted by log-luminance distance) and the detail faded out in deep shadow, in highlights
//                and where it is several stops (an edge the blur did not separate): the halo guard
//   dof_down / dof_blur / dof_comp : depth of field, far field only (ours): circle of confusion from the scene depth, 0 up to 1.2 x the focus distance
//                and rising behind it (nothing in front of the focus blurs: no near-field bleed over the HUD's subject); the input at half res with the
//                CoC in alpha, a 16-tap disc gather there in which each tap counts only as far as its own CoC reaches (sharp things do not smear into the
//                blur behind them), then a full-res mix by the full-res CoC, the half-res blur fetched joint-bilaterally by CoC
//   stylize    : post.outline / post.posterize (ours, opt-in): ink lines on the near side of scene-depth steps and on creases, then luminance in N soft
//                bands, hue kept. Dither guard: a step counts only where the 3x3 holds at least 3 texels of each side and the 5 texels across the centre on
//                each axis change surface at most once, a crease only where all 8 neighbours are the centre's surface, so the screen-door of the occluder cone, the build ghost and the distance fade (scattered single texels) draw none
//   post_final : the chain's output into the swapchain: the grade (unless post_grade ran), CAS sharpening, vignette, letterbox bars, then output dither =
//                triangular noise of +-1 output LSB, new each frame
@include origin.inc

// Every stage but SMAA's reads render targets only, and works on the image alone (no NDC maths: the vignette is symmetric), so it runs in the target's own
// row order: uv is the render target's uv (origin.inc), the same on every backend for the same pixel. SMAA keeps the logical uv and flips each read (smaa_lang).
@vs post_vs
@include_block origin
out vec2 uv;

void main()
{
	vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
	uv = offscreen_uv(p);
	gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
@end

@include tone.inc

@block grade
@include_block tone
// g: x = shoulder 0..1, y = black point, z = vibrance, w = warmth (all 0 = untouched).
// tone.inc applies the original's display first (display-referred HDR in, hard-clipped 0..1 out); the rest of the grade is display-space (post.contrast is retired, k16.2.82).
vec3 grade(vec3 c, vec4 g)
{
	c = tone_map(c);
	c = max(c - g.y, 0.0) / (1.0 - g.y);
	float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
	if (g.z != 0.0)
	{
		float sat = clamp((max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b))) / max(max(c.r, max(c.g, c.b)), 1e-4), 0.0, 1.0);
		c = max(mix(vec3(l), c, 1.0 + g.z * (1.0 - sat)), 0.0);	// vibrance: saturation up where it is low
	}
	// warmth: cool shadows, warm highlights, by luminance
	if (g.w != 0.0) { c *= 1.0 + g.w * mix(vec3(-0.15, 0.0, 0.2), vec3(0.2, 0.05, -0.2), smoothstep(0.0, 1.0, l)); }
	// shoulder: above the knee the brightest channel rolls off exponentially toward 1.0 (slope 1 at the knee) and the others scale with it, so the hue
	// survives where the plain output clips per channel
	if (g.x > 0.0)
	{
		float knee = 1.0 - 0.4 * g.x;
		float m = max(c.r, max(c.g, c.b));
		if (m > knee) { c *= (knee + (1.0 - knee) * (1.0 - exp((knee - m) / (1.0 - knee)))) / m; }
	}
	return c;
}
@end

@fs post_grade_fs
@include_block grade
layout(binding=0) uniform texture2D src_tex;
layout(binding=0) uniform sampler smp;
layout(binding=0) uniform post_grade_params
{
	vec4 g;
};
in vec2 uv;
out vec4 frag_color;

void main()
{
	frag_color = vec4(clamp(grade(textureLod(sampler2D(src_tex, smp), uv, 0.0).rgb, g), 0.0, 1.0), 1.0);
}
@end

// SMAA's SMAA_CUSTOM_SL hook (smaa.inc "Porting Functions"): separate textures and samplers, as sokol-shdc wants them. smaa_lin / smaa_pt are the
// samplers each SMAA stage declares; SMAA_RT_METRICS = (1 / width, 1 / height, width, height).
@block smaa_lang
@include_block origin
#define SMAA_CUSTOM_SL 1
#define SMAA_PRESET_HIGH 1
#define SMAA_RT_METRICS rt
#define SMAATexture2D(tex) texture2D tex
#define SMAATexturePass2D(tex) tex
// SMAA works on the logical uv (v up) on every backend, as on GL; its render target reads (the parameter names smaa.inc gives them: colorTex, edgesTex,
// blendTex) go through origin.inc, its two lookup tables (areaTex, searchTex: uploaded images, not render targets) do not.
#define SMAA_UV_colorTex(c) offscreen_uv(c)
#define SMAA_UV_edgesTex(c) offscreen_uv(c)
#define SMAA_UV_blendTex(c) offscreen_uv(c)
#define SMAA_UV_tex(c) offscreen_uv(c)	// the depth edge detection and the generic helpers (unused by our stages, compiled all the same)
#define SMAA_UV_velocityTex(c) offscreen_uv(c)
#define SMAA_UV_currentColorTex(c) offscreen_uv(c)
#define SMAA_UV_previousColorTex(c) offscreen_uv(c)
#define SMAA_UV_areaTex(c) (c)
#define SMAA_UV_searchTex(c) (c)
#define SMAASampleLevelZero(tex, coord) textureLod(sampler2D(tex, smaa_lin), SMAA_UV_##tex(coord), 0.0)
#define SMAASampleLevelZeroPoint(tex, coord) textureLod(sampler2D(tex, smaa_pt), SMAA_UV_##tex(coord), 0.0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) textureLodOffset(sampler2D(tex, smaa_lin), SMAA_UV_##tex(coord), 0.0, offscreen_offset(offset))
#define SMAASample(tex, coord) texture(sampler2D(tex, smaa_lin), SMAA_UV_##tex(coord))
#define SMAASamplePoint(tex, coord) texture(sampler2D(tex, smaa_pt), SMAA_UV_##tex(coord))
#define SMAASampleOffset(tex, coord, offset) textureOffset(sampler2D(tex, smaa_lin), SMAA_UV_##tex(coord), offscreen_offset(offset))
#define SMAA_FLATTEN
#define SMAA_BRANCH
#define lerp(a, b, t) mix(a, b, t)
#define saturate(a) clamp(a, 0.0, 1.0)
#define mad(a, b, c) (a * b + c)
#define float2 vec2
#define float3 vec3
#define float4 vec4
#define int2 ivec2
#define int3 ivec3
#define int4 ivec4
#define bool2 bvec2
#define bool3 bvec3
#define bool4 bvec4
@end

// The vertex stages sample nothing, but smaa.inc's shared helpers still name the samplers: stub the sample macros there.
@block smaa_vs_stubs
#undef SMAASampleLevelZero
#undef SMAASampleLevelZeroPoint
#undef SMAASampleLevelZeroOffset
#undef SMAASample
#undef SMAASamplePoint
#undef SMAASampleOffset
#define SMAASampleLevelZero(tex, coord) vec4(0.0)
#define SMAASampleLevelZeroPoint(tex, coord) vec4(0.0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) vec4(0.0)
#define SMAASample(tex, coord) vec4(0.0)
#define SMAASamplePoint(tex, coord) vec4(0.0)
#define SMAASampleOffset(tex, coord, offset) vec4(0.0)
@end

// SMAA itself, once (sokol-shdc includes a file once per source); each stage first picks its half with SMAA_INCLUDE_VS / _PS
@block smaa_lib
@include smaa.inc
@end

@vs smaa_edge_vs
@include_block smaa_lang
@include_block smaa_vs_stubs
#define SMAA_INCLUDE_PS 0
layout(binding=0) uniform smaa_vs_params
{
	vec4 rt;
};
@include_block smaa_lib
out vec2 uv;
out vec4 off0;
out vec4 off1;
out vec4 off2;

void main()
{
	vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
	uv = p;
	vec4 o[3];
	SMAAEdgeDetectionVS(uv, o);
	off0 = o[0];
	off1 = o[1];
	off2 = o[2];
	gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
@end

@fs smaa_edge_fs
@include_block smaa_lang
#define SMAA_INCLUDE_VS 0
layout(binding=0) uniform texture2D color_tex;
layout(binding=0) uniform sampler smaa_lin;
layout(binding=1) uniform sampler smaa_pt;
layout(binding=1) uniform smaa_fs_params
{
	vec4 rt;
};
@include_block smaa_lib
in vec2 uv;
in vec4 off0;
in vec4 off1;
in vec4 off2;
out vec4 frag_color;

void main()
{
	vec4 o[3] = vec4[3](off0, off1, off2);
	frag_color = vec4(SMAALumaEdgeDetectionPS(uv, o, color_tex), 0.0, 0.0);
}
@end

@vs smaa_weight_vs
@include_block smaa_lang
@include_block smaa_vs_stubs
#define SMAA_INCLUDE_PS 0
layout(binding=0) uniform smaa_vs_params
{
	vec4 rt;
};
@include_block smaa_lib
out vec2 uv;
out vec2 pix;
out vec4 off0;
out vec4 off1;
out vec4 off2;

void main()
{
	vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
	uv = p;
	vec4 o[3];
	SMAABlendingWeightCalculationVS(uv, pix, o);
	off0 = o[0];
	off1 = o[1];
	off2 = o[2];
	gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
@end

@fs smaa_weight_fs
@include_block smaa_lang
#define SMAA_INCLUDE_VS 0
layout(binding=0) uniform texture2D edges_tex;
layout(binding=1) uniform texture2D area_tex;
layout(binding=2) uniform texture2D search_tex;
layout(binding=0) uniform sampler smaa_lin;
layout(binding=1) uniform sampler smaa_pt;
layout(binding=1) uniform smaa_fs_params
{
	vec4 rt;
};
@include_block smaa_lib
in vec2 uv;
in vec2 pix;
in vec4 off0;
in vec4 off1;
in vec4 off2;
out vec4 frag_color;

void main()
{
	vec4 o[3] = vec4[3](off0, off1, off2);
	frag_color = SMAABlendingWeightCalculationPS(uv, pix, o, edges_tex, area_tex, search_tex, vec4(0.0));
}
@end

@vs smaa_blend_vs
@include_block smaa_lang
@include_block smaa_vs_stubs
#define SMAA_INCLUDE_PS 0
layout(binding=0) uniform smaa_vs_params
{
	vec4 rt;
};
@include_block smaa_lib
out vec2 uv;
out vec4 off;

void main()
{
	vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
	uv = p;
	SMAANeighborhoodBlendingVS(uv, off);
	gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
@end

@fs smaa_blend_fs
@include_block smaa_lang
#define SMAA_INCLUDE_VS 0
layout(binding=0) uniform texture2D color_tex;
layout(binding=1) uniform texture2D blend_tex;
layout(binding=0) uniform sampler smaa_lin;
layout(binding=1) uniform sampler smaa_pt;
layout(binding=1) uniform smaa_fs_params
{
	vec4 rt;
};
@include_block smaa_lib
in vec2 uv;
in vec4 off;
out vec4 frag_color;

void main()
{
	frag_color = SMAANeighborhoodBlendingPS(uv, off, color_tex, blend_tex);
}
@end

@block dof_coc
layout(binding=0) uniform dof_params
{
	vec4 dz;	// x = near, y = far, z = 1 when the depth is already 0..1 clip (Vulkan), w = focus distance
	vec4 dr;	// x = max blur radius (half-res pixels), yz = half-res texel size, w = 1 / that radius
	vec4 ds;	// xy = full-res texel size
};

float coc(float d)
{
	float zn = dz.x, zf = dz.y;
	float z = dz.z > 0.5 ? zn * zf / (zf - d * (zf - zn)) : 2.0 * zn * zf / ((zf + zn) - (d * 2.0 - 1.0) * (zf - zn));
	return clamp(1.0 - 1.2 * dz.w / z, 0.0, 1.0);
}
@end

@fs dof_down_fs
@include_block dof_coc
layout(binding=0) uniform texture2D src_tex;
layout(binding=1) uniform texture2D dof_depth;
layout(binding=0) uniform sampler smp;
layout(binding=1) uniform sampler dof_pt;
in vec2 uv;
out vec4 frag_color;

// the 2x2 full-res block under the half-res texel: colour by bilinear, CoC the smallest of the four (an in-focus edge stays in focus)
void main()
{
	float c = 1.0;
	for (int i = 0; i < 4; i++)
	{
		vec2 o = vec2(float(i & 1), float(i >> 1)) - 0.5;
		c = min(c, coc(textureLod(sampler2D(dof_depth, dof_pt), uv + o * ds.xy, 0.0).r));
	}
	frag_color = vec4(textureLod(sampler2D(src_tex, smp), uv, 0.0).rgb, c);
}
@end

@fs dof_blur_fs
@include_block dof_coc
layout(binding=0) uniform texture2D src_tex;
layout(binding=0) uniform sampler smp;
in vec2 uv;
out vec4 frag_color;

void main()
{
	vec4 c = textureLod(sampler2D(src_tex, smp), uv, 0.0);
	float r = c.a * dr.x;
	if (r < 0.5)
	{
		frag_color = c;
		return;
	}
	vec3 sum = c.rgb;
	float wsum = 1.0;
	const int kTaps = 16;
	for (int i = 0; i < kTaps; i++)
	{
		float a = sqrt((float(i) + 0.5) / float(kTaps));	// Vogel disc: even density
		float ang = float(i) * 2.39996323;
		float d = a * r;
		vec4 t = textureLod(sampler2D(src_tex, smp), uv + vec2(cos(ang), sin(ang)) * d * dr.yz, 0.0);
		float w = clamp(t.a * dr.x - d + 1.0, 0.0, 1.0);	// the tap's own blur must reach this pixel
		sum += t.rgb * w;
		wsum += w;
	}
	frag_color = vec4(sum / wsum, c.a);
}
@end

@fs dof_comp_fs
@include_block dof_coc
layout(binding=0) uniform texture2D src_tex;
layout(binding=1) uniform texture2D dof_depth;
layout(binding=2) uniform texture2D blur_tex;
layout(binding=0) uniform sampler smp;
layout(binding=1) uniform sampler dof_pt;
in vec2 uv;
out vec4 frag_color;

void main()
{
	vec3 s = textureLod(sampler2D(src_tex, smp), uv, 0.0).rgb;
	float c = coc(textureLod(sampler2D(dof_depth, dof_pt), uv, 0.0).r);
	if (c * dr.x < 0.25)
	{
		frag_color = vec4(s, 1.0);
		return;
	}
	vec2 hs = 1.0 / dr.yz;
	vec2 p = uv * hs - 0.5, f = fract(p);
	vec2 t0 = (floor(p) + 0.5) * dr.yz;
	vec3 b = vec3(0.0);
	float wsum = 0.0;
	for (int i = 0; i < 4; i++)
	{
		vec2 o = vec2(float(i & 1), float(i >> 1));
		vec4 t = textureLod(sampler2D(blur_tex, smp), t0 + o * dr.yz, 0.0);
		vec2 bw = mix(1.0 - f, f, o);
		float w = bw.x * bw.y * max(1.0 - abs(t.a - c) * 4.0, 0.0) + 1e-5;	// texels of another CoC (an in-focus edge) count for little
		b += t.rgb * w;
		wsum += w;
	}
	frag_color = vec4(mix(s, b / wsum, smoothstep(0.0, 1.0, c * dr.x * 2.0)), 1.0);
}
@end

@fs stylize_fs
layout(binding=0) uniform texture2D src_tex;
layout(binding=1) uniform texture2D dof_depth;
layout(binding=0) uniform sampler smp;
layout(binding=1) uniform sampler dof_pt;
layout(binding=0) uniform stylize_params
{
	vec4 sz;	// x = near, y = far, z = 1 when the depth is already 0..1 clip (Vulkan)
	vec4 st;	// x = outline 0..1, y = posterize levels (0 = off), zw = texel size
};
in vec2 uv;
out vec4 frag_color;

float view_z(vec2 o)
{
	float d = textureLod(sampler2D(dof_depth, dof_pt), uv + o * st.zw, 0.0).r;
	float zn = sz.x, zf = sz.y;
	return sz.z > 0.5 ? zn * zf / (zf - d * (zf - zn)) : 2.0 * zn * zf / ((zf + zn) - (d * 2.0 - 1.0) * (zf - zn));
}

// surface changes along the 5 texels through the centre on axis a (m = the -1 neighbour, p = the +1): a silhouette makes one, dither alternates
int flips(float z, vec2 a, float m, float p)
{
	float zz[5] = float[5](view_z(-2.0 * a), m, z, p, view_z(2.0 * a));
	int n = 0;
	for (int i = 0; i < 4; i++)
	{
		bool s0 = abs(zz[i] - z) < 0.04 * z, s1 = abs(zz[i + 1] - z) < 0.04 * z;
		if (s0 != s1) { n++; }
	}
	return n;
}

void main()
{
	vec3 c = textureLod(sampler2D(src_tex, smp), uv, 0.0).rgb;
	if (st.y > 0.5)
	{
		float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
		float x = min(l, 1.0) * st.y;
		float q = (floor(x) + smoothstep(0.35, 0.65, fract(x))) / st.y;	// soft steps: no aliasing staircase on gradients
		c *= l > 1e-4 ? (q + max(l - 1.0, 0.0)) / l : 1.0;
	}
	if (st.x > 0.0)
	{
		float z = view_z(vec2(0.0));
		float zs[8];
		zs[0] = view_z(vec2(-1.0, -1.0)); zs[1] = view_z(vec2(0.0, -1.0)); zs[2] = view_z(vec2(1.0, -1.0)); zs[3] = view_z(vec2(-1.0, 0.0));
		zs[4] = view_z(vec2(1.0, 0.0)); zs[5] = view_z(vec2(-1.0, 1.0)); zs[6] = view_z(vec2(0.0, 1.0)); zs[7] = view_z(vec2(1.0, 1.0));
		int same = 0, far = 0;
		for (int i = 0; i < 8; i++)
		{
			float rel = (zs[i] - z) / z;
			if (abs(rel) < 0.04) { same++; }
			else if (rel > 0.08) { far++; }
		}
		float e = same >= 3 && far >= 3 && flips(z, vec2(1.0, 0.0), zs[3], zs[4]) <= 1 && flips(z, vec2(0.0, 1.0), zs[1], zs[6]) <= 1 ? 1.0 : 0.0;
		if (same == 8)	// crease: the second difference across the centre, relative to the depth
		{
			float k = max(abs(zs[3] + zs[4] - 2.0 * z), abs(zs[1] + zs[6] - 2.0 * z)) / z;
			e = smoothstep(0.004, 0.012, k);
		}
		e *= 1.0 - smoothstep(4000.0, 12000.0, z);	// far terrain would turn to noise
		c *= 1.0 - st.x * e;
	}
	frag_color = vec4(c, 1.0);
}
@end

@fs post_final_fs
@include_block grade
layout(binding=0) uniform texture2D src_tex;
layout(binding=0) uniform sampler smp;
layout(binding=0) uniform post_final_params
{
	vec4 p;		// x = dither amplitude (one output LSB, 0 = off), y = frame number, z = vignette 0..1, w = letterbox bar height (pixels, 0 = none)
	vec4 g;		// the grade (grade()); unused when s.y = 1
	vec4 s;		// x = sharpen 0..1 (0 = off), y = 1: src_tex is already graded (post_grade ran), zw = texel size
};
in vec2 uv;
out vec4 frag_color;

// pcg2d (Jarzynski & Olano, "Hash Functions for GPU Rendering", JCGT 2020): two uniform values per pixel and frame
vec2 hash2(uvec3 v)
{
	uvec2 q = v.xy * 1664525u + 1013904223u + v.z * 747796405u;
	q.x += q.y * 1664525u;
	q.y += q.x * 1664525u;
	q ^= q >> 16u;
	q.x += q.y * 1664525u;
	q.y += q.x * 1664525u;
	q ^= q >> 16u;
	return vec2(q) * (1.0 / 4294967296.0);
}

vec3 tap(float x, float y)
{
	vec3 c = textureLod(sampler2D(src_tex, smp), uv + vec2(x, y) * s.zw, 0.0).rgb;
	return s.y > 0.5 ? c : clamp(grade(c, g), 0.0, 1.0);
}

// Contrast adaptive sharpening (AMD FidelityFX CAS, MIT; the no-scaling CasFilter path): a negative-lobe cross filter whose weight shrinks where the
// 3x3 neighbourhood is already near black or white, so edges sharpen without ringing or clipping.
vec3 cas(vec3 e)
{
	vec3 a = tap(-1.0, -1.0), b = tap(0.0, -1.0), c = tap(1.0, -1.0);
	vec3 d = tap(-1.0, 0.0), f = tap(1.0, 0.0);
	vec3 gg = tap(-1.0, 1.0), h = tap(0.0, 1.0), i = tap(1.0, 1.0);
	vec3 mn = min(min(min(d, e), min(f, b)), h);
	vec3 mx = max(max(max(d, e), max(f, b)), h);
	mn += min(mn, min(min(a, c), min(gg, i)));	// the cross plus the full 3x3: 0..2
	mx += max(mx, max(max(a, c), max(gg, i)));
	vec3 amp = sqrt(clamp(min(mn, 2.0 - mx) / max(mx, 1e-5), 0.0, 1.0));
	vec3 w = amp * (-1.0 / mix(8.0, 5.0, s.x));
	return clamp(((b + d + f + h) * w + e) / (1.0 + 4.0 * w), 0.0, 1.0);
}

void main()
{
	vec3 c;
	if (s.x > 0.0) { c = cas(tap(0.0, 0.0)); }
	else
	{
		c = textureLod(sampler2D(src_tex, smp), uv, 0.0).rgb;
		if (s.y < 0.5) { c = grade(c, g); }	// the display's clip, then the grade
	}
	if (p.z > 0.0)
	{
		vec2 d = (uv - 0.5) * vec2(s.w / s.z, 1.0);	// aspect-corrected: round, not screen-shaped
		c *= 1.0 - p.z * smoothstep(0.35, 1.0, length(d) / length(vec2(0.5 * s.w / s.z, 0.5)));
	}
	if (gl_FragCoord.y < p.w || gl_FragCoord.y > 1.0 / s.w - p.w)
	{
		frag_color = vec4(0.0, 0.0, 0.0, 1.0);
		return;
	}
	if (p.x > 0.0)
	{
		vec2 r = hash2(uvec3(uvec2(gl_FragCoord.xy), uint(p.y)));
		c += (r.x + r.y - 1.0) * p.x;
	}
	frag_color = vec4(c, 1.0);
}
@end

@fs bloom_pre_fs
layout(binding=0) uniform texture2D src_tex;
layout(binding=0) uniform sampler smp;
layout(binding=0) uniform bloom_params
{
	vec4 b;		// xy = source texel size, z = threshold, w = knee (pre); xy = source texel size (down / up)
};
in vec2 uv;
out vec4 frag_color;

// one bilinear tap of a 2x2 source block, thresholded and weighted by 1 / (1 + luma) (Karis) so single bright pixels do not flicker.
// A non-finite tap (a NaN or Inf scene pixel) gives nothing: the pyramid would spread one bad pixel into a black octagon (kenshi-k16.2.60).
vec4 pre(vec2 o)
{
	vec3 c = textureLod(sampler2D(src_tex, smp), uv + o * b.xy, 0.0).rgb;
	if (any(isnan(c)) || any(isinf(c))) { return vec4(0.0); }
	float br = max(c.r, max(c.g, c.b));
	float soft = clamp(br - b.z + b.w, 0.0, 2.0 * b.w);
	c *= max(soft * soft / (4.0 * b.w + 1e-4), br - b.z) / max(br, 1e-4);
	float w = 1.0 / (1.0 + dot(c, vec3(0.2126, 0.7152, 0.0722)));
	return vec4(c * w, w);
}

void main()
{
	vec4 s = pre(vec2(-1.0, -1.0)) + pre(vec2(1.0, -1.0)) + pre(vec2(-1.0, 1.0)) + pre(vec2(1.0, 1.0));
	frag_color = vec4(s.rgb / max(s.a, 1e-4), 1.0);
}
@end

@fs bloom_down_fs
layout(binding=0) uniform texture2D src_tex;
layout(binding=0) uniform sampler smp;
layout(binding=0) uniform bloom_params
{
	vec4 b;
};
in vec2 uv;
out vec4 frag_color;

void main()
{
	vec3 c = textureLod(sampler2D(src_tex, smp), uv, 0.0).rgb * 4.0;
	c += textureLod(sampler2D(src_tex, smp), uv + vec2(-1.0, -1.0) * b.xy, 0.0).rgb;
	c += textureLod(sampler2D(src_tex, smp), uv + vec2(1.0, -1.0) * b.xy, 0.0).rgb;
	c += textureLod(sampler2D(src_tex, smp), uv + vec2(-1.0, 1.0) * b.xy, 0.0).rgb;
	c += textureLod(sampler2D(src_tex, smp), uv + vec2(1.0, 1.0) * b.xy, 0.0).rgb;
	frag_color = vec4(c * 0.125, 1.0);
}
@end

@fs bloom_up_fs
layout(binding=0) uniform texture2D src_tex;
layout(binding=0) uniform sampler smp;
layout(binding=0) uniform bloom_params
{
	vec4 b;
};
in vec2 uv;
out vec4 frag_color;

// the dual filter's tent: 4 edge taps x1, 4 diagonal taps x2; the pipeline adds it onto the level below
void main()
{
	vec3 c = vec3(0.0);
	c += textureLod(sampler2D(src_tex, smp), uv + vec2(-2.0, 0.0) * b.xy, 0.0).rgb;
	c += textureLod(sampler2D(src_tex, smp), uv + vec2(2.0, 0.0) * b.xy, 0.0).rgb;
	c += textureLod(sampler2D(src_tex, smp), uv + vec2(0.0, -2.0) * b.xy, 0.0).rgb;
	c += textureLod(sampler2D(src_tex, smp), uv + vec2(0.0, 2.0) * b.xy, 0.0).rgb;
	c += textureLod(sampler2D(src_tex, smp), uv + vec2(-1.0, -1.0) * b.xy, 0.0).rgb * 2.0;
	c += textureLod(sampler2D(src_tex, smp), uv + vec2(1.0, -1.0) * b.xy, 0.0).rgb * 2.0;
	c += textureLod(sampler2D(src_tex, smp), uv + vec2(-1.0, 1.0) * b.xy, 0.0).rgb * 2.0;
	c += textureLod(sampler2D(src_tex, smp), uv + vec2(1.0, 1.0) * b.xy, 0.0).rgb * 2.0;
	frag_color = vec4(c / 12.0, 1.0);
}
@end

@fs clarity_down_fs
layout(binding=0) uniform texture2D src_tex;
layout(binding=0) uniform sampler smp;
layout(binding=0) uniform clarity_params
{
	vec4 c;		// xy = source texel size (down); xy = one base texel along the blur axis (blur)
};
in vec2 uv;
out vec4 frag_color;

float log_luma(vec2 o)
{
	float y = dot(textureLod(sampler2D(src_tex, smp), uv + o * c.xy, 0.0).rgb, vec3(0.2126, 0.7152, 0.0722));
	return isnan(y) || isinf(y) ? log2(1e-4) : log2(max(y, 1e-4));	// a non-finite pixel would spread through the blur (as bloom_pre)
}

// 4 bilinear taps: the 4x4 source block under the base texel
void main()
{
	frag_color = vec4(0.25 * (log_luma(vec2(-1.0, -1.0)) + log_luma(vec2(1.0, -1.0)) + log_luma(vec2(-1.0, 1.0)) + log_luma(vec2(1.0, 1.0))), 0.0, 0.0, 1.0);
}
@end

@fs clarity_blur_fs
layout(binding=0) uniform texture2D src_tex;
layout(binding=0) uniform sampler smp;
layout(binding=0) uniform clarity_params
{
	vec4 c;
};
in vec2 uv;
out vec4 frag_color;

void main()
{
	float l = textureLod(sampler2D(src_tex, smp), uv, 0.0).r;
	float sum = l, wsum = 1.0;
	for (int i = -6; i <= 6; i++)
	{
		if (i == 0) { continue; }
		float t = textureLod(sampler2D(src_tex, smp), uv + c.xy * float(i), 0.0).r;
		float w = exp(-float(i * i) / 18.0 - 0.5 * (t - l) * (t - l));
		sum += w * t;
		wsum += w;
	}
	frag_color = vec4(sum / wsum, 0.0, 0.0, 1.0);
}
@end

@fs scene_comp_fs
layout(binding=0) uniform texture2D src_tex;
layout(binding=1) uniform texture2D bloom_tex;
layout(binding=2) uniform texture2D base_tex;
layout(binding=0) uniform sampler smp;
layout(binding=1) uniform sampler lin;
layout(binding=0) uniform comp_params
{
	vec4 b;		// x = bloom strength (the up chain sums the levels), y = clarity, zw = the base's size
};
in vec2 uv;
out vec4 frag_color;

void main()
{
	vec3 s = textureLod(sampler2D(src_tex, smp), uv, 0.0).rgb;
	vec3 c = s + textureLod(sampler2D(bloom_tex, lin), uv, 0.0).rgb * b.x;
	if (b.y > 0.0)
	{
		float l = log2(max(dot(s, vec3(0.2126, 0.7152, 0.0722)), 1e-4));
		vec2 p = uv * b.zw - 0.5, f = fract(p);
		vec2 t0 = (floor(p) + 0.5) / b.zw, dt = 1.0 / b.zw;
		float base = 0.0, wsum = 1e-4;
		for (int i = 0; i < 4; i++)
		{
			vec2 o = vec2(float(i & 1), float(i >> 1));
			float t = textureLod(sampler2D(base_tex, smp), t0 + o * dt, 0.0).r;
			vec2 bw = mix(1.0 - f, f, o);
			float w = bw.x * bw.y * exp(-0.5 * (t - l) * (t - l)) + 1e-5;
			base += w * t;
			wsum += w;
		}
		float d = l - base / wsum;
		float m = smoothstep(-8.0, -5.0, l) * (1.0 - smoothstep(-0.5, 0.5, l)) * exp(-d * d / 4.0);
		c *= exp2(clamp(b.y * 1.5 * d * m, -1.0, 1.0));
	}
	frag_color = vec4(c, 1.0);
}
@end

@program post_grade post_vs post_grade_fs
@program smaa_edge smaa_edge_vs smaa_edge_fs
@program smaa_weight smaa_weight_vs smaa_weight_fs
@program smaa_blend smaa_blend_vs smaa_blend_fs
@program post_final post_vs post_final_fs
@program bloom_pre post_vs bloom_pre_fs
@program bloom_down post_vs bloom_down_fs
@program bloom_up post_vs bloom_up_fs
@program clarity_down post_vs clarity_down_fs
@program clarity_blur post_vs clarity_blur_fs
@program scene_comp post_vs scene_comp_fs
@program dof_down post_vs dof_down_fs
@program dof_blur post_vs dof_blur_fs
@program dof_comp post_vs dof_comp_fs
@program stylize post_vs stylize_fs

@sampler_type smaa_pt nonfiltering
@image_sample_type dof_depth unfilterable_float
@sampler_type dof_pt nonfiltering
