#pragma once
#include <string>

#include "sokol_gfx.h"

// HDR auto exposure (docs/reference/LIGHTING.md section 10). The world is drawn into an RGBA16F target; a luminance reduction (128^2 -> 1x1) and a 1x1 adapted-luminance
// target (ping-pong, R32F) live on the GPU, and the composite pass samples the 1x1 texture while writing the swapchain: no CPU readback in the interactive path.
// The world shaders still bake an exposure estimate (Sky::exposure(), `bakedExposure`); the composite multiplies by game exposure / baked, so the estimate only has to be
// stable, not right. The overlay (HUD, text, minimap) is drawn after the composite and is never exposed.
class Hdr
{
public:
	struct Frame
	{
		float bakedExposure = 1;	// the exposure the world shaders multiplied in this frame
		float minLum = 0.8f, maxLum = 1.2f;	// MIN_LUMINANCE (Sky), MAX_LUMINANCE
		float floorLum = -1;		// post.eye_adapt (experimental): >= 0 lowers the adapted-luminance floor to min(minLum, this) ...
		float eyeExp = 1;		// ... and compensates below minLum with this exponent (1 = the plain chain)
		float eyeKnee = 0.3f;		// ... below this scene luminance (above it the original's clamp holds)
		float key = 0.55f;		// EXPOSURE_KEY
		float dt = 1.0f / 60;		// real seconds
		float rate = 0.5f;		// AUTOEXP_ADAPTATION_RATE
		bool instant = false;		// adapted = current this frame (first frame, scripted runs)
		float fixedExposure = 0;	// > 0: use this multiplier, ignore the luminance (--exposure N)
		// Heat haze (the game's HeatHaze compositor node, post/heathaze.hlsl): the composite wobbles the scene uv. 0 or no textures = off.
		float heatHaze = 0;		// SharedSkyParams heatHaze (the weather's `heat haze`, blended)
		float gameTime = 0;		// SharedSkyParams gameTime
		float nearZ = 1, farZ = 1;	// the scene projection, to linearise the depth
		float farClip = 1;		// the game's camera far clip: haze depth = distance / farClip
		float tanY = 1, aspect = 1;	// tan(fovY / 2), width / height
	};

	// `swapchain` = sglue_environment().defaults before kclient overrides the colour default to RGBA16F for the scene pipelines.
	void init(const sg_environment_defaults& swapchain);
	// The heat haze textures (data/materials/FlowHAZE.dds, Perturber.dds); without them the haze stays off.
	void loadHaze(const std::string& dataDir);
	void shutdown();

	// Begins nothing: returns the pass to begin for the scene (RGBA16F colour + depth, cleared to black). Recreates the targets when the size changed.
	sg_pass scenePass(int w, int h, bool* resized = nullptr);
	// The fog volume pass (fogvolume.hpp): the scene colour kept (LOAD), no depth attachment, because the scene depth is sampled instead. Call after the scene pass ended.
	sg_pass fogPass() const;
	// The weather particle pass (particles.hpp): the same as fogPass (scene colour kept, no depth attachment), after the fog volumes.
	sg_pass particlePass() const;
	// The interior mask (the game's rt_interiormask): its own RG16F target, resized with the scene, cleared to 0 (no hull: nothing clips), no depth.
	// Render it once per view (WorldBuildings::renderMask with that view's mvp); the particle pass of the same view samples maskTex().
	sg_pass maskPass(int w, int h);
	sg_view maskTex() const { return mask_.tex; }
	sg_view depthTex() const { return depthTex_; }
	// The peeked-glass layer (WorldBuildings::renderGlass) writes its nearest surface into the scene depth so its blend lands on one even layer; the glass is see-through,
	// so that depth must not reach the passes that sample the scene depth (particles, fog volumes, AO, heat haze: the bolt behind a peeked roof was dropped by it,
	// kenshi-k16.2.11.1). While glass draws, the opaque depth is copied to an R32F target first (copyOpaqueDepth, after the scene pass ended), the glass continues in
	// continuePass(), and sceneDepthTex() returns the copy; a frame without glass reads the depth itself. clearOpaqueDepth() at the start of the scene pass.
	void copyOpaqueDepth();
	// The water's own copy (water.glsl: the opacity depth is the G-buffer distance behind the surface, water.hlsl:155): the opaque depth in R32F, taken before the water draws.
	// Does not mark the copy as the scene depth (the foliage fade band still writes depth after the water); copyOpaqueDepth overwrites it later.
	void copyDepthForWater();
	sg_view opaqueCopyTex() const { return opaque_.tex; }
	sg_pass continuePass() const;
	void clearOpaqueDepth() { opaqueValid_ = false; }
	sg_view sceneDepthTex() const { return opaqueValid_ ? opaque_.tex : depthTex_; }
	// Dev (KCLIENT_DEV_TOOLS `probe depth`): the image the particle FS samples, for a GL readback (raw window depth; `colour` = the R32F
	// opaque copy, read as a red channel instead of GL_DEPTH_COMPONENT).
	sg_image sceneDepthImage(bool* colour) const { if (colour) { *colour = opaqueValid_; } return opaqueValid_ ? opaque_.image : depth_; }
	sg_view depthAtt() const { return depthAtt_; }	// the scene depth as an attachment (the cursor ID pass loads it)
	sg_view sceneTex() const { return scene_.tex; }	// the scene colour (exposed by Frame::bakedExposure), after the scene pass
	// Luminance chain and adaptation (own passes). Call after the scene pass ended.
	void resolve(const Frame& f);
	// Draws the exposed scene; call inside the swapchain pass, before the overlay. toPost: into post.hpp's RGBA16F input target instead (no depth, one sample).
	void composite(const Frame& f, bool toPost = false);

	// Diagnostics for scripted runs ONLY (GL readback: stalls). Mean scene luminance of the last resolve, the adapted luminance, both in game HDR units.
	void read(float* sceneLum, float* adapted) const;

	// A colour render target with a texture view (also post.hpp's).
	struct Target
	{
		sg_image image = {};
		sg_view tex = {}, att = {};
	};
	static Target makeColour(int w, int h, sg_pixel_format fmt, const char* label);
	static void destroy(Target& t);

private:
	void makeTargets(int w, int h);
	void destroyTargets();

	static constexpr int kLevels = 5;
	static constexpr int kSize[kLevels] = { 128, 64, 16, 4, 1 };

	int w_ = 0, h_ = 0;
	Target scene_ = {}, mask_ = {}, opaque_ = {}, lum_[kLevels] = {}, adapt_[2] = {};
	bool opaqueValid_ = false;
	sg_image depth_ = {};
	sg_view depthAtt_ = {}, depthTex_ = {};
	sg_sampler linear_ = {}, point_ = {}, clamp_ = {};
	sg_image hazeImg_[2] = {};	// FlowHAZE, Perturber
	sg_view hazeTex_[2] = {};
	sg_shader shd_[5] = {};
	sg_pipeline lumPip_ = {}, downPip_ = {}, adaptPip_ = {}, compPip_ = {}, compPostPip_ = {}, opaquePip_ = {};
	int cur_ = 0;	// adapt_[cur_] holds the latest adapted luminance
};
