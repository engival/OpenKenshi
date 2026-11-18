#pragma once
// The character creator's scene look (kenshi-k16.2.141; kinspect findings/rwh121_creator_exposure_live.md). The original's editor workspace is
// GBuffer_Minimal -> Character_Editor -> GUI: no sun, ambient map, fog, sky, terrain, AO, bloom or exposure. Character_Editor draws a full-screen quad
// of SandyGround.dds, then Deferred_RTT_Lighting (rtt.hlsl) over the character and platform with the fixed lightDirection / lightColour / exposure of
// the CharacterEditorLighting material. Ours: the same backdrop, the same values read from the game's own scripts (charedit.compositor, deferred.material),
// the lighting through the world's shared light block (the deferred formula, lighting.inc), the display gain pinned to 1 (main.cpp).
#include <string>

#include "sokol_gfx.h"

struct LightParams;

class CreatorScene
{
public:
	// Reads dataDir/materials/compositors/charedit.compositor (lightDirection, lightColour), dataDir/materials/deferred/deferred.material (the Deferred_RTT
	// exposure default) and loads dataDir/gui/gfx/SandyGround.dds. False with err set when one is missing: nothing is substituted.
	bool load(const std::string& dataDir, std::string& err);
	bool ready() const { return pip_.id != SG_INVALID_ID; }
	void shutdown();
	// The backdrop, first in the scene pass (colour RGBA16F + depth, depth untouched).
	void drawBackdrop();
	// Turns the world's light block into the creator's: the fixed light, the cube ambient, the pass' exposure, no fog.
	void light(LightParams& lp) const;

private:
	float dir_[3] = {}, colour_[3] = {}, exposure_ = 0;
	sg_image img_ = {};
	sg_view view_ = {};
	sg_sampler smp_ = {};
	sg_shader shd_ = {};
	sg_pipeline pip_ = {};
};
