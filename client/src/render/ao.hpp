#pragma once
#include "render/camera.hpp"
#include "render/hdr.hpp"

// Ambient occlusion (post.ao) and indirect light (post.indirect, docs/CLIENT_GOALS.md "Post effects"; ours, the game has none). After the scene pass, ao.glsl turns the scene depth into a half-res AO
// texture; the next frame's world shaders read it reprojected through the previous camera (lighting.inc light_ao) and darken only the ambient light. Kept per
// eye (a camera of its own each), so a stereo mode can run one per eye. Anything drawn outside bind() .. compute() (portraits, the creator) gets a white 1x1: no AO.

// What the lit programs bind (lighting.inc lighting_ao): VIEW_ao_tex, VIEW_il_tex, SMP_ao_smp and UB_ao_params from here, next to spec_cube_binding().
struct AoBinding
{
	sg_view view = {};
	sg_view il = {};
	sg_sampler smp = {};
	float params[24] = {};	// ao_params: prev_vp[16], delta[4] (w = 1: on), par[4] (x = strength, yz = texture size, w = indirect strength)
};
const AoBinding& ao_binding();

class Ao
{
public:
	void init();
	void shutdown();
	// Before the scene pass: the world reads this eye's AO and indirect light of the previous frame (none when both strengths are 0 or the previous frame
	// computed none).
	void bind(const Camera& cam, float strength, float indirect, int eye = 0);
	// After the scene pass: unbinds, then (a strength > 0 at bind) computes this frame's of the eye from the scene depth, and the indirect light from the
	// scene colour (bakedExposure: what the world shaders multiplied in, Hdr::Frame).
	void compute(const Camera& cam, sg_view depthTex, sg_view colourTex, float bakedExposure);

private:
	static constexpr int kEyes = 2;
	struct Eye
	{
		Hdr::Target out = {};	// RG16F: r = AO, g = view depth
		Hdr::Target il = {};	// RGBA16F: rgb = indirect light
		int w = 0, h = 0;
		float vp[16] = {};	// the camera-relative projection x view it was computed with
		double pos[3] = {};	// and that camera's position
		bool valid = false;	// computed in the previous frame
	};
	Eye eyes_[kEyes];
	Hdr::Target raw_ = {}, tmp_ = {}, rawIl_ = {}, tmpIl_ = {};	// ao_sample's outputs and the half-blurred ones, shared by the eyes
	int w_ = 0, h_ = 0;
	sg_image white_ = {};
	sg_view whiteTex_ = {};
	sg_sampler point_ = {};
	sg_shader shd_[2] = {};
	sg_pipeline samplePip_ = {}, blurPip_ = {};
	float strength_ = 0, indirect_ = 0;	// of the bind() before the compute()
	int eye_ = 0;
};
