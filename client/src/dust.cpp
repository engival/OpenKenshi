#include "dust.hpp"

#include <algorithm>
#include <cmath>

#include "drawstats.hpp"
#include "dust.glsl.h"
#include "lightvol.hpp"

namespace
{
// Tuned by eye (kenshi-jj3.8.1): the lab's 60000 motes of 0.05 units filled a hall; a Kenshi room is a few units across, so the count follows the box volume.
constexpr float kDensity = 2.0f;	// motes per cubic unit at render.dust_motes 1
constexpr int kMaxMotes = 60000;
constexpr float kRadius = 0.06f;	// world units
constexpr float kGain = 15.0f;		// the lab's 30 clipped white against the sky seen through a door (90 more so); half of it keeps the sun-lit motes below the clip
constexpr float kG = 0.6f;			// Henyey-Greenstein g of the lab's dust
constexpr float kLampGain = 0.35f;	// sunshafts.cpp kLampGain: the lamps' cone gain at render.lamp_beams 1
constexpr float kNearFade = 0.5f;
}

void Dust::init()
{
	shd_ = sg_make_shader(dust_shader_desc(sg_query_backend()));
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
	pd.colors[0].blend.src_factor_rgb = pd.colors[0].blend.src_factor_alpha = SG_BLENDFACTOR_ONE;	// alpha 0: additive
	pd.colors[0].blend.dst_factor_rgb = pd.colors[0].blend.dst_factor_alpha = SG_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
	pd.depth.pixel_format = SG_PIXELFORMAT_NONE;
	pd.sample_count = 1;
	pd.label = "dust";
	pip_ = sg_make_pipeline(&pd);
}

void Dust::shutdown()
{
	sg_destroy_pipeline(pip_);
	sg_destroy_sampler(smp_);
	sg_destroy_shader(shd_);
	pip_ = {};
	smp_ = {};
	shd_ = {};
}

int Dust::render(const Camera& c, const LightParams& light, const Shadow& shadow, float scale, float lampScale, float sunY, double time, sg_view depth)
{
	const lightvol::Binding& lv = lightvol::binding();
	const bool sunLit = shadow.active() && sunY > 0.0f;	// the sun lights a mote only where its beam reaches (kenshi-jj3.8.11); the lamps' cones are the other light
	if (scale <= 0.0f || lv.params[0] < 0.5f || (!sunLit && lampScale <= 0.0f)) { return 0; }
	// the nearest live volume (lightvol.hpp: nearest first): its probe box, back in camera relative space: g = A rel + off, so rel = A^-1 (g - off)
	const float* v = lv.params + 8;
	if (v[24 + 1] <= 0.0f) { return 0; }	// a volume whose light has not arrived
	double A[3][3], off[3], top[3];
	for (int i = 0; i < 3; i++)
	{
		for (int j = 0; j < 3; j++) { A[i][j] = v[i * 4 + j]; }
		off[i] = v[i * 4 + 3];
		top[i] = v[12 + i * 4 + 3] - 1.0;
	}
	const double det = A[0][0] * (A[1][1] * A[2][2] - A[1][2] * A[2][1]) - A[0][1] * (A[1][0] * A[2][2] - A[1][2] * A[2][0]) + A[0][2] * (A[1][0] * A[2][1] - A[1][1] * A[2][0]);
	if (std::fabs(det) < 1e-12) { return 0; }
	double inv[3][3];
	inv[0][0] = (A[1][1] * A[2][2] - A[1][2] * A[2][1]) / det;
	inv[0][1] = (A[0][2] * A[2][1] - A[0][1] * A[2][2]) / det;
	inv[0][2] = (A[0][1] * A[1][2] - A[0][2] * A[1][1]) / det;
	inv[1][0] = (A[1][2] * A[2][0] - A[1][0] * A[2][2]) / det;
	inv[1][1] = (A[0][0] * A[2][2] - A[0][2] * A[2][0]) / det;
	inv[1][2] = (A[0][2] * A[1][0] - A[0][0] * A[1][2]) / det;
	inv[2][0] = (A[1][0] * A[2][1] - A[1][1] * A[2][0]) / det;
	inv[2][1] = (A[0][1] * A[2][0] - A[0][0] * A[2][1]) / det;
	inv[2][2] = (A[0][0] * A[1][1] - A[0][1] * A[1][0]) / det;
	const double vol = std::fabs(top[0] * top[1] * top[2] / det);	// world volume of the box
	const int count = (int)std::min<double>(kMaxMotes, vol * kDensity * scale);
	if (count <= 0) { return 0; }
	dust_vs_params_t p = {};
	for (int k = 0; k < 3; k++)
	{
		p.cam_f[k] = (float)c.f[k];
		p.cam_r[k] = (float)c.r[k];
		p.cam_u[k] = (float)c.u[k];
		double o = 0;
		for (int j = 0; j < 3; j++) { o -= inv[k][j] * off[j]; }
		p.box_o[k] = (float)o;
		p.box_x[k] = (float)(inv[k][0] * top[0]);
		p.box_y[k] = (float)(inv[k][1] * top[1]);
		p.box_z[k] = (float)(inv[k][2] * top[2]);
	}
	p.cam_f[3] = (float)std::fmod(time, 3600.0);
	p.cam_r[3] = 1.0f / (c.tanY * c.aspect);
	p.cam_u[3] = 1.0f / c.tanY;
	p.box_o[3] = kRadius;
	p.box_x[3] = (float)c.height / (2.0f * c.tanY);
	p.box_y[3] = kGain;
	p.box_z[3] = kNearFade;
	dust_fs_params_t fp = {};
	fp.zinfo[0] = c.nearZ;
	fp.zinfo[1] = c.farZ;
#if defined(SOKOL_VULKAN)
	fp.zinfo[2] = 1;
#endif
	fp.zinfo[3] = kG;
	fp.dinfo[0] = sunLit ? 1.0f : 0.0f;
	fp.dinfo[1] = lampScale * kLampGain;
	drawstats::pipeline(pip_);
	sg_bindings bd = {};
	bd.views[VIEW_depth_tex] = depth;
	bd.samplers[SMP_depth_smp] = smp_;
	bd.views[VIEW_shadow_tex] = shadow.view();
	bd.samplers[SMP_shadow_smp] = shadow.sampler();
	bd.views[VIEW_vol_use] = lv.use;
	bd.samplers[SMP_vol_smp] = lv.smp;
	drawstats::bindings(&bd);
	sg_apply_uniforms(UB_light_params, { &light, sizeof light });
	sg_apply_uniforms(UB_shadow_params, { &shadow.params(), sizeof(ShadowParams) });
	sg_apply_uniforms(UB_light_volumes, { lv.params, sizeof lv.params });
	sg_apply_uniforms(UB_dust_vs_params, SG_RANGE(p));
	sg_apply_uniforms(UB_dust_fs_params, SG_RANGE(fp));
	drawstats::draw(0, 6, count);
	return count;
}
