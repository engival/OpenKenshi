#include "post.hpp"
#include "drawstats.hpp"
#include "dev/lightterms.hpp"

#include <algorithm>
#include <cmath>

#include "config.hpp"
#include "post.glsl.h"
#include "smaa_lut.hpp"
#include "sokol_app.h"

namespace
{
	const cfg::Bool kDither{ cfg::Group{ "Colour" }, "post.dither", false, "Output dither: one step of noise before the 8-bit output, removes banding in sky gradients and fog" };
	const cfg::Float kShoulder{ cfg::Group{ "Exposure" }, "post.shoulder", 0, 0, 1, "Highlight roll-off: bright areas fade softly into white instead of clipping (0 = the original's hard clip)" };
	const cfg::Float kBlackPoint{ cfg::Group{ "Exposure" }, "post.black_point", 0, 0, 0.2, "Black point: deepens the darkest tones (0 = off)" };
	const cfg::Float kVibrance{ cfg::Group{ "Colour" }, "post.vibrance", 0, -1, 1, "Vibrance: more colour in the dull areas, saturated ones left alone (0 = off, negative = less)" };
	const cfg::Float kWarmth{ cfg::Group{ "Colour" }, "post.warmth", 0, -1, 1, "Warmth: warm highlights and cool shadows (0 = off, negative = the reverse)" };
	const cfg::Text kAa{ cfg::Group{ "Sharpness" }, "post.aa", "off", { "off", "smaa" }, "Anti-aliasing of the 3D view: smaa = SMAA 1x (smooths jagged edges; the UI is never touched)" };
	const cfg::Float kSharpen{ cfg::Group{ "Sharpness" }, "post.sharpen", 0, 0, 1, "Sharpening (AMD CAS): crisper detail without halos (0 = off)" };
	const cfg::Float kBloom{ cfg::Group{ "Effects" }, "post.bloom", 0, 0, 1, "Bloom: a soft glow around the brightest highlights (sun, lamps, glare off sand); 0 = off, as in the original" };
	const cfg::Float kClarity{ cfg::Group{ "Effects" }, "post.clarity", 0, 0, 1, "Clarity: more local contrast in the mid tones (rock, sand, cloth detail); shadows, highlights and skylines left alone (0 = off)" };
	const cfg::Float kVignette{ cfg::Group{ "Effects" }, "post.vignette", 0, 0, 1, "Vignette: darkens the corners of the 3D view (0 = off)" };
	const cfg::Float kLetterbox{ cfg::Group{ "Effects" }, "post.letterbox", 0, 0, 4, "Letterbox: black bars over the 3D view down to this aspect ratio, e.g. 2.39 (0 = off; hides world under the HUD)" };
	const cfg::Bool kDof{ cfg::Group{ "Effects" }, "post.dof", false, "Depth of field: the distance behind what the camera looks at goes soft; the subject and everything in front stay sharp (free camera: none)" };
	const cfg::Float kOutline{ cfg::Group{ "Effects" }, "post.outline", 0, 0, 1, "Outlines: ink lines along silhouettes and creases, a drawn look (0 = off)" };
	const cfg::Float kPosterize{ cfg::Group{ "Colour" }, "post.posterize", 0, 0, 32, "Posterize: brightness in this many soft bands, colours kept, a cel-shaded look (0 = off)" };
	const cfg::Float kIndirect{ cfg::Group{ "Ambient light" }, "post.indirect", 0, 0, 1, "Indirect light: sunlit walls and ground cast a little of their colour into nearby shade (0 = off, as in the original)" };
	const cfg::Float kSkyAmbient{ cfg::Group{ "Ambient light" }, "post.sky_ambient", 0, 0, 1, "Sky ambient: shade takes its light from the sky you see and the ground's bounce (blue at noon, warm at dusk, sand red under overhangs) instead of the original's fixed light (0 = off)" };
	const cfg::Float kMoonlight{ cfg::Group{ "Ambient light", "needs post.sky_ambient" }, "post.moonlight", 0.5, 0, 1, "Moonlight: with sky ambient on, the least light the night sky gives shade; 1 = the original's night, 0 = lamps only (nothing while sky ambient is off)" };
	const cfg::Float kAo{ cfg::Group{ "Ambient light" }, "post.ao", 0, 0, 1, "Ambient occlusion: contact shadows in corners, under ledges and around feet; the sky light only, sunlight untouched (0 = off)" };
}

