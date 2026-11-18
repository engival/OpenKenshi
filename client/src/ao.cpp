#include "ao.hpp"
#include "drawstats.hpp"

#include <cstring>

#include "ao.glsl.h"

namespace
{
	AoBinding g_binding;	// what the lit programs bind now
	AoBinding g_none;	// the white 1x1, ao_params all 0 (off)

	constexpr float kRadius = 15;	// world units (10 per metre): the reach of the occlusion
	constexpr float kRange = 2500;	// view depth where the AO has faded out
}

const AoBinding& ao_binding() { return g_binding; }

void Ao::init()
{
	sg_sampler_desc sd = {};
	sd.min_filter = sd.mag_filter = SG_FILTER_NEAREST;
	sd.wrap_u = sd.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
	point_ = sg_make_sampler(&sd);
	const uint32_t white = 0xffffffff;
	sg_image_desc id = {};
	id.width = id.height = 1;
	id.pixel_format = SG_PIXELFORMAT_RGBA8;
	id.data = SG_RANGE(white);
	id.label = "ao-none";
	white_ = sg_make_image(&id);
	sg_view_desc vd = {};
	vd.texture.image = white_;
	whiteTex_ = sg_make_view(&vd);
	g_none.view = g_none.il = whiteTex_;
	g_none.smp = point_;
	g_binding = g_none;
	const sg_backend be = sg_query_backend();
	shd_[0] = sg_make_shader(ao_sample_shader_desc(be));
	shd_[1] = sg_make_shader(ao_blur_shader_desc(be));
	auto pipeline = [](sg_shader shd, const char* label)
	{
		sg_pipeline_desc pd = {};
		pd.shader = shd;
		pd.cull_mode = SG_CULLMODE_NONE;
		pd.color_count = 2;
		pd.colors[0].pixel_format = SG_PIXELFORMAT_RG16F;
		pd.colors[1].pixel_format = SG_PIXELFORMAT_RGBA16F;
		pd.depth.pixel_format = SG_PIXELFORMAT_NONE;
		pd.sample_count = 1;
		pd.label = label;
		return sg_make_pipeline(&pd);
	};
	samplePip_ = pipeline(shd_[0], "ao-sample");
	blurPip_ = pipeline(shd_[1], "ao-blur");
}

void Ao::shutdown()
{
	for (Eye& e : eyes_) { Hdr::destroy(e.out); Hdr::destroy(e.il); }
	for (Hdr::Target* t : { &raw_, &tmp_, &rawIl_, &tmpIl_ }) { Hdr::destroy(*t); }
	sg_destroy_pipeline(samplePip_);
	sg_destroy_pipeline(blurPip_);
	for (sg_shader sh : shd_) { sg_destroy_shader(sh); }
	sg_destroy_view(whiteTex_);
	sg_destroy_image(white_);
	sg_destroy_sampler(point_);
}

void Ao::bind(const Camera& cam, float strength, float indirect, int eye)
{
	Eye& e = eyes_[eye];
	strength_ = strength;
	indirect_ = indirect;
	eye_ = eye;
	const bool use = (strength > 0 || indirect > 0) && e.valid && e.w == (cam.width + 1) / 2 && e.h == (cam.height + 1) / 2;
	e.valid = false;
	g_binding = g_none;
	if (!use) { return; }
	g_binding.view = e.out.tex;
	g_binding.il = e.il.tex;
	memcpy(g_binding.params, e.vp, sizeof e.vp);
	for (int k = 0; k < 3; k++) { g_binding.params[16 + k] = (float)(cam.pos[k] - e.pos[k]); }
	g_binding.params[19] = 1;
	g_binding.params[20] = strength;
	g_binding.params[21] = (float)e.w;
	g_binding.params[22] = (float)e.h;
	g_binding.params[23] = indirect;
}

