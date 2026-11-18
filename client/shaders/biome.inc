// Biome layer mixing (terrainfp4.hlsl computeBiome), shared by terrain.glsl (terrain chunks) and mesh.glsl (TERRAIN-mode map features, mapfeature_fs).
// The including fragment shader declares, before `@include_block biome`: texture2D biome_table + sampler tsmp, texture2DArray diffuse_maps + normal_maps, sampler asmp.
@block biome
vec4 tab(int row, int col) { return texelFetch(sampler2D(biome_table, tsmp), ivec2(col, row), 0); }

// smoothstep that also accepts edge0 > edge1 (HLSL behaviour the game relies on for the slope max edge)
float ss(float e0, float e1, float x)
{
	float t = clamp((x - e0) / (abs(e1 - e0) < 1e-6 ? 1e-6 : e1 - e0), 0.0, 1.0);
	return t * t * (3.0 - 2.0 * t);
}

// The layer fetches below sit in branches that skip layers whose mix weight is zero, so they need explicit gradients (implicit derivatives are undefined in
// non-uniform control flow). g = (dFdx, dFdy) of the uv, taken once in uniform flow.
// rgb = albedo, a = gloss (the game writes biome.albedo.a as the gbuffer gloss, terrainfp4.hlsl:274)
vec4 dif(float layer, vec2 uv, vec4 g) { return textureGrad(sampler2DArray(diffuse_maps, asmp), vec3(uv, layer), g.xy, g.zw); }
// the colour map multiplies the layer: rgb by colour, gloss by colour.a (terrainfp4.hlsl computeBiome, `colour` is a float4)
vec4 tint(vec4 d, vec4 c) { return vec4(d.rgb * c.rgb, d.a * c.a); }
vec4 tint_by(vec4 d, vec4 c, float m) { return tint(d, mix(vec4(1.0), c, m)); }
vec3 nml(float layer, vec2 uv, vec4 g) { return textureGrad(sampler2DArray(normal_maps, asmp), vec3(uv, layer), g.xy, g.zw).rgb; }
vec4 grad2(vec2 uv) { return vec4(dFdx(uv), dFdy(uv)); }
vec4 scaled(vec4 g, vec2 s) { return vec4(g.xy * s, g.zw * s); }

const float kMinW = 0.002;	// a layer weighing less than this is skipped (below one 8-bit step of the weight maps)

// One biome's six layers mixed as terrainfp4.hlsl computeBiome does: out_n is the tangent-space normal in 0..1 encoding; the result is (albedo rgb, gloss); out_abs is the layers' wet absorbance mixed with the same weights (terrainfp4.hlsl:158-163: base, grass, slope, dirt, road, cliff; no distance fade).
vec4 compute_biome(int row, vec3 n, vec2 cliff_blend, vec4 map, vec4 colour, float dist, vec3 ground_rgb, vec3 wpos, out vec3 out_n, out float out_abs)
{
	vec4 sB = tab(row, 0), sA = tab(row, 1), sC = tab(row, 2);
	vec4 smin = tab(row, 3), smax = tab(row, 4), sblend = tab(row, 5), omult = tab(row, 6), fadep = tab(row, 7), misc = tab(row, 8);
	vec4 d0 = tab(row, 9), d1 = tab(row, 10), n0 = tab(row, 11), n1 = tab(row, 12), ab0 = tab(row, 13), ab1 = tab(row, 14);
	vec2 uv = wpos.xz / 5000.0;
	// cliff vertical coordinate with the biome's cos-wave distortion
	float uvv = 1.0 - wpos.y / 5000.0 + (cos(wpos.x * misc.y) + cos(wpos.z * misc.y)) * misc.z;
	float slope = min(1.0, 1.0 - n.y);	// mapfeature_fs clamps it: map features have overhangs (n.y < 0)
	float w_slope = ss(smin.x - sblend.x, smin.x, slope) * ss(smax.x + sblend.x, smax.x, slope);
	float w_cliff = ss(smin.y - sblend.y, smin.y, slope) * ss(smax.y + sblend.y, smax.y, slope);
	vec2 uyz = vec2(uv.y, uvv) * sA.zw, uxz = vec2(uv.x, uvv) * sA.zw;
	vec4 gu = grad2(uv), gyz = grad2(uyz), gxz = grad2(uxz);
	float fade = clamp(dist * fadep.w - 0.3, 0.0, 1.0);
	bool near = fade < 1.0;		// at fade 1 the base / grass / slope / dirt layers are all replaced by the ground colour
	bool base = near && map.r < 1.0 - kMinW, grass = near && map.r > kMinW, dirt = near && map.b > kMinW, slp = near && w_slope > kMinW;
	bool road = map.a > kMinW, cliff = w_cliff > kMinW;
	vec4 a = vec4(0.0);
	vec3 nn = vec3(0.0);
	if (base)
	{
		a = tint(dif(d0.x, uv * sB.xy, scaled(gu, sB.xy)), colour);
		nn = nml(n0.x, uv * sB.xy, scaled(gu, sB.xy));
	}
	if (grass)
	{
		a = mix(a, tint_by(dif(d0.w, uv * sB.zw, scaled(gu, sB.zw)), colour, omult.y), map.r);
		nn = mix(nn, nml(n0.w, uv * sB.zw, scaled(gu, sB.zw)), map.r);
	}
	if (slp)
	{
		a = mix(a, tint(dif(d0.y, uv * sA.xy, scaled(gu, sA.xy)), colour), w_slope);
		nn = mix(nn, nml(n0.y, uv * sA.xy, scaled(gu, sA.xy)), w_slope);
	}
	if (dirt)
	{
		a = mix(a, tint_by(dif(d1.x, uv * sC.xy, scaled(gu, sC.xy)), colour, omult.z), map.b);
		nn = mix(nn, nml(n1.x, uv * sC.xy, scaled(gu, sC.xy)), map.b);
	}
	a.rgb = mix(a.rgb, ground_rgb, fade);	// rgb only: the game's distance fade leaves the gloss alone
	nn = mix(nn, vec3(0.5, 0.5, 1.0), fade);
	if (road)
	{
		a.rgb = mix(a.rgb, tint_by(dif(d1.y, uv * sC.zw, scaled(gu, sC.zw)), colour, omult.w).rgb, map.a);	// roads: rgb only
		nn = mix(nn, nml(n1.y, uv * sC.zw, scaled(gu, sC.zw)), map.a);
	}
	out_n = nn;
	out_abs = mix(mix(mix(mix(mix(ab0.x, ab0.w, map.r), ab0.y, w_slope), ab1.x, map.b), ab1.y, map.a), ab0.z, w_cliff);
	if (cliff)
	{
		vec4 c_cliff = tint_by(dif(d0.z, uyz, gyz) * cliff_blend.x + dif(d0.z, uxz, gxz) * cliff_blend.y, colour, omult.x);
		// the two planar projections' normals are mirrored into the face's frame (computeBiome): red inverted, green inverted on the -x / +z facing sides
		vec3 nx = nml(n0.z, uyz, gyz), nz = nml(n0.z, uxz, gxz);
		nx.rg = vec2(1.0 - nx.r, n.x > 0.0 ? nx.g : 1.0 - nx.g);
		nz.rg = vec2(1.0 - nz.r, n.z < 0.0 ? nz.g : 1.0 - nz.g);
		vec3 n_cliff = nx * cliff_blend.x + nz * cliff_blend.y;
		a = mix(a, c_cliff, w_cliff);
		out_n = mix(nn, n_cliff, w_cliff);
	}
	return vec4(a.rgb * misc.x, a.a);
}
@end
