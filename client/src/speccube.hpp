#pragma once
#include <string>

#include "sokol_gfx.h"

// The game's environment specularity cube (data/materials/mp_specularity.dds, 256^2 BC3 x 6 faces x 9 mips; rgb x a x 10 is the reflected sky radiance,
// lightingFunctions.hlsl CalcEnvironmentLight), read from the install at startup. A black 4x4 cube stands in when the file is missing.
struct SpecCubeBinding
{
	sg_view view = {};
	sg_sampler smp = {};
};
const SpecCubeBinding& spec_cube_binding();
// The water's own cube (forward/water.material specularityCube = mp_specuwatery.dds, 512^2 BC3 x 6, one level): far dimmer than mp_specularity,
// darkest toward the horizon. Loaded by spec_cube_init with the main cube.
const SpecCubeBinding& water_cube_binding();
// The game's static irradiance cube (data/materials/mp_irradiance.dds, 16^2 BC3 x 6 faces x 5 mips, the deferred material's `cubic_texture mp_irradiance.dds`):
// the ambient light of every world shader, sampled at mip 3 by the normal as texCUBElod(N, 3) x a x 4 (lighting.inc light_irradiance; kenshi-inspect rwh.141).
// Its view is bound at VIEW_irr_cube and filtered with spec_cube_binding().smp: a black stand-in (no ambient) when the file is missing or malformed.
const SpecCubeBinding& irr_cube_binding();

bool spec_cube_init(const std::string& dataDir);	// false when mp_specularity.dds is missing or malformed (its stand-in is used); the other cubes warn on their own
void spec_cube_shutdown();