void Post::init(const sg_environment_defaults& swapchain)
{
	sg_sampler_desc sd = {};
	sd.min_filter = sd.mag_filter = SG_FILTER_NEAREST;
	sd.wrap_u = sd.wrap_v = SG_WRAP_CLAMP_TO_EDGE;
	point_ = sg_make_sampler(&sd);
	sd.min_filter = sd.mag_filter = SG_FILTER_LINEAR;
	linear_ = sg_make_sampler(&sd);
	const sg_backend be = sg_query_backend();
	shd_[0] = sg_make_shader(post_grade_shader_desc(be));
	shd_[1] = sg_make_shader(smaa_edge_shader_desc(be));
	shd_[2] = sg_make_shader(smaa_weight_shader_desc(be));
	shd_[3] = sg_make_shader(smaa_blend_shader_desc(be));
	shd_[4] = sg_make_shader(post_final_shader_desc(be));
	shd_[5] = sg_make_shader(bloom_pre_shader_desc(be));
	shd_[6] = sg_make_shader(bloom_down_shader_desc(be));
	shd_[7] = sg_make_shader(bloom_up_shader_desc(be));
	shd_[8] = sg_make_shader(scene_comp_shader_desc(be));
	shd_[9] = sg_make_shader(clarity_down_shader_desc(be));
	shd_[10] = sg_make_shader(clarity_blur_shader_desc(be));
	shd_[11] = sg_make_shader(dof_down_shader_desc(be));
	shd_[12] = sg_make_shader(dof_blur_shader_desc(be));
	shd_[13] = sg_make_shader(dof_comp_shader_desc(be));
	shd_[14] = sg_make_shader(stylize_shader_desc(be));
	auto pipeline = [](sg_shader shd, sg_pixel_format fmt, const char* label, bool add = false)
	{
		sg_pipeline_desc pd = {};
		pd.shader = shd;
		pd.cull_mode = SG_CULLMODE_NONE;
		pd.colors[0].pixel_format = fmt;
		if (add)
		{
			pd.colors[0].blend.enabled = true;
			pd.colors[0].blend.src_factor_rgb = pd.colors[0].blend.dst_factor_rgb = SG_BLENDFACTOR_ONE;
		}
		pd.depth.pixel_format = SG_PIXELFORMAT_NONE;
		pd.sample_count = 1;
		pd.label = label;
		return sg_make_pipeline(&pd);
	};
	gradePip_ = pipeline(shd_[0], SG_PIXELFORMAT_RGBA16F, "post-grade");
	edgePip_ = pipeline(shd_[1], SG_PIXELFORMAT_RG8, "smaa-edge");
	weightPip_ = pipeline(shd_[2], SG_PIXELFORMAT_RGBA8, "smaa-weight");
	blendPip_ = pipeline(shd_[3], SG_PIXELFORMAT_RGBA16F, "smaa-blend");
	bloomPrePip_ = pipeline(shd_[5], SG_PIXELFORMAT_RGBA16F, "bloom-pre");
	bloomDownPip_ = pipeline(shd_[6], SG_PIXELFORMAT_RGBA16F, "bloom-down");
	bloomUpPip_ = pipeline(shd_[7], SG_PIXELFORMAT_RGBA16F, "bloom-up", true);
	compPip_ = pipeline(shd_[8], SG_PIXELFORMAT_RGBA16F, "scene-comp");
	clarityDownPip_ = pipeline(shd_[9], SG_PIXELFORMAT_R16F, "clarity-down");
	clarityBlurPip_ = pipeline(shd_[10], SG_PIXELFORMAT_R16F, "clarity-blur");
	dofDownPip_ = pipeline(shd_[11], SG_PIXELFORMAT_RGBA16F, "dof-down");
	dofBlurPip_ = pipeline(shd_[12], SG_PIXELFORMAT_RGBA16F, "dof-blur");
	dofCompPip_ = pipeline(shd_[13], SG_PIXELFORMAT_RGBA16F, "dof-comp");
	stylizePip_ = pipeline(shd_[14], SG_PIXELFORMAT_RGBA16F, "stylize");
	sg_pipeline_desc pd = {};
	pd.shader = shd_[4];
	pd.cull_mode = SG_CULLMODE_NONE;
	pd.colors[0].pixel_format = swapchain.color_format;
	pd.depth.pixel_format = swapchain.depth_format;
	pd.depth.compare = SG_COMPAREFUNC_ALWAYS;
	pd.depth.write_enabled = false;
	pd.sample_count = swapchain.sample_count;
	pd.label = "post-final";
	finalPip_ = sg_make_pipeline(&pd);
	lsb_ = swapchain.color_format == SG_PIXELFORMAT_RGB10A2 ? 1.0f / 1023 : 1.0f / 255;
	ao_.init();
}

