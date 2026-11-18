// Retired 2026-11-15 (kenshi-k16.2.256.4): the six-colour ambient cube that lighting.inc light_irradiance used before the client sampled the game's static
// irradiance cube exactly. Kept as the reference for the approximation's numbers; not built (the shader globs only client/shaders/*.glsl) and no fallback path
// reads it: the compiled shaders sample irr_cube (mp_irradiance.dds, src/speccube.cpp) at mip 3 unconditionally.
//
// `light_irradiance_cube_faces` blended each cube face mean (mip 0, already x alpha x 4, so in the shader's units) by the squared normal, an ambient cube. It
// matched the live cube within ~1 % red/green at every normal, but at the Black Desert City dome's normal (+Z) it gave 1.32 1.41 1.74 against the exe's exact
// texCUBElod(N, 3) sample 1.31 1.41 1.66: blue 5 % high, the whole of the dome's residual blue column (findings kenshi-k16.2.256.2 README; rwh.141 §1).

const vec3 kIrrX[2] = vec3[2](vec3(1.069, 1.210, 1.493), vec3(1.006, 1.151, 1.451));	// +X, -X
const vec3 kIrrY[2] = vec3[2](vec3(1.091, 1.204, 1.504), vec3(1.110, 1.044, 0.953));	// +Y (up), -Y (down)
const vec3 kIrrZ[2] = vec3[2](vec3(1.327, 1.449, 1.792), vec3(1.194, 1.394, 1.785));	// +Z, -Z
vec3 light_irradiance_cube_faces(vec3 n)
{
	vec3 w = n * n;
	return w.x * kIrrX[n.x < 0.0 ? 1 : 0] + w.y * kIrrY[n.y < 0.0 ? 1 : 0] + w.z * kIrrZ[n.z < 0.0 ? 1 : 0];
}
