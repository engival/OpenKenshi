#pragma once
#include "camera.hpp"
#include "shadow.hpp"
#include "sky.hpp"
#include "sokol_gfx.h"

// Dust motes (render.dust_motes, kenshi-jj3.8.1; shaders/dust.glsl): small drifting specks in the air of the nearest baked building (lightvol.hpp) that show only inside a sunbeam
// (the sun's shadow lookup of the sun shafts, no ambient: out of a beam a mote is black on the additive blend). One instanced draw over the HDR scene, in the pass of the sun shafts
// (Hdr::fogPass) while lightvol::Volumes are live; the scene depth is sampled per mote.
class Dust
{
public:
	void init();
	void shutdown();
	// lampScale: render.lamp_beams (0 = no lamp light on the motes), scale: render.dust_motes (the count scale, > 0), sunY: the unclamped sun height, time: seconds (the drift), viewH: the target height in pixels. Returns the motes drawn (0 = none).
	int render(const Camera& c, const LightParams& light, const Shadow& shadow, float scale, float lampScale, float sunY, double time, sg_view depth);

private:
	sg_shader shd_ = {};
	sg_pipeline pip_ = {};
	sg_sampler smp_ = {};
};