void Post::shutdown()
{
	ao_.shutdown();
	for (Hdr::Target* t : { &input_, &graded_, &edges_, &weights_, &aa_, &comped_, &base_[0], &base_[1], &dofOut_, &dofHalf_[0], &dofHalf_[1], &styled_ }) { Hdr::destroy(*t); }
	for (Hdr::Target& t : bloom_) { Hdr::destroy(t); }
	sg_destroy_view(areaTex_);
	sg_destroy_view(searchTex_);
	sg_destroy_image(area_);
	sg_destroy_image(search_);
	for (sg_pipeline p : { gradePip_, edgePip_, weightPip_, blendPip_, finalPip_, bloomPrePip_, bloomDownPip_, bloomUpPip_, compPip_, clarityDownPip_, clarityBlurPip_, dofDownPip_, dofBlurPip_, dofCompPip_, stylizePip_ }) { sg_destroy_pipeline(p); }
	for (sg_shader sh : shd_) { sg_destroy_shader(sh); }
	sg_destroy_sampler(point_);
	sg_destroy_sampler(linear_);
}

// The input target always; SMAA's four, the bloom pyramid, clarity's base, DoF's three and stylize's only once they are on (and again on every resize after that).
void Post::makeTargets(int w, int h)
{
	if (w != w_ || h != h_)
	{
		for (Hdr::Target* t : { &input_, &graded_, &edges_, &weights_, &aa_, &comped_, &base_[0], &base_[1], &dofOut_, &dofHalf_[0], &dofHalf_[1], &styled_ }) { Hdr::destroy(*t); }
		for (Hdr::Target& t : bloom_) { Hdr::destroy(t); }
		input_ = Hdr::makeColour(w, h, SG_PIXELFORMAT_RGBA16F, "post-input");
		w_ = w;
		h_ = h;
	}
	if (set_.smaa && !aa_.image.id)
	{
		graded_ = Hdr::makeColour(w, h, SG_PIXELFORMAT_RGBA16F, "post-graded");
		edges_ = Hdr::makeColour(w, h, SG_PIXELFORMAT_RG8, "smaa-edges");
		weights_ = Hdr::makeColour(w, h, SG_PIXELFORMAT_RGBA8, "smaa-weights");
		aa_ = Hdr::makeColour(w, h, SG_PIXELFORMAT_RGBA16F, "smaa-out");
	}
	if (set_.comp() && !comped_.image.id) { comped_ = Hdr::makeColour(w, h, SG_PIXELFORMAT_RGBA16F, "scene-comp"); }
	if (set_.clarity > 0 && !base_[0].image.id)
	{
		baseW_ = std::max(1, (w + 3) / 4);
		baseH_ = std::max(1, (h + 3) / 4);
		for (Hdr::Target& t : base_) { t = Hdr::makeColour(baseW_, baseH_, SG_PIXELFORMAT_R16F, "clarity-base"); }
	}
	if ((set_.outline > 0 || set_.posterize > 0) && !styled_.image.id) { styled_ = Hdr::makeColour(w, h, SG_PIXELFORMAT_RGBA16F, "stylize"); }
	if (set_.dof && !dofOut_.image.id)
	{
		dofOut_ = Hdr::makeColour(w, h, SG_PIXELFORMAT_RGBA16F, "dof-out");
		for (Hdr::Target& t : dofHalf_) { t = Hdr::makeColour((w + 1) / 2, (h + 1) / 2, SG_PIXELFORMAT_RGBA16F, "dof-half"); }
	}
	if (set_.bloom > 0 && !bloom_[0].image.id)
	{
		for (int i = 0; i < kBloomLevels; i++)
		{
			bloomW_[i] = std::max(1, w >> (i + 1));
			bloomH_[i] = std::max(1, h >> (i + 1));
			bloom_[i] = Hdr::makeColour(bloomW_[i], bloomH_[i], SG_PIXELFORMAT_RGBA16F, "bloom-level");
		}
	}
}

