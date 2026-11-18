// Retired 2026-11-10 (kenshi-k16.2.82): the AgX and Khronos PBR Neutral display curves and the sRGB output encode, with the tone selector that chose
// them. The original's display is the client's one base now: clamp to 0..1, no curve, no encode (shaders/tone.inc tone_map). Kept for reference only;
// not built (the shader globs only client/shaders/*.glsl).

// tone: 1 AgX, 2 Neutral, 3 clip. Input linear HDR; output display values.
vec3 tone_map(vec3 c, float tone)
{
	if (tone > 2.5) { return clamp(c, 0.0, 1.0); }
	c = tone > 1.5 ? tone_neutral(c) : tone_agx(c);
	return srgb_encode(c);
}

// AgX (Troy Sobotka, MIT) and Khronos PBR Neutral (Khronos Group, Apache-2.0) are our transcriptions of the published curves.
vec3 tone_agx(vec3 c)
{
	const mat3 inM = mat3(
		0.842479062253094, 0.0423282422610123, 0.0423756549057051,
		0.0784335999999992, 0.878468636469772, 0.0784336,
		0.0792237451477643, 0.0791661274605434, 0.879142973793104);
	const mat3 outM = mat3(
		1.19687900512017, -0.0528968517574562, -0.0529716355144438,
		-0.0980208811401368, 1.15190312990417, -0.0980434501171241,
		-0.0990297440797205, -0.0989611768448433, 1.15107367264116);
	c = inM * max(c, 0.0);
	c = clamp(log2(max(c, 1e-10)), -12.47393, 4.026069);
	c = (c + 12.47393) / 16.5;
	vec3 x2 = c * c, x4 = x2 * x2;
	c = 15.5 * x4 * x2 - 40.14 * x4 * c + 31.96 * x4 - 6.868 * x2 * c + 0.4298 * x2 + 0.1191 * c - 0.00232;
	// The sigmoid approximates AgX's gamma-encoded output; linearize before tone_map's sRGB OETF (IOLITE minimal AgX /
	// three.js AgXToneMapping both end with pow(2.2)). Without this 0.18 maps to 0.73 sRGB instead of ~0.5.
	c = pow(clamp(outM * c, 0.0, 1.0), vec3(2.2));
	return c;
}

vec3 tone_neutral(vec3 c)
{
	const float startCompression = 0.8 - 0.04;
	const float desaturation = 0.15;
	float x = min(c.r, min(c.g, c.b));
	float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
	c -= offset;
	float peak = max(c.r, max(c.g, c.b));
	if (peak < startCompression) { return clamp(c, 0.0, 1.0); }
	const float d = 1.0 - startCompression;
	float newPeak = 1.0 - d * d / (peak + d - startCompression);
	c *= newPeak / peak;
	float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
	return clamp(mix(c, vec3(newPeak), g), 0.0, 1.0);
}

vec3 srgb_encode(vec3 c)
{
	c = clamp(c, 0.0, 1.0);
	return mix(12.92 * c, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, c));
}
