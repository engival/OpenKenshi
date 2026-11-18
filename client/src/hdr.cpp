#include "hdr.hpp"
#include "drawstats.hpp"

#include <cmath>
#include <cstdio>

#include "config.hpp"
#include "dds.hpp"
#include "hdr.glsl.h"
#include "sokol_app.h"
#if defined(SOKOL_GLCORE)
#include <GL/gl.h>
#endif

constexpr int Hdr::kSize[Hdr::kLevels];

namespace
{
const cfg::Bool kNonFinite{ cfg::Group{ "Logging" }, "debug.nonfinite", false, "Debug: mark NaN / Inf pixels of the scene bright magenta (bright cyan: made by the exposure composite itself)" };
}

Hdr::Target Hdr::makeColour(int w, int h, sg_pixel_format fmt, const char* label)
{
	Target t;
	sg_image_desc id = {};
	id.usage.color_attachment = true;
	id.width = w;
	id.height = h;
	id.pixel_format = fmt;
	id.sample_count = 1;
	id.label = label;
	t.image = sg_make_image(&id);
	sg_view_desc vd = {};
	vd.texture.image = t.image;
	t.tex = sg_make_view(&vd);
	vd = {};
	vd.color_attachment.image = t.image;
	t.att = sg_make_view(&vd);
	return t;
}

void Hdr::destroy(Target& t)
{
	sg_destroy_view(t.tex);
	sg_destroy_view(t.att);
	sg_destroy_image(t.image);
	t = {};
}

void Hdr::init(const sg_environment_defaults& swapchain)
{
	sg_sampler_desc sd = {};
	sd.min_filter = sd.mag_filter = SG_FILTER_LINEAR;
	sd.wrap_u = sd.wrap_v = SG_WRAP_REPEAT;	// the game's g_samLinear / g_samPoint are Wrap
	linear_ = sg_make_sampler(&sd);
	sd.min_filter = sd.mag_filter = SG_FILTER_NEAREST;
	point_ = sg_make_sampler(&sd);
	sd.min_filter = sd.mag_filter = SG_FILTER_LINEAR;
	sd.wrap_u = sd.wrap_v = SG_WRAP_CLAMP_TO_EDGE;	// HeatHaze `base`: bilinear, clamp
	clamp_ = sg_make_sampler(&sd);
	shd_[0] = sg_make_shader(hdr_lum_shader_desc(sg_query_backend()));
	shd_[1] = sg_make_shader(hdr_down_shader_desc(sg_query_backend()));
	shd_[2] = sg_make_shader(hdr_adapt_shader_desc(sg_query_backend()));
	shd_[3] = sg_make_shader(hdr_comp_shader_desc(sg_query_backend()));
	shd_[4] = sg_make_shader(hdr_opaque_shader_desc(sg_query_backend()));
	sg_pipeline_desc pd = {};
	pd.cull_mode = SG_CULLMODE_NONE;
	pd.color_count = 1;
	pd.depth.pixel_format = SG_PIXELFORMAT_NONE;
	pd.sample_count = 1;
	pd.colors[0].pixel_format = SG_PIXELFORMAT_R16F;
	pd.shader = shd_[0];
	pd.label = "hdr-lum";
	lumPip_ = sg_make_pipeline(&pd);
	pd.shader = shd_[1];
	pd.label = "hdr-down";
	downPip_ = sg_make_pipeline(&pd);
	pd.colors[0].pixel_format = SG_PIXELFORMAT_R32F;
	pd.shader = shd_[2];
	pd.label = "hdr-adapt";
	adaptPip_ = sg_make_pipeline(&pd);
	pd.shader = shd_[4];
	pd.label = "hdr-opaque-depth";
	opaquePip_ = sg_make_pipeline(&pd);
	pd.colors[0].pixel_format = SG_PIXELFORMAT_RGBA16F;
	pd.shader = shd_[3];
	pd.label = "hdr-composite-post";
	compPostPip_ = sg_make_pipeline(&pd);
	pd.colors[0].pixel_format = swapchain.color_format;
	pd.depth.pixel_format = swapchain.depth_format;
	pd.sample_count = swapchain.sample_count;
	pd.depth.compare = SG_COMPAREFUNC_ALWAYS;
	pd.depth.write_enabled = false;
	pd.shader = shd_[3];
	pd.label = "hdr-composite";
	compPip_ = sg_make_pipeline(&pd);
	for (int i = 0; i < kLevels; i++) { lum_[i] = makeColour(kSize[i], kSize[i], SG_PIXELFORMAT_R16F, "hdr-lum"); }
	for (Target& t : adapt_) { t = makeColour(1, 1, SG_PIXELFORMAT_R32F, "hdr-adapted"); }
}

