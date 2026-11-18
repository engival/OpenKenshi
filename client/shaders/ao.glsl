// Screen-space ambient occlusion (ao.hpp, post.ao; ours, the game has none), from the scene depth after the scene pass, at half resolution. The world shaders
// read it in the next frame (lighting.inc light_ao) and darken only the ambient light with it. Indirect light (post.indirect, ours) rides along: the same taps
// read this frame's scene colour, and the occluded share of the sky (1 - AO) is refilled with the occluders' mean radiance, one bounce per frame (the next frame's
// scene already holds it: the bounces accumulate, damped by the albedo). Taps in the sky carry no weight; a tap's colour is capped at display white (lamps, glints).
//   ao_sample : AO at each half-res pixel, from the full-res depth texel at its top-left: normal from the depth neighbours (the side with the smaller step, so
//               silhouettes keep their own surface), then 12 taps on a spiral turned per pixel by interleaved gradient noise, the scalable AO estimator
//               (McGuire, Mara, Luebke, "Scalable Ambient Obscurance", HPG 2012; our own code). Out 0: r = AO, g = view depth;
//               out 1: rgb = bounce in irradiance units (the colour over the exposure the world shaders baked in, so it does not follow the exposure adapting).
//   ao_blur   : separable 9-tap blur that skips texels whose depth differs (the noise goes, edges and dithered fades stay apart), both outputs. Twice: x, then y.
@include origin.inc

@vs ao_vs
out vec2 uv;	// the logical uv (v up, origin.inc); a read of a render target goes through offscreen_uv

void main()
{
	vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
	uv = p;
	gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
@end

@fs ao_sample_fs
@include_block origin
in vec2 uv;
layout(location=0) out vec4 frag_color;
layout(location=1) out vec4 frag_il;

layout(binding=0) uniform texture2D depth_tex;
layout(binding=1) uniform texture2D col_tex;
layout(binding=0) uniform sampler depth_smp;
layout(binding=0) uniform ao_sample_params
{
	vec4 zinfo;	// x = near, y = far, z = 1 when the depth is already 0..1 clip (Vulkan), w = AO radius (world units)
	vec4 proj;	// x = tanY x aspect, y = tanY, z = full-res pixels per world unit at view depth 1, w = range: the AO fades out towards it
	vec4 size;	// xy = full-res size, zw = 1 / that
	vec4 il;	// x = 1 / (0.96 x the baked exposure): scene colour -> irradiance units, y = 1: indirect on
};

float view_depth(vec2 tuv)
{
	float d = textureLod(sampler2D(depth_tex, depth_smp), offscreen_uv(tuv), 0.0).r;
	float zn = zinfo.x, zf = zinfo.y;
	return zinfo.z > 0.5 ? zn * zf / (zf - d * (zf - zn)) : 2.0 * zn * zf / ((zf + zn) - (d * 2.0 - 1.0) * (zf - zn));
}

vec3 view_pos(vec2 tuv)
{
	float z = view_depth(tuv);
	return vec3((tuv * 2.0 - 1.0) * proj.xy * z, z);
}

void main()
{
	vec2 c = (floor(uv * size.xy - 0.5) + 0.5) * size.zw;
	vec3 p = view_pos(c);
	if (p.z >= proj.w)
	{
		frag_color = vec4(1.0, min(p.z, 60000.0), 0.0, 0.0);
		frag_il = vec4(0.0);
		return;
	}
	vec3 px = view_pos(c + vec2(size.z, 0.0)) - p, nx = p - view_pos(c - vec2(size.z, 0.0));
	vec3 py = view_pos(c + vec2(0.0, size.w)) - p, ny = p - view_pos(c - vec2(0.0, size.w));
	vec3 n = normalize(cross(abs(px.z) < abs(nx.z) ? px : nx, abs(py.z) < abs(ny.z) ? py : ny));
	if (dot(n, p) > 0.0) { n = -n; }
	float r = zinfo.w;
	float rpx = min(r * proj.z / p.z, 120.0);
	float ao = 1.0;
	vec3 bounce = vec3(0.0);
	if (rpx > 1.0)
	{
		const int kTaps = 12;
		float phi = 6.2831853 * fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
		float bias = 0.3 + 0.002 * p.z;
		float sum = 0.0;
		vec3 csum = vec3(0.0);
		for (int i = 0; i < kTaps; i++)
		{
			float a = (float(i) + 0.5) / float(kTaps);
			float ang = a * 7.0 * 6.2831853 + phi;
			vec2 tuv = c + vec2(cos(ang), sin(ang)) * (a * rpx) * size.zw;
			vec3 v = view_pos(tuv) - p;
			float vv = dot(v, v);
			float f = max(r * r - vv, 0.0);
			float w = f * f * f * max((dot(v, n) - bias) / (vv + 0.01), 0.0);
			sum += w;
			if (il.y > 0.5 && w > 0.0)
			{
				vec3 tc = textureLod(sampler2D(col_tex, depth_smp), offscreen_uv(tuv), 0.0).rgb;
				csum += w * tc * min(1.0, 1.0 / max(dot(tc, vec3(0.2126, 0.7152, 0.0722)), 1e-4));	// capped at display white
			}
		}
		ao = max(0.0, 1.0 - sum * (5.0 / (pow(r, 6.0) * float(kTaps))));
		if (sum > 0.0) { bounce = (1.0 - ao) * csum / sum * il.x; }
	}
	float fade = smoothstep(0.6 * proj.w, proj.w, p.z);
	frag_color = vec4(mix(ao, 1.0, fade), p.z, 0.0, 0.0);
	frag_il = vec4(bounce * (1.0 - fade), 0.0);
}
@end

@fs ao_blur_fs
@include_block origin
in vec2 uv;
layout(location=0) out vec4 frag_color;
layout(location=1) out vec4 frag_il;

layout(binding=0) uniform texture2D src_tex;
layout(binding=1) uniform texture2D il_tex;
layout(binding=0) uniform sampler src_smp;
layout(binding=0) uniform ao_blur_params
{
	vec4 step;	// xy = one texel along the blur axis (uv)
};

void main()
{
	vec2 ruv = offscreen_uv(uv);	// the blur axis (step) is logical too: the taps are placed in uv and flipped together
	vec2 c = textureLod(sampler2D(src_tex, src_smp), ruv, 0.0).rg;
	float sum = c.x, wsum = 1.0;
	vec3 isum = textureLod(sampler2D(il_tex, src_smp), ruv, 0.0).rgb;
	for (int i = -4; i <= 4; i++)
	{
		if (i == 0) { continue; }
		vec2 o = offscreen_uv(uv + step.xy * float(i));
		vec2 t = textureLod(sampler2D(src_tex, src_smp), o, 0.0).rg;
		float w = exp(-float(i * i) / 12.0) * max(0.0, 1.0 - abs(t.y - c.y) / (0.02 * c.y));
		sum += w * t.x;
		isum += w * textureLod(sampler2D(il_tex, src_smp), o, 0.0).rgb;
		wsum += w;
	}
	frag_color = vec4(sum / wsum, c.y, 0.0, 0.0);
	frag_il = vec4(isum / wsum, 0.0);
}
@end

@program ao_sample ao_vs ao_sample_fs
@program ao_blur ao_vs ao_blur_fs
@image_sample_type depth_tex unfilterable_float
@sampler_type depth_smp nonfiltering
@sampler_type src_smp nonfiltering
