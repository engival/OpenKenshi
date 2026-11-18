#pragma once
#include "render/ao.hpp"
#include "render/hdr.hpp"

// Optional post effects (docs/CLIENT_GOALS.md "Post effects"): our additions, not the game's, between the exposure composite and the overlay. Each effect is
// its own post.* option, all off by default: the original's display (tone clip, EXPOSURE_KEY 0.55, no output encode) is the one base they layer on
// (k16.2.82; the presets and the display curves are retired). With every effect off Post is inactive and the frame is exactly Hdr::composite into the
// swapchain, so the base stays byte-identical. The overlay is never post-processed.
class Post
{
public:
	// `swapchain` as for Hdr::init.
	void init(const sg_environment_defaults& swapchain);
	void shutdown();
	// The retired post.* keys (k16.2.82: post.preset, post.tone, post.contrast): cfg::load() warns once per key a cfg file still sets, and ignores it.
	static void retireOptions();
	// post.sky_ambient (the world lighting's input, not a chain stage: Sky::skyAmbient); 0 = the original's fixed light
	static float skyAmbient();
	static float moonlight();	// post.moonlight: Sky::moonlight

	// Around the scene pass of a camera: before it the world shaders get last frame's AO (post.ao; world = false: none, e.g. the creator), after it this
	// frame's is computed from the scene depth. Portraits and other draws outside the two get none.
	void beginScene(const Camera& cam, bool world);
	void endScene(const Camera& cam, sg_view depthTex, sg_view colourTex, float bakedExposure);

	// After Hdr::resolve, outside any pass: the exposed scene into the post input target, then the chain's passes. Nothing when inactive.
	// w x h: the applied size the frame was built for (the packet's, RENDER_THREAD.md 7.1 rule 10), never the live window.
	void render(Hdr& hdr, const Hdr::Frame& f, int w, int h);
	// Inside the swapchain pass, before the overlay: the chain's output, or Hdr::composite when inactive.
	void present(Hdr& hdr, const Hdr::Frame& f);
	// post.letterbox: the height in framebuffer pixels of each of the two black bars (top and bottom) over the 3D view this frame, 0 = none. Clicks and
	// the cursor inside them must not reach the world.
	int letterboxBar() const { return bar_; }
	void debugOff(uint32_t mask) { dbgOff_ = mask; }	// the light debugger's switched-off terms (dev/lightterms.hpp), from the packet: bloom and the grade read it in settings()

private:
	struct Settings
	{
		bool dither = false, smaa = false;
		float shoulder = 0, blackPoint = 0, vibrance = 0, warmth = 0, sharpen = 0, ao = 0, bloom = 0, clarity = 0, vignette = 0, letterbox = 0, indirect = 0;
		bool dof = false;
		float outline = 0, posterize = 0;
		// the chain after the composite (AO is in the scene, not in it)
		bool any() const { return dither || smaa || shoulder > 0 || blackPoint > 0 || vibrance != 0 || warmth != 0 || sharpen > 0 || bloom > 0 || clarity > 0 || vignette > 0 || letterbox > 0 || dof || outline > 0 || posterize > 0; }
		bool comp() const { return bloom > 0 || clarity > 0; }	// scene_comp runs
	};
	Settings settings() const;
	void makeTargets(int w, int h);
	void makeLuts();
	void compose();
	void aa();
	void dof();
	void stylize();
	bool stylizeOn() const { return (set_.outline > 0 || set_.posterize > 0) && depth_.id; }
	// what post_final reads: the last stage that ran
	const Hdr::Target& chainOut() const { return stylizeOn() ? styled_ : dofOn() ? dofOut_ : set_.smaa ? aa_ : scene(); }
	bool dofOn() const { return set_.dof && focus_ > 0 && depth_.id; }	// the free camera, the creator and the world map have no focus
	const Hdr::Target& scene() const { return set_.comp() ? comped_ : input_; }	// the exposed scene, with bloom and clarity when on

	uint32_t dbgOff_ = 0;
	int w_ = 0, h_ = 0;
	int bar_ = 0;	// letterboxBar()
	Hdr::Target input_ = {};
	Hdr::Target graded_ = {}, edges_ = {}, weights_ = {}, aa_ = {};	// SMAA's, made on first use
	static constexpr int kBloomLevels = 5;	// half res down to 1/32
	Hdr::Target comped_ = {};	// scene_comp's output; made on first use
	Hdr::Target bloom_[kBloomLevels] = {};	// the pyramid; made on first use
	int bloomW_[kBloomLevels] = {}, bloomH_[kBloomLevels] = {};
	Hdr::Target base_[2] = {};	// clarity's 1/4-res log luminance, blurred x into [1], y back into [0]; made on first use
	int baseW_ = 0, baseH_ = 0;
	Hdr::Target styled_ = {};	// stylize's output; made on first use
	Hdr::Target dofHalf_[2] = {}, dofOut_ = {};	// DoF's half-res input + CoC and its blur, and the result; made on first use
	float focus_ = 0, nearZ_ = 0, farZ_ = 0;	// the scene camera's, from beginScene / endScene
	sg_view depth_ = {};	// the scene depth, from endScene
	sg_image area_ = {}, search_ = {};	// SMAA's tables (smaa_lut.hpp), made on first use
	sg_view areaTex_ = {}, searchTex_ = {};
	sg_sampler point_ = {}, linear_ = {};
	sg_shader shd_[15] = {};		// grade, edge, weight, blend, final, bloom pre, down, up, scene comp, clarity down, blur, dof down, blur, comp, stylize
	sg_pipeline gradePip_ = {}, edgePip_ = {}, weightPip_ = {}, blendPip_ = {}, finalPip_ = {};
	sg_pipeline bloomPrePip_ = {}, bloomDownPip_ = {}, bloomUpPip_ = {}, compPip_ = {}, clarityDownPip_ = {}, clarityBlurPip_ = {};
	sg_pipeline dofDownPip_ = {}, dofBlurPip_ = {}, dofCompPip_ = {}, stylizePip_ = {};
	Ao ao_;
	float lsb_ = 1.0f / 255;	// one output step of the swapchain format
	Settings set_;		// this frame's, from render(); any() = render() drew the input target
	uint32_t frame_ = 0;
};