void Hdr::destroyTargets()
{
	if (!scene_.image.id) { return; }
	destroy(scene_);
	destroy(mask_);
	destroy(opaque_);
	sg_destroy_view(depthAtt_);
	sg_destroy_view(depthTex_);
	sg_destroy_image(depth_);
}

void Hdr::makeTargets(int w, int h)
{
	destroyTargets();
	w_ = w;
	h_ = h;
	scene_ = makeColour(w, h, SG_PIXELFORMAT_RGBA16F, "hdr-scene");
	opaque_ = makeColour(w, h, SG_PIXELFORMAT_R32F, "opaque-depth");
	mask_ = makeColour(w, h, SG_PIXELFORMAT_RG16F, "interior-mask");	// the game's rt_depth_range (PF_FLOAT16_GR)
	sg_image_desc id = {};
	id.usage.depth_stencil_attachment = true;
	id.width = w;
	id.height = h;
	id.pixel_format = SG_PIXELFORMAT_DEPTH_STENCIL;
	id.sample_count = 1;
	id.label = "hdr-depth";
	depth_ = sg_make_image(&id);
	sg_view_desc vd = {};
	vd.depth_stencil_attachment.image = depth_;
	depthAtt_ = sg_make_view(&vd);
	vd = {};
	vd.texture.image = depth_;
	depthTex_ = sg_make_view(&vd);
}

void Hdr::shutdown()
{
	destroyTargets();
	for (Target& t : lum_) { destroy(t); }
	for (Target& t : adapt_) { destroy(t); }
	sg_destroy_pipeline(lumPip_);
	sg_destroy_pipeline(downPip_);
	sg_destroy_pipeline(adaptPip_);
	sg_destroy_pipeline(compPip_);
	sg_destroy_pipeline(compPostPip_);
	for (sg_shader s : shd_) { sg_destroy_shader(s); }
	sg_destroy_sampler(linear_);
	sg_destroy_sampler(point_);
	sg_destroy_sampler(clamp_);
	for (sg_view v : hazeTex_) { sg_destroy_view(v); }
	for (sg_image i : hazeImg_) { sg_destroy_image(i); }
}

sg_pass Hdr::scenePass(int w, int h, bool* resized)
{
	const bool changed = w != w_ || h != h_;
	if (changed) { makeTargets(w, h); }
	if (resized) { *resized = changed; }
	sg_pass pass = {};
	pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
	pass.action.colors[0].clear_value = { 0.0f, 0.0f, 0.0f, 1.0f };
	pass.attachments.colors[0] = scene_.att;
	pass.attachments.depth_stencil = depthAtt_;
	pass.label = "hdr-scene";
	return pass;
}

void Hdr::copyOpaqueDepth()
{
	copyDepthForWater();
	opaqueValid_ = true;
}

void Hdr::copyDepthForWater()
{
	sg_pass p = {};
	p.action.colors[0].load_action = SG_LOADACTION_DONTCARE;
	p.attachments.colors[0] = opaque_.att;
	p.label = "opaque-depth";
	sg_begin_pass(&p);
	drawstats::pipeline(opaquePip_);
	sg_bindings bd = {};
	bd.views[VIEW_opq_src] = depthTex_;
	bd.samplers[SMP_osmp] = point_;
	drawstats::bindings(&bd);
	drawstats::draw(0, 3, 1);
	sg_end_pass();
}

sg_pass Hdr::continuePass() const
{
	sg_pass pass = {};
	pass.action.colors[0].load_action = SG_LOADACTION_LOAD;
	pass.action.depth.load_action = SG_LOADACTION_LOAD;
	pass.action.stencil.load_action = SG_LOADACTION_LOAD;
	pass.attachments.colors[0] = scene_.att;
	pass.attachments.depth_stencil = depthAtt_;
	pass.label = "hdr-scene-glass";
	return pass;
}

sg_pass Hdr::fogPass() const
{
	sg_pass pass = {};
	pass.action.colors[0].load_action = SG_LOADACTION_LOAD;
	pass.attachments.colors[0] = scene_.att;
	pass.label = "fog-volumes";
	return pass;
}

sg_pass Hdr::particlePass() const
{
	sg_pass pass = fogPass();
	pass.label = "particles";
	return pass;
}

sg_pass Hdr::maskPass(int w, int h)
{
	if (w != w_ || h != h_) { makeTargets(w, h); }
	sg_pass pass = {};
	pass.action.colors[0].load_action = SG_LOADACTION_CLEAR;
	pass.action.colors[0].clear_value = { 0.0f, 0.0f, 0.0f, 0.0f };	// the compositor's clear: no hull at a pixel, nothing clips there
	pass.attachments.colors[0] = mask_.att;
	pass.label = "interior-mask";
	return pass;
}

