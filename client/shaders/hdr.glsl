// HDR auto exposure (docs/reference/LIGHTING.md section 10): the game's Resolve_HDR chain (compositors/main.compositor, post/hdrfp4.hlsl) on the GPU.
//   hdr_lum   : scene (RGBA16F) -> 128^2 R16F, Downsample2x2Luminance_fp4 exactly: 4 nearest taps at +-0.5 scene texel around the destination texel centre (sparse: ~4 of every 10x5.6 pixels), luminance max(dot(rgb, (.299,.587,.114)), 1e-4)
//   hdr_down  : Downsample3x3_fp4: 9 NEAREST taps at -1..+1 source texel (texelSize = inverse size of the source, clamp), 128 -> 64 -> 16 -> 4 -> 1 (not a flat mean)
//   hdr_adapt : 1x1 R32F, adapted = last + (cur - last) x (1 - exp(-dt x rate)), then clamp(MIN, MAX) (the scene's domain: AdaptLuminance_fp4)
//   hdr_comp  : out = scene x max(key / adapted, 0.001), then the display transform (tone_map) for the direct path; the post chain gets the exposed display-referred values
@include tone.inc
@include origin.inc

@vs hdr_vs
out vec2 uv;	// the logical uv (v up, origin.inc); a read of a render target goes through offscreen_uv

