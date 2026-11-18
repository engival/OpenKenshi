// Retired 2026-11-11 (kenshi-k16.2.83): the water shader's hand-tuned light model and the C++ gain that went with it.
// `light_shade_water` was water.glsl's model until 2026-10-31, when water moved to the game's light_shade + light_spec
// (forward/water.hlsl: colour = (sunDiffuse + envDiffuse) x albedo + sunSpec + envSpec x rf); it had no caller since.
// `kWorldLightScale` (src/sky.cpp) was 1.6, the same gain this function hard-codes in `fog_col.w * 1.6`; it was written
// to the light block's fog_rng.z (L.v[5][2]) and read by no shader.
// Kept for reference only; not built (the shader build globs client/shaders/*.glsl only).

// The retired C++ constant (was src/sky.cpp):
// constexpr double kWorldLightScale = 1.6;	// WATER shader only (light_shade_water, hand-tuned): display = pow(light x exposure x this, 1/2.2)

// water only: hand-tuned gains and the old display-referred model (not calibrated against the game)
vec3 light_shade_water(vec3 albedo, vec3 n, float amb_gain, float sun_gain)
{
	float diff = max(dot(n, sun_dir.xyz), 0.0);
	vec3 amb = nadir.rgb + zenith.rgb * (0.5 + 0.5 * n.y);
	vec3 light = (amb * amb_gain + sun_col.rgb * (sun_gain * diff)) * (fog_col.w * 1.6);
	return albedo * pow(light, vec3(1.0 / 2.2));
}
