#include "sunshafts.hpp"

#include "drawstats.hpp"
#include "lightvol.hpp"
#include "sunshafts.glsl.h"

namespace
{
// Scattering per world unit in open air and extra indoors, the Henyey-Greenstein g, the march steps per volume the ray crosses. kOpen and kG are the lab's (kenshi-lab stormview
// 074dc80 / its v2 look --phase 0.3). kIndoor is not: the lab's 0.0022 was tuned in a hall whose rays cross it for hundreds of units, and a Kenshi room's air is a few units deep
// (kenshi-jj3.7.3: 0.0022 gave the room an optical depth of ~0.02, a beam of ~1e-3 HDR that no exposure could show). 0.06 (an optical depth of ~0.5 per room) lit a dark
// wall facing the sun from 52 to 231/255 and veiled the doorway white; the lead picked half of it, 0.03, as the look of render.sun_shafts 1 (jj3.7.3).
// The lamp cone gain at render.lamp_beams 1 (kenshi-jj3.28.1): the lamps' colour is an exposed radiance, far brighter than the sun's in-scatter per unit, so it is a factor below 1 chosen by look (1.0 hazes the whole room, 0.4 is hardly visible: kenshi-jj3.28.1 look test):
// at render.lamp_beams 1 the cone reads clearly fainter than render.sun_shafts 1.
constexpr float kLampGain = 0.35f;
constexpr float kOpen = 0.00004f, kIndoor = 0.03f, kG = 0.3f, kSteps = 48.0f;
}

void SunShafts::init()
{
	shd_ = sg_make_shader(sunshafts_shader_desc(sg_query_backend()));
	sg_sampler_desc sd = {};
	sd.min_filter = sd.mag_filter = SG_FILTER_NEAREST;
	sd.wrap_u = sd.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
	smp_ = sg_make_sampler(&sd);
	sg_pipeline_desc pd = {};
	pd.shader = shd_;
	pd.cull_mode = SG_CULLMODE_NONE;
	pd.color_count = 1;
	pd.colors[0].pixel_format = SG_PIXELFORMAT_RGBA16F;
	pd.colors[0].blend.enabled = true;
	pd.colors[0].blend.src_factor_rgb = pd.colors[0].blend.src_factor_alpha = SG_BLENDFACTOR_ONE;	// premultiplied
	pd.colors[0].blend.dst_factor_rgb = pd.colors[0].blend.dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
	pd.depth.pixel_format = SG_PIXELFORMAT_NONE;
	pd.sample_count = 1;
	pd.label = "sunshafts";
	pip_ = sg_make_pipeline(&pd);
}

void SunShafts::shutdown()
{
	sg_destroy_pipeline(pip_);
	sg_destroy_sampler(smp_);
	sg_destroy_shader(shd_);
	pip_ = {};
	smp_ = {};
	shd_ = {};
}

bool SunShafts::render(const Camera& c, const LightParams& light, const Shadow& shadow, float scale, float lampScale, float sunY, sg_view depth, int debug)
{
	const lightvol::Binding& lv = lightvol::binding();
	const bool sun = scale > 0.0f && shadow.active() && sunY > 0.0f;	// below the horizon: night
	if (lv.params[0] < 0.5f || (!sun && lampScale <= 0.0f)) { return false; }
	drawstats::pipeline(pip_);
	sg_bindings bd = {};
	bd.views[VIEW_depth_tex] = depth;
	bd.samplers[SMP_depth_smp] = smp_;
	bd.views[VIEW_shadow_tex] = shadow.view();
	bd.samplers[SMP_shadow_smp] = shadow.sampler();	bd.views[VIEW_vol_use] = lv.use;
	bd.views[VIEW_vol_lamp] = lv.lamp;	// lamp_vis (render.lamp_beams)
	bd.samplers[SMP_vol_smp] = lv.smp;
	drawstats::bindings(&bd);
	shaft_params_t p = {};
	for (int k = 0; k < 3; k++)
	{
		p.cam_f[k] = (float)c.f[k];
		p.cam_r[k] = (float)(c.r[k] * c.tanY * c.aspect);
		p.cam_u[k] = (float)(c.u[k] * c.tanY);
	}
	p.zinfo[0] = c.nearZ;
	p.zinfo[1] = c.farZ;
#if defined(SOKOL_VULKAN) || defined(SOKOL_METAL)	// z clip 0..1 (GL: -1..1)
	p.zinfo[2] = 1;
#endif
	const float dens = scale > 0.0f ? scale : 1.0f;	// the lamp cones use the default haze when the sun's is off
	p.shaft[0] = kOpen * dens;
	p.shaft[1] = kIndoor * dens;
	p.shaft[2] = kG;
	p.shaft[3] = kSteps;
	p.dbg[0] = (float)debug;
	p.dbg[1] = lampScale * kLampGain;
	p.dbg[2] = sun ? 1.0f : 0.0f;
	sg_apply_uniforms(UB_light_params, { &light, sizeof light });
	sg_apply_uniforms(UB_shadow_params, { &shadow.params(), sizeof(ShadowParams) });
	sg_apply_uniforms(UB_light_volumes, { lv.params, sizeof lv.params });
	sg_apply_uniforms(UB_shaft_params, SG_RANGE(p));
	drawstats::draw(0, 3, 1);
	return true;
}
