#pragma once
#include <string>
#include <vector>

#include "sokol_gfx.h"
#include "sky.hpp"
#include "camera.hpp"

// The static fog volumes of newland/land/fogfeatures.dat (docs/formats/TERRAIN_FORMAT.md, docs/CLIENT_GOALS.md "Fog volumes"): spheres, beams (cylinders) and 7-plane
// prisms (each clipped to the screen box of its bounding sphere), drawn the way materials/post/fog.hlsl does (analytic thickness along each pixel's ray against the scene depth, alpha blended over the finished,
// atmosphere-fogged scene). One full-screen triangle per visible volume, far to near; the scene depth is sampled, so this is its own pass after the scene pass
// (hdr.hpp fogPass). Volume colour = record rgb x sunColour.w (planes: max(0.08, sunColour.w x brighten)) x baked exposure; the density is 1 / `distance`.
class FogVolumes
{
public:
	struct Volume
	{
		std::string name;
		int type = 0;			// 2 sphere, 3 beam, 4 planes (the file's type byte)
		float rgb[3] = { 1, 1, 1 };	// colour
		float alpha = 1;
		float distance = 1;		// header float: thickness at which the volume is opaque (density = 1 / distance)
		float p1[3] = {}, p2[3] = {};	// sphere centre (p1) / beam base and top
		float radius = 0;		// sphere or beam radius from the file (the game's node scale is 2 x this; the shader radius 0.48 / 0.4 of the scale)
		float blur = 0;			// beam end / plane edge fade length (the file's last float, the game stores 1 / it)
		float planes[7][4] = {};	// nx ny nz d: inside is n.p <= -d
		double bound[4] = {};		// bounding sphere (centre, radius) for the visibility test
	};

	// Reads fogfeatures.dat under `dataDir`; false (message on stderr) when missing or malformed.
	bool load(const std::string& dataDir);
	void init();
	void shutdown();

	// Draws the visible volumes inside the open fog pass; returns how many. `depth` = the scene depth as a texture. sunY = unclamped sun height (planes' brighten).
	int render(const Camera& c, const LightParams& light, float sunY, sg_view depth);

	const std::vector<Volume>& volumes() const { return vols_; }
	bool enabled = true;
	int drawn() const { return drawn_; }

private:
	std::vector<Volume> vols_;
	sg_shader shd_ = {};
	sg_pipeline pip_ = {};
	sg_sampler smp_ = {};
	int drawn_ = 0;
};