// SMAA's AreaTex (RG8) and SearchTex (R8), generated (about 50 ms) the first time SMAA runs. Rows go up as upstream's headers store them: SMAA addresses
// them by texcoord, which every backend maps to the same memory rows.
void Post::makeLuts()
{
	auto make = [](int w, int h, sg_pixel_format fmt, const std::vector<uint8_t>& px, const char* label, sg_view* tex)
	{
		sg_image_desc id = {};
		id.width = w;
		id.height = h;
		id.pixel_format = fmt;
		id.data = { px.data(), px.size() };
		id.label = label;
		const sg_image img = sg_make_image(&id);
		sg_view_desc vd = {};
		vd.texture.image = img;
		*tex = sg_make_view(&vd);
		return img;
	};
	area_ = make(smaa_lut::kAreaW, smaa_lut::kAreaH, SG_PIXELFORMAT_RG8, smaa_lut::area(), "smaa-area", &areaTex_);
	search_ = make(smaa_lut::kSearchW, smaa_lut::kSearchH, SG_PIXELFORMAT_R8, smaa_lut::search(), "smaa-search", &searchTex_);
}

void Post::retireOptions()
{
	// The preset table and the display curves (client/retired/): a cfg file still setting one gets a line at load, and the key is ignored (k16.2.82).
	cfg::retire("post.preset", "post.preset is gone (k16.2.82): the original's display is the only base");
	cfg::retire("post.tone", "post.tone is gone (k16.2.82): the original's display is the only base");
	cfg::retire("post.contrast", "post.contrast is gone (k16.2.82): the original's display is the only base");
}

float Post::skyAmbient()
{
	return (float)kSkyAmbient.get();
}

float Post::moonlight()
{
	return (float)kMoonlight.get();
}

Post::Settings Post::settings() const
{
	Settings s;	// every effect off: the original's display alone (the composite's clip)
	s.dither = kDither.get();
	s.shoulder = (float)kShoulder.get();
	s.blackPoint = (float)kBlackPoint.get();
	s.vibrance = (float)kVibrance.get();
	s.warmth = (float)kWarmth.get();
	s.smaa = kAa.get() == "smaa";
	s.sharpen = (float)kSharpen.get();
	s.ao = (float)kAo.get();
	s.indirect = (float)kIndirect.get();
	s.bloom = (float)kBloom.get();
	s.clarity = (float)kClarity.get();
	s.vignette = (float)kVignette.get();
	s.dof = kDof.get();
	s.outline = (float)kOutline.get();
	s.posterize = (float)kPosterize.get();
	s.letterbox = (float)kLetterbox.get();
	if (dev::lightOff(dbgOff_, dev::kLtBloom)) { s.bloom = 0; }
	if (dev::lightOff(dbgOff_, dev::kLtGrade)) { s.shoulder = s.blackPoint = s.vibrance = s.warmth = s.clarity = 0; }	// the grade: the display clip stays (it is the original's display)
	return s;
}

void Post::beginScene(const Camera& cam, bool world)
{
	const Settings s = settings();
	ao_.bind(cam, world ? s.ao : 0.0f, world ? s.indirect : 0.0f);
	focus_ = world ? cam.focus : 0;
}

void Post::endScene(const Camera& cam, sg_view depthTex, sg_view colourTex, float bakedExposure)
{
	ao_.compute(cam, depthTex, colourTex, bakedExposure);
	depth_ = depthTex;
	nearZ_ = cam.nearZ;
	farZ_ = cam.farZ;
}