void main()
{
	vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
	uv = p;
	gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
@end

// The scene depth copied into an R32F colour target (Hdr::copyOpaqueDepth): the same raw 0..1 depth values, one texel per pixel.
@fs hdr_opaque_fs
@include_block origin
layout(binding=0) uniform texture2D opq_src;
layout(binding=0) uniform sampler osmp;
in vec2 uv;
out vec4 frag_color;

void main()
{
	ivec2 sz = textureSize(sampler2D(opq_src, osmp), 0);
	frag_color = vec4(texelFetch(sampler2D(opq_src, osmp), min(ivec2(offscreen_uv(uv) * vec2(sz)), sz - 1), 0).r, 0.0, 0.0, 1.0);
}
@end

// The game samples the luminance passes POINT + CLAMP (the .material's `filtering none` / `tex_address_mode clamp`; the HLSL's g_samLinear does not apply: kenshi-rwh.123,
// bit-exact against the live rt_lum1). A nearest tap at a texel boundary rounds to the LARGER index (D3D11), so each tap is worked out here as an explicit texel,
// floor(uv x size), in the game's top-down rows (row 0 = top of the screen), then turned into this backend's storage row (offscreen_texel).
@fs hdr_lum_fs
@include_block origin
layout(binding=0) uniform texture2D scene_tex;
layout(binding=0) uniform sampler smp;
layout(binding=0) uniform hdr_lum_params
{
	vec4 p;		// x = 1 / exposure baked into the scene by the world shaders, yz = scene texel size in uv
};
in vec2 uv;
out vec4 frag_color;

void main()
{
	ivec2 sz = textureSize(sampler2D(scene_tex, smp), 0);
	vec2 dst = vec2(floor(uv.x * 128.0), 127.0 - floor(uv.y * 128.0));	// this texel, top-down rows
	vec2 c0 = (dst + 0.5) * vec2(sz) / 128.0;	// its centre in scene texels (the tap offsets are +-0.5 scene texel)
	float sum = 0.0;
	for (int k = 0; k < 4; k++)
	{
		vec2 t = floor(c0 + vec2(float(k >> 1) - 0.5, float(k & 1) - 0.5));
		t = clamp(t, vec2(0.0), vec2(sz) - 1.0);
		vec3 c = texelFetch(sampler2D(scene_tex, smp), ivec2(offscreen_texel(vec2(t.x, float(sz.y) - 1.0 - t.y), float(sz.y))), 0).rgb;
		sum += max(dot(c, vec3(0.299, 0.587, 0.114)) * p.x, 1e-4);
	}
	frag_color = vec4(min(sum * 0.25, 60000.0), 0.0, 0.0, 1.0);
}
@end

@fs hdr_down_fs
@include_block origin
layout(binding=0) uniform texture2D src_tex;
layout(binding=0) uniform sampler smp;
layout(binding=0) uniform hdr_down_params
{
	vec4 p;		// x = 1 / source size, z = destination size
};
in vec2 uv;
out vec4 frag_color;

void main()
{
	float s = floor(1.0 / p.x + 0.5);	// the source size (square): 128, 64, 16, 4
	float ds = p.z;	// the destination size: 64, 16, 4, 1
	vec2 dst = vec2(floor(uv.x * ds), ds - 1.0 - floor(uv.y * ds));	// this texel, top-down rows
	vec2 c0 = (dst + 0.5) * (s / ds);	// its centre in source texels
	float sum = 0.0;
	for (int k = 0; k < 9; k++)
	{
		vec2 t = clamp(floor(c0 + vec2(float(k % 3 - 1), float(k / 3 - 1))), vec2(0.0), vec2(s - 1.0));
		sum += texelFetch(sampler2D(src_tex, smp), ivec2(offscreen_texel(vec2(t.x, s - 1.0 - t.y), s)), 0).r;
	}
	frag_color = vec4(sum * (1.0 / 9.0), 0.0, 0.0, 1.0);
}
@end

@fs hdr_adapt_fs
layout(binding=0) uniform texture2D cur_tex;
layout(binding=1) uniform texture2D last_tex;
layout(binding=1) uniform sampler psmp;
layout(binding=0) uniform hdr_adapt_params
{
	vec4 p;		// x = frame time x adaptation rate, y = MIN_LUMINANCE, z = MAX_LUMINANCE, w = 1: jump to the current value (first frame, scripted runs)
	vec4 q;		// post.eye_adapt: x = the original's MIN_LUMINANCE, y = compensation exponent (1 = off: the plain chain), z = knee
};
in vec2 uv;
out vec4 frag_color;

void main()
{
	float cur = textureLod(sampler2D(cur_tex, psmp), vec2(0.5), 0.0).r;
	// post.eye_adapt: down to the knee (z) the original's clamp holds (a daylight exterior is unchanged); below it a dark scene is compensated only partly
	// (exponent y < 1), continuing from MIN_LUMINANCE, so a dark room still reads darker than outdoors
	if (q.y != 1.0) { cur = cur >= q.z ? max(cur, q.x) : q.x * pow(max(cur, 1e-6) / q.z, q.y); }
	float last = textureLod(sampler2D(last_tex, psmp), vec2(0.5), 0.0).r;
	float a = p.w > 0.5 ? cur : last + (cur - last) * (1.0 - exp(-p.x));
	frag_color = vec4(clamp(a, p.y, p.z), 0.0, 0.0, 1.0);
}
@end

@fs hdr_comp_fs
@include_block tone
@include_block origin
layout(binding=0) uniform texture2D scene_tex;
layout(binding=1) uniform texture2D adapted_tex;
layout(binding=2) uniform texture2D depth_tex;
layout(binding=3) uniform texture2D flow_tex;
layout(binding=4) uniform texture2D pert_tex;
layout(binding=0) uniform sampler smp;
layout(binding=1) uniform sampler psmp;
layout(binding=2) uniform sampler dsmp;
layout(binding=3) uniform sampler wsmp;
layout(binding=0) uniform hdr_comp_params
{
	vec4 p;		// x = EXPOSURE_KEY, y = 1 / exposure baked into the scene (1 now), z = fixed exposure (> 0: --exposure N, ignores the adapted luminance), w = heat haze (0 = off)
	vec4 zinfo;	// x = near, y = far (projection), z = 1 when the depth is already 0..1 clip (Vulkan), w = the game's farClip (the haze depth scale)
	vec4 cam;	// x = tan(fovY / 2), y = aspect, z = gameTime
	vec4 post;	// x = unused (the retired post.tone switch), y = 1: apply the original's display (direct path); 0: the exposed display-referred values (the post chain),
			// z = 1: debug.nonfinite (mark non-finite pixels)
};
in vec2 uv;
out vec4 frag_color;

// heathaze.hlsl sampleNormal: one flowing perturbation layer, faded in and out over its period
vec3 haze_layer(vec2 coord, vec2 dir, float t)
{
	t = fract(t);
	vec3 n = textureLod(sampler2D(pert_tex, wsmp), coord + dir * t, 0.0).rgb * 2.0 - 1.0;
	return n.xzy * (1.0 - abs(t * 2.0 - 1.0));
}

// The game's HeatHaze compositor node (post/heathaze.hlsl, listed in compositors/compositors.cfg): the screen uv wobbles by a flowing normal, scaled by the
// pixel's distance / farClip (full from farClip / 6, and on the sky) and the weather's heat haze; two taps blended. Ogre's uv has v = 0 at the top, ours at the bottom.
vec2 haze_offset()
{
	vec2 tuv = vec2(uv.x, 1.0 - uv.y);
	vec2 dir = textureLod(sampler2D(flow_tex, wsmp), tuv * 3.341, 0.0).rg * 2.0 - 1.0;
	vec2 tex = tuv * 7.341;
	float t = cam.z * 100.0;
	vec3 n = vec3(0.0, 1.0, 0.0) + haze_layer(tex, dir, t) + haze_layer(tex + vec2(0.1, 0.3), dir, t + 0.33) + haze_layer(tex + vec2(0.4, 0.7), dir, t + 0.66);
	n.y = 0.0;
	n = normalize(n);
	float d = textureLod(sampler2D(depth_tex, dsmp), offscreen_uv(uv), 0.0).r;
	float depth = 1.0;
	if (d < 0.99999)
	{
		float zn = zinfo.x, zf = zinfo.y;
		float zv = zinfo.z > 0.5 ? zn * zf / (zf - d * (zf - zn)) : 2.0 * zn * zf / ((zf + zn) - (d * 2.0 - 1.0) * (zf - zn));
		vec2 ndc = uv * 2.0 - 1.0;
		depth = zv * length(vec3(ndc.x * cam.x * cam.y, ndc.y * cam.x, 1.0)) / zinfo.w;
	}
	vec2 o = n.xz * (clamp(depth * 6.0, 0.0, 1.0) * 0.002 * p.w);
	return vec2(o.x, -o.y);
}

void main()
{
	float adapted = textureLod(sampler2D(adapted_tex, psmp), vec2(0.5), 0.0).r;
	float e = p.z > 0.0 ? p.z : max(p.x / adapted, 0.001);
	vec3 c;
	if (p.w > 0.0)
	{
		vec2 o = haze_offset();
		c = mix(textureLod(sampler2D(scene_tex, smp), offscreen_uv(uv + o), 0.0).rgb, textureLod(sampler2D(scene_tex, smp), offscreen_uv(uv + o * 0.7), 0.0).rgb, 0.5);
	}
	else { c = textureLod(sampler2D(scene_tex, smp), offscreen_uv(uv), 0.0).rgb; }
	c *= e * p.y;
	if (post.y > 0.5) { c = tone_map(c); }
	// debug.nonfinite: a 7 x 7 px square round each NaN or Inf in the scene target magenta, round one made here (the haze offset, the exposure) cyan.
	// Mid values, not bright ones: the tone map turns a bright mark white, and the post chain smears a single pixel.
	if (post.z > 0.5)
	{
		bool bad = false;
		for (int y = -3; y <= 3; y++)
		{
			for (int x = -3; x <= 3; x++)
			{
				vec3 s = texelFetch(sampler2D(scene_tex, smp), ivec2(gl_FragCoord.xy) + ivec2(x, y), 0).rgb;
				bad = bad || any(isnan(s)) || any(isinf(s));
			}
		}
		if (bad) { c = vec3(0.5, 0.0, 0.5); }
		else if (any(isnan(c)) || any(isinf(c))) { c = vec3(0.0, 0.5, 0.5); }
	}
	frag_color = vec4(c, 1.0);
}
@end

@program hdr_lum hdr_vs hdr_lum_fs
@program hdr_down hdr_vs hdr_down_fs
@program hdr_adapt hdr_vs hdr_adapt_fs
@program hdr_comp hdr_vs hdr_comp_fs
@program hdr_opaque hdr_vs hdr_opaque_fs

@image_sample_type cur_tex unfilterable_float
@image_sample_type last_tex unfilterable_float
@sampler_type psmp nonfiltering
@image_sample_type adapted_tex unfilterable_float
@image_sample_type depth_tex unfilterable_float
@sampler_type dsmp nonfiltering
@image_sample_type opq_src unfilterable_float
@sampler_type osmp nonfiltering