void Ao::compute(const Camera& cam, sg_view depthTex, sg_view colourTex, float bakedExposure)
{
	g_binding = g_none;
	if (strength_ <= 0 && indirect_ <= 0) { return; }
	Eye& e = eyes_[eye_];
	const int w = (cam.width + 1) / 2, h = (cam.height + 1) / 2;
	if (w != w_ || h != h_)
	{
		for (Hdr::Target* t : { &raw_, &tmp_, &rawIl_, &tmpIl_ }) { Hdr::destroy(*t); }
		raw_ = Hdr::makeColour(w, h, SG_PIXELFORMAT_RG16F, "ao-raw");
		tmp_ = Hdr::makeColour(w, h, SG_PIXELFORMAT_RG16F, "ao-tmp");
		rawIl_ = Hdr::makeColour(w, h, SG_PIXELFORMAT_RGBA16F, "il-raw");
		tmpIl_ = Hdr::makeColour(w, h, SG_PIXELFORMAT_RGBA16F, "il-tmp");
		w_ = w;
		h_ = h;
	}
	if (w != e.w || h != e.h)
	{
		Hdr::destroy(e.out);
		Hdr::destroy(e.il);
		e.out = Hdr::makeColour(w, h, SG_PIXELFORMAT_RG16F, "ao-out");
		e.il = Hdr::makeColour(w, h, SG_PIXELFORMAT_RGBA16F, "il-out");
		e.w = w;
		e.h = h;
	}
	auto pass = [](const Hdr::Target& dst, const Hdr::Target& il, const char* label)
	{
		sg_pass p = {};
		p.action.colors[0].load_action = p.action.colors[1].load_action = SG_LOADACTION_DONTCARE;
		p.attachments.colors[0] = dst.att;
		p.attachments.colors[1] = il.att;
		p.label = label;
		sg_begin_pass(&p);
	};
#if defined(SOKOL_VULKAN) || defined(SOKOL_METAL)	// z clip 0..1 (GL: -1..1)
	const float clip01 = 1;
#else
	const float clip01 = 0;
#endif
	pass(raw_, rawIl_, "ao-sample");
	drawstats::pipeline(samplePip_);
	sg_bindings bd = {};
	bd.views[VIEW_depth_tex] = depthTex;
	bd.views[VIEW_col_tex] = colourTex;
	bd.samplers[SMP_depth_smp] = point_;
	drawstats::bindings(&bd);
	const float sp[16] = { cam.nearZ, cam.farZ, clip01, kRadius, cam.tanY * cam.aspect, cam.tanY, cam.height * 0.5f / cam.tanY, kRange,
		(float)cam.width, (float)cam.height, 1.0f / cam.width, 1.0f / cam.height, 1.0f / (0.96f * bakedExposure), indirect_ > 0 ? 1.0f : 0.0f, 0, 0 };
	sg_apply_uniforms(UB_ao_sample_params, SG_RANGE(sp));
	drawstats::draw(0, 3, 1);
	sg_end_pass();

	auto blur = [&](const Hdr::Target& src, const Hdr::Target& srcIl, const Hdr::Target& dst, const Hdr::Target& dstIl, float dx, float dy, const char* label)
	{
		pass(dst, dstIl, label);
		drawstats::pipeline(blurPip_);
		sg_bindings b = {};
		b.views[VIEW_src_tex] = src.tex;
		b.views[VIEW_il_tex] = srcIl.tex;
		b.samplers[SMP_src_smp] = point_;
		drawstats::bindings(&b);
		const float step[4] = { dx, dy, 0, 0 };
		sg_apply_uniforms(UB_ao_blur_params, SG_RANGE(step));
		drawstats::draw(0, 3, 1);
		sg_end_pass();
	};
	blur(raw_, rawIl_, tmp_, tmpIl_, 1.0f / w, 0, "ao-blur-x");
	blur(tmp_, tmpIl_, e.out, e.il, 0, 1.0f / h, "ao-blur-y");
	cam.mvp(e.vp);
	memcpy(e.pos, cam.pos, sizeof e.pos);
	e.valid = true;
}