void Post::render(Hdr& hdr, const Hdr::Frame& f, int w, int h)
{
	set_ = settings();
	// The applied size (windowgate.hpp, from the packet): while a window change is held the chain keeps the last applied size, whose image the
	// final pass scales onto the window (k16.2.222).
	bar_ = set_.letterbox > 0 ? std::max(0, (int)std::lround(0.5 * (h - w / set_.letterbox))) : 0;	// none when the screen is already as wide
	if (!set_.any()) { return; }
	makeTargets(w, h);
	if (set_.smaa && !area_.id) { makeLuts(); }
	auto pass = [](const Hdr::Target& dst, sg_load_action load, const char* label)
	{
		sg_pass p = {};
		p.action.colors[0].load_action = load;
		p.action.colors[0].clear_value = { 0.0f, 0.0f, 0.0f, 0.0f };
		p.attachments.colors[0] = dst.att;
		p.label = label;
		sg_begin_pass(&p);
	};
	pass(input_, SG_LOADACTION_DONTCARE, "post-input");
	hdr.composite(f, true);
	sg_end_pass();
	if (set_.comp()) { compose(); }
	if (set_.smaa) { aa(); }
	if (dofOn()) { dof(); }
	if (stylizeOn()) { stylize(); }
}

// SMAA 1x into aa_, with the grade into graded_ first.
void Post::aa()
{
	auto pass = [](const Hdr::Target& dst, sg_load_action load, const char* label)
	{
		sg_pass p = {};
		p.action.colors[0].load_action = load;
		p.action.colors[0].clear_value = { 0.0f, 0.0f, 0.0f, 0.0f };
		p.attachments.colors[0] = dst.att;
		p.label = label;
		sg_begin_pass(&p);
	};
	// SMAA 1x after the grade (it must see the colours the player sees): edges (the pass discards edge-free pixels: cleared), blend weights, blending.
	const float g[4] = { set_.shoulder, set_.blackPoint, set_.vibrance, set_.warmth };
	const float rt[4] = { 1.0f / w_, 1.0f / h_, (float)w_, (float)h_ };
	pass(graded_, SG_LOADACTION_DONTCARE, "post-grade");
	drawstats::pipeline(gradePip_);
	sg_bindings bd = {};
	bd.views[VIEW_src_tex] = scene().tex;
	bd.samplers[SMP_smp] = point_;
	drawstats::bindings(&bd);
	sg_apply_uniforms(UB_post_grade_params, SG_RANGE(g));
	drawstats::draw(0, 3, 1);
	sg_end_pass();

	pass(edges_, SG_LOADACTION_CLEAR, "smaa-edge");
	drawstats::pipeline(edgePip_);
	bd = {};
	bd.views[VIEW_color_tex] = graded_.tex;
	bd.samplers[SMP_smaa_pt] = point_;
	drawstats::bindings(&bd);
	sg_apply_uniforms(UB_smaa_vs_params, SG_RANGE(rt));
	drawstats::draw(0, 3, 1);
	sg_end_pass();

	pass(weights_, SG_LOADACTION_CLEAR, "smaa-weight");
	drawstats::pipeline(weightPip_);
	bd = {};
	bd.views[VIEW_edges_tex] = edges_.tex;
	bd.views[VIEW_area_tex] = areaTex_;
	bd.views[VIEW_search_tex] = searchTex_;
	bd.samplers[SMP_smaa_lin] = linear_;
	drawstats::bindings(&bd);
	sg_apply_uniforms(UB_smaa_vs_params, SG_RANGE(rt));
	sg_apply_uniforms(UB_smaa_fs_params, SG_RANGE(rt));
	drawstats::draw(0, 3, 1);
	sg_end_pass();

	pass(aa_, SG_LOADACTION_DONTCARE, "smaa-blend");
	drawstats::pipeline(blendPip_);
	bd = {};
	bd.views[VIEW_color_tex] = graded_.tex;
	bd.views[VIEW_blend_tex] = weights_.tex;
	bd.samplers[SMP_smaa_lin] = linear_;
	drawstats::bindings(&bd);
	sg_apply_uniforms(UB_smaa_vs_params, SG_RANGE(rt));
	sg_apply_uniforms(UB_smaa_fs_params, SG_RANGE(rt));
	drawstats::draw(0, 3, 1);
	sg_end_pass();
}

