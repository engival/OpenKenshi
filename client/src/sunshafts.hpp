#pragma once
#include "camera.hpp"
#include "shadow.hpp"
#include "sky.hpp"
#include "sokol_gfx.h"

// Sun shafts (render.sun_shafts, kenshi-jj3.7.1; shaders/sunshafts.glsl): beams of sunlight in the air of the baked buildings (lightvol.hpp), a full-screen ray march against the shadow
// atlas, blended over the HDR scene after the main scene pass. Call render() inside a pass that keeps the scene colour (Hdr::fogPass) while lightvol::Volumes are still live.
class SunShafts
{
public:
	void init();
	void shutdown();
	// scale: render.sun_shafts (the density scale, > 0), sunY: the unclamped sun height. Draws nothing without live volumes, with the shadows off or the sun below the horizon.
	// debug: render.sun_shafts_debug (kenshi-jj3.7.3; 0 off) - 1 = the lit fraction of the marched path, 2 = the sun visibility at the scene surface, 3 = the scene with the
	// sun forced lit, 4 = the enclosure along the path. Returns true when it drew.
	// lampScale: render.lamp_beams (kenshi-jj3.28.1; 0 = off): the cones of the shaded lamps in `light` (the scene's list with its visibility codes) ride the same march; the pass then
	// runs without a sun (scale 0, or night / shadows off) as well.
	bool render(const Camera& c, const LightParams& light, const Shadow& shadow, float scale, float lampScale, float sunY, sg_view depth, int debug = 0);

private:
	sg_shader shd_ = {};
	sg_pipeline pip_ = {};
	sg_sampler smp_ = {};
};
