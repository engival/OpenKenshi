#pragma once
#include "render/camera.hpp"
#include "sokol_gfx.h"

// The water's planar reflection (the game's Water_Reflection compositor node, main.compositor:166-178; kenshi-inspect findings/water_reflection_rt.md,
// docs/reference/WATER.md section 4): a 512^2 target drawn from the camera mirrored about the water plane y = 100, the plane itself as the near clip.
// The world draws into it with LightParams::kReflectPass set, so a surface writes the YCoCg luma of its unlit albedo as the game's G-buffer pass does;
// the sky writes its own colour. water.glsl multiplies its environment specular by the red channel.
//
// The mirror would flip the triangle winding (a reflection has determinant -1), so the target is drawn by a proper camera (right = -mirrored right):
// its image is the reflection flipped left to right, and water.glsl samples it at 1 - u. Every pipeline keeps its cull mode.
// The target matches the scene pass (RGBA16F, DEPTH_STENCIL, one sample), so the renderers draw into it with their own pipelines.
class Reflection
{
public:
	static constexpr int kSize = 512;
	static constexpr double kPlane = 100.0;

	void init();
	void shutdown();

	// The mirrored view for this frame; false (nothing to draw) with the camera at or below the plane.
	bool setup(const Camera& cam);
	// projection x view of the mirrored camera for renderers that draw relative to the main camera's position (the offset is folded in)
	const float* mvp() const { return mvp_; }
	const Camera& camera() const { return cam_; }	// the mirrored camera (the sky's rays)
	sg_pass pass() const;
	sg_view view() const { return tex_; }
	sg_sampler sampler() const { return smp_; }

private:
	Camera cam_ = {};
	float mvp_[16] = {};
	sg_image colour_ = {}, depth_ = {};
	sg_view tex_ = {}, att_ = {}, depthAtt_ = {};
	sg_sampler smp_ = {};
};