void Hdr::resolve(const Frame& f)
{
	auto run = [](const Target& dst, sg_pipeline pip, const Target* a, const Target* b, sg_sampler smp, int smpSlot, int ub, const void* params, size_t bytes)
	{
		sg_pass p = {};
		p.action.colors[0].load_action = SG_LOADACTION_DONTCARE;
		p.attachments.colors[0] = dst.att;
		sg_begin_pass(&p);
		drawstats::pipeline(pip);
		sg_bindings bd = {};
		bd.views[0] = a->tex;
		if (b) { bd.views[1] = b->tex; }
		bd.samplers[smpSlot] = smp;
		drawstats::bindings(&bd);
		sg_apply_uniforms(ub, { params, bytes });
		drawstats::draw(0, 3, 1);
		sg_end_pass();
	};
	const float p0[4] = { 1.0f / f.bakedExposure, 1.0f / w_, 1.0f / h_, 0 };
	run(lum_[0], lumPip_, &scene_, nullptr, point_, SMP_smp, UB_hdr_lum_params, p0, sizeof p0);
	for (int i = 1; i < kLevels; i++)
	{
		const float p[4] = { 1.0f / kSize[i - 1], 1.0f / kSize[i - 1], (float)kSize[i], 0 };
		run(lum_[i], downPip_, &lum_[i - 1], nullptr, point_, SMP_smp, UB_hdr_down_params, p, sizeof p);
	}
	const bool eye = f.floorLum >= 0;
	const float ap[8] = { f.dt * f.rate, eye ? std::fmin(f.minLum, f.floorLum) : f.minLum, f.maxLum, f.instant ? 1.0f : 0.0f, f.minLum, eye ? f.eyeExp : 1.0f, std::fmin(f.eyeKnee, f.minLum), 0 };
	const int next = 1 - cur_;
	run(adapt_[next], adaptPip_, &lum_[kLevels - 1], &adapt_[cur_], point_, SMP_psmp, UB_hdr_adapt_params, ap, sizeof ap);
	cur_ = next;
}

void Hdr::loadHaze(const std::string& dataDir)
{
	const char* files[2] = { "FlowHAZE.dds", "Perturber.dds" };
	for (int i = 0; i < 2; i++)
	{
		DdsImage dds;
		std::string err;
		if (!dds_load(dataDir + "/materials/" + files[i], dds, err))
		{
			fprintf(stderr, "kclient: heat haze texture %s unavailable (%s), heat haze off\n", files[i], err.c_str());
			return;
		}
		hazeImg_[i] = dds_make_image(dds, files[i]);
		sg_view_desc vd = {};
		vd.texture.image = hazeImg_[i];
		hazeTex_[i] = sg_make_view(&vd);
	}
}

void Hdr::composite(const Frame& f, bool toPost)
{
	drawstats::pipeline(toPost ? compPostPip_ : compPip_);
	sg_bindings bd = {};
	bd.views[VIEW_scene_tex] = scene_.tex;
	bd.views[VIEW_adapted_tex] = adapt_[cur_].tex;
	const bool haze = f.heatHaze > 0 && hazeTex_[1].id;
	bd.views[VIEW_depth_tex] = sceneDepthTex();
	bd.views[VIEW_flow_tex] = haze ? hazeTex_[0] : adapt_[cur_].tex;	// unread without haze; any texture satisfies the binding
	bd.views[VIEW_pert_tex] = haze ? hazeTex_[1] : adapt_[cur_].tex;
	bd.samplers[SMP_smp] = haze ? clamp_ : point_;
	bd.samplers[SMP_psmp] = point_;
	bd.samplers[SMP_dsmp] = point_;
	bd.samplers[SMP_wsmp] = linear_;
	drawstats::bindings(&bd);
#if defined(SOKOL_VULKAN)
	const bool clip01 = true;
#else
	const bool clip01 = false;
#endif
	const float p[16] = { f.key, 1.0f / f.bakedExposure, f.fixedExposure, haze ? f.heatHaze : 0.0f, f.nearZ, f.farZ, clip01 ? 1.0f : 0.0f, f.farClip,
		f.tanY, f.aspect, f.gameTime, 0, 0.0f, toPost ? 0.0f : 1.0f, kNonFinite.get() ? 1.0f : 0.0f, 0.0f };
	sg_apply_uniforms(UB_hdr_comp_params, SG_RANGE(p));
	drawstats::draw(0, 3, 1);
}

void Hdr::read(float* sceneLum, float* adapted) const
{
	*sceneLum = *adapted = 0;
#if defined(SOKOL_GLCORE)
	auto get = [](const Target& t, float* out)
	{
		glBindTexture(GL_TEXTURE_2D, sg_gl_query_image_info(t.image).tex[0]);
		glGetTexImage(GL_TEXTURE_2D, 0, GL_RED, GL_FLOAT, out);
		glBindTexture(GL_TEXTURE_2D, 0);
	};
	get(lum_[kLevels - 1], sceneLum);
	get(adapt_[cur_], adapted);
	sg_reset_state_cache();
#endif
}