// The scene the grade reads (post.glsl scene_comp). Bloom (bloom_*): the thresholded scene into half res, down the pyramid, back up adding each level onto
// the next larger; the original adds its (disabled) bloom at the same place: after the exposure, before anything else (Composite_fp4). Clarity
// (clarity_*): the 1/4-res log luminance, blurred x then y. scene_comp then adds the bloom and applies clarity into comped_.
void Post::compose()
{
	auto draw = [&](const Hdr::Target& dst, sg_load_action load, sg_pipeline pip, sg_view src, sg_sampler smp, int ub, const float u[4], const char* label)
	{
		sg_pass p = {};
		p.action.colors[0].load_action = load;
		p.attachments.colors[0] = dst.att;
		p.label = label;
		sg_begin_pass(&p);
		drawstats::pipeline(pip);
		sg_bindings bd = {};
		bd.views[VIEW_src_tex] = src;
		bd.samplers[SMP_smp] = smp;
		drawstats::bindings(&bd);
		sg_apply_uniforms(ub, { u, 4 * sizeof(float) });
		drawstats::draw(0, 3, 1);
		sg_end_pass();
	};
	if (set_.bloom > 0)
	{
		const float pre[4] = { 1.0f / w_, 1.0f / h_, 1.0f, 0.5f };	// threshold: display white; knee 0.5
		draw(bloom_[0], SG_LOADACTION_DONTCARE, bloomPrePip_, input_.tex, linear_, UB_bloom_params, pre, "bloom-pre");
		for (int i = 1; i < kBloomLevels; i++)
		{
			const float b[4] = { 1.0f / bloomW_[i - 1], 1.0f / bloomH_[i - 1], 0, 0 };
			draw(bloom_[i], SG_LOADACTION_DONTCARE, bloomDownPip_, bloom_[i - 1].tex, linear_, UB_bloom_params, b, "bloom-down");
		}
		for (int i = kBloomLevels - 1; i > 0; i--)
		{
			const float b[4] = { 1.0f / bloomW_[i - 1], 1.0f / bloomH_[i - 1], 0, 0 };	// half a texel of the source level
			draw(bloom_[i - 1], SG_LOADACTION_LOAD, bloomUpPip_, bloom_[i].tex, linear_, UB_bloom_params, b, "bloom-up");
		}
	}
	if (set_.clarity > 0)
	{
		const float down[4] = { 1.0f / w_, 1.0f / h_, 0, 0 };
		const float bx[4] = { 1.0f / baseW_, 0, 0, 0 }, by[4] = { 0, 1.0f / baseH_, 0, 0 };
		draw(base_[0], SG_LOADACTION_DONTCARE, clarityDownPip_, input_.tex, linear_, UB_clarity_params, down, "clarity-down");
		draw(base_[1], SG_LOADACTION_DONTCARE, clarityBlurPip_, base_[0].tex, point_, UB_clarity_params, bx, "clarity-blur-x");
		draw(base_[0], SG_LOADACTION_DONTCARE, clarityBlurPip_, base_[1].tex, point_, UB_clarity_params, by, "clarity-blur-y");
	}
	sg_pass p = {};
	p.action.colors[0].load_action = SG_LOADACTION_DONTCARE;
	p.attachments.colors[0] = comped_.att;
	p.label = "scene-comp";
	sg_begin_pass(&p);
	drawstats::pipeline(compPip_);
	sg_bindings bd = {};
	bd.views[VIEW_src_tex] = input_.tex;
	bd.views[VIEW_bloom_tex] = set_.bloom > 0 ? bloom_[0].tex : input_.tex;	// unread when off (strength 0)
	bd.views[VIEW_base_tex] = set_.clarity > 0 ? base_[0].tex : input_.tex;
	bd.samplers[SMP_smp] = point_;
	bd.samplers[SMP_lin] = linear_;
	drawstats::bindings(&bd);
	const float b[4] = { set_.bloom, set_.clarity, (float)baseW_, (float)baseH_ };	// the pyramid sums its levels: 1 = each level of glow at full strength
	sg_apply_uniforms(UB_comp_params, SG_RANGE(b));
	drawstats::draw(0, 3, 1);
	sg_end_pass();
}

void Post::present(Hdr& hdr, const Hdr::Frame& f)
{
	if (!set_.any())
	{
		hdr.composite(f);
		return;
	}
	drawstats::pipeline(finalPip_);
	sg_bindings bd = {};
	bd.views[VIEW_src_tex] = chainOut().tex;
	bd.samplers[SMP_smp] = point_;
	drawstats::bindings(&bd);
	const float p[12] = { set_.dither ? lsb_ : 0.0f, (float)(frame_++ & 0xffffff), set_.vignette, (float)bar_, set_.shoulder, set_.blackPoint, set_.vibrance, set_.warmth,
		set_.sharpen, set_.smaa ? 1.0f : 0.0f, 1.0f / w_, 1.0f / h_ };
	sg_apply_uniforms(UB_post_final_params, SG_RANGE(p));
	drawstats::draw(0, 3, 1);
}

// Depth of field (post.glsl dof_*) on what post_final would read: the input with its CoC at half res, the gather blur, the full-res mix into dofOut_.
void Post::dof()
{
	const Hdr::Target& src = set_.smaa ? aa_ : scene();
	const int hw = (w_ + 1) / 2, hh = (h_ + 1) / 2;
#if defined(SOKOL_VULKAN)
	const float clip01 = 1;
#else
	const float clip01 = 0;
#endif
	const float radius = 0.006f * h_;	// half-res pixels at full blur: 0.012 of the screen height
	const float u[12] = { nearZ_, farZ_, clip01, focus_, radius, 1.0f / hw, 1.0f / hh, 1.0f / radius, 1.0f / w_, 1.0f / h_, 0, 0 };
	auto pass = [](const Hdr::Target& dst, sg_pipeline pip, const char* label)
	{
		sg_pass p = {};
		p.action.colors[0].load_action = SG_LOADACTION_DONTCARE;
		p.attachments.colors[0] = dst.att;
		p.label = label;
		sg_begin_pass(&p);
		drawstats::pipeline(pip);
	};
	pass(dofHalf_[0], dofDownPip_, "dof-down");
	sg_bindings bd = {};
	bd.views[VIEW_src_tex] = src.tex;
	bd.views[VIEW_dof_depth] = depth_;
	bd.samplers[SMP_smp] = linear_;
	bd.samplers[SMP_dof_pt] = point_;
	drawstats::bindings(&bd);
	sg_apply_uniforms(UB_dof_params, SG_RANGE(u));
	drawstats::draw(0, 3, 1);
	sg_end_pass();

	pass(dofHalf_[1], dofBlurPip_, "dof-blur");
	bd = {};
	bd.views[VIEW_src_tex] = dofHalf_[0].tex;
	bd.samplers[SMP_smp] = point_;
	drawstats::bindings(&bd);
	sg_apply_uniforms(UB_dof_params, SG_RANGE(u));
	drawstats::draw(0, 3, 1);
	sg_end_pass();

	pass(dofOut_, dofCompPip_, "dof-comp");
	bd = {};
	bd.views[VIEW_src_tex] = src.tex;
	bd.views[VIEW_dof_depth] = depth_;
	bd.views[VIEW_blur_tex] = dofHalf_[1].tex;
	bd.samplers[SMP_smp] = point_;
	bd.samplers[SMP_dof_pt] = point_;
	drawstats::bindings(&bd);
	sg_apply_uniforms(UB_dof_params, SG_RANGE(u));
	drawstats::draw(0, 3, 1);
	sg_end_pass();
}

// Outline and posterize (post.glsl stylize) on the chain so far, into styled_.
void Post::stylize()
{
	const Hdr::Target& src = dofOn() ? dofOut_ : set_.smaa ? aa_ : scene();
#if defined(SOKOL_VULKAN)
	const float clip01 = 1;
#else
	const float clip01 = 0;
#endif
	sg_pass p = {};
	p.action.colors[0].load_action = SG_LOADACTION_DONTCARE;
	p.attachments.colors[0] = styled_.att;
	p.label = "stylize";
	sg_begin_pass(&p);
	drawstats::pipeline(stylizePip_);
	sg_bindings bd = {};
	bd.views[VIEW_src_tex] = src.tex;
	bd.views[VIEW_dof_depth] = depth_;
	bd.samplers[SMP_smp] = point_;
	bd.samplers[SMP_dof_pt] = point_;
	drawstats::bindings(&bd);
	const float u[8] = { nearZ_, farZ_, clip01, 0, set_.outline, std::round(set_.posterize), 1.0f / w_, 1.0f / h_ };
	sg_apply_uniforms(UB_stylize_params, SG_RANGE(u));
	drawstats::draw(0, 3, 1);
	sg_end_pass();
}
